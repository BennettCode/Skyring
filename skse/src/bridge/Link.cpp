#include "bridge/Link.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <format>
#include <vector>

#include <Windows.h>
#include <spdlog/spdlog.h>

namespace sxer
{
	// While the peer is alive but no handshake happened, re-send the Hello this often (link.rs HELLO_RETRY_MS).
	constexpr std::uint64_t kHelloRetryMs = 2000;

	namespace
	{
		using namespace proto;

		constexpr std::size_t kMsgHeader = sizeof(MsgHeader);

		struct SideFields
		{
			std::size_t pid, state, heartbeat, attach;
		};

		SideFields Fields(Side a_side)
		{
			if (a_side == Side::Skyrim) {
				return { kOffHeader + offsetof(Header, sky_pid), kOffHeader + offsetof(Header, sky_state),
					kOffHeader + offsetof(Header, sky_heartbeat_ms), kOffHeader + offsetof(Header, sky_attach_count) };
			}
			return { kOffHeader + offsetof(Header, er_pid), kOffHeader + offsetof(Header, er_state),
				kOffHeader + offsetof(Header, er_heartbeat_ms), kOffHeader + offsetof(Header, er_attach_count) };
		}

		Side Peer(Side a_side) { return a_side == Side::Skyrim ? Side::EldenRing : Side::Skyrim; }
		const char* Name(Side a_side) { return a_side == Side::Skyrim ? "Skyrim" : "ER"; }

		template <class T>
		std::atomic_ref<T> At(std::uint8_t* a_base, std::size_t a_offset)
		{
			return std::atomic_ref<T>(*reinterpret_cast<T*>(a_base + a_offset));
		}

		std::uint64_t TotalLen(std::size_t a_payload)
		{
			return (kMsgHeader + a_payload + kMsgAlign - 1) / kMsgAlign * kMsgAlign;
		}

		void CopyIn(std::uint8_t* a_data, std::uint64_t a_pos, const void* a_src, std::size_t a_len)
		{
			const auto start = static_cast<std::size_t>(a_pos % kRingCapacity);
			const auto first = std::min<std::size_t>(a_len, kRingCapacity - start);
			std::memcpy(a_data + start, a_src, first);
			std::memcpy(a_data, static_cast<const std::uint8_t*>(a_src) + first, a_len - first);
		}

		void CopyOut(const std::uint8_t* a_data, std::uint64_t a_pos, void* a_dst, std::size_t a_len)
		{
			const auto start = static_cast<std::size_t>(a_pos % kRingCapacity);
			const auto first = std::min<std::size_t>(a_len, kRingCapacity - start);
			std::memcpy(a_dst, a_data + start, first);
			std::memcpy(static_cast<std::uint8_t*>(a_dst) + first, a_data, a_len - first);
		}

		std::string Narrow(const std::wstring& a_text)
		{
			std::string out;
			for (const wchar_t c : a_text) {
				out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
			}
			return out;
		}

		template <std::size_t N>
		std::string VersionString(const std::uint16_t (&a_parts)[N])
		{
			std::string out;
			for (std::size_t i = 0; i < N; ++i) {
				out += (i ? "." : "") + std::to_string(a_parts[i]);
			}
			return out;
		}

		const char* StateName(std::uint32_t a_raw)
		{
			switch (static_cast<SideState>(a_raw)) {
			case SideState::Absent: return "Some(Absent)";
			case SideState::Starting: return "Some(Starting)";
			case SideState::Ready: return "Some(Ready)";
			case SideState::Running: return "Some(Running)";
			case SideState::ShuttingDown: return "Some(ShuttingDown)";
			case SideState::Faulted: return "Some(Faulted)";
			default: return "None";
			}
		}
	}

	std::uint64_t NowMs() { return ::GetTickCount64(); }

	std::uint64_t NowUs()
	{
		static const std::uint64_t freq = [] {
			LARGE_INTEGER f{};
			::QueryPerformanceFrequency(&f);
			return static_cast<std::uint64_t>(std::max<LONGLONG>(f.QuadPart, 1));
		}();
		LARGE_INTEGER c{};
		::QueryPerformanceCounter(&c);
		const auto ticks = static_cast<std::uint64_t>(c.QuadPart);
		return ticks / freq * 1'000'000 + ticks % freq * 1'000'000 / freq;  // split: no overflow, same result as Rust's u128 math
	}

	Link::Link(Side a_side, Identity a_identity, std::wstring a_regionName) :
		side_(a_side), identity_(a_identity), regionName_(std::move(a_regionName))
	{}

	Link::~Link()
	{
		Shutdown();
		Close();
	}

	void Link::Close()
	{
		shared_.base.store(nullptr, std::memory_order_release);
		shared_.connected.store(false, std::memory_order_release);
		if (base_) {
			::UnmapViewOfFile(base_);
			base_ = nullptr;
		}
		if (mapping_) {
			::CloseHandle(mapping_);
			mapping_ = nullptr;
		}
	}

	void Link::Say(Level a_level, const std::string& a_message)
	{
		const auto level = a_level == Level::Info ? spdlog::level::info : a_level == Level::Warn ? spdlog::level::warn : spdlog::level::err;
		spdlog::log(level, "[link] {}", a_message);
	}

	void Link::Problem(Level a_level, const std::string& a_message)
	{
		if (a_message != lastProblem_) {
			Say(a_level, a_message);
			lastProblem_ = a_message;
		}
	}

	void Link::SetState(SideState a_state)
	{
		state_ = a_state;
		At<std::uint32_t>(base_, Fields(side_).state).store(static_cast<std::uint32_t>(a_state), std::memory_order_release);
	}

	void Link::Tick(std::uint64_t a_nowMs, std::uint64_t a_frames)
	{
		if (!base_ && !TryOpen(a_nowMs)) {
			return;
		}
		if (joined_ || TryJoin(a_nowMs)) {
			At<std::uint64_t>(base_, Fields(side_).heartbeat).store(a_nowMs, std::memory_order_release);
			// Peer check first: a new peer is greeted before its own Hello is read, so a Hello never looks like a restart.
			CheckPeer(a_nowMs);
			Pump(a_nowMs);
			SendHeartbeatEvent(a_nowMs, a_frames);
		}
		shared_.connected.store(Connected(), std::memory_order_release);
	}

	void Link::Shutdown(bool a_log)
	{
		if (!base_ || !joined_ || state_ == SideState::ShuttingDown) {
			return;
		}
		const bool sent = Connected() && Send(Bye{ static_cast<std::uint32_t>(ByeReason::Quit), 0 });
		SetState(SideState::ShuttingDown);
		shared_.connected.store(false, std::memory_order_release);
		if (a_log) {
			Say(Level::Info, std::format("shutting down (Bye sent: {})", sent ? "yes" : "no peer"));
		}
	}

	bool Link::TryOpen(std::uint64_t a_nowMs)
	{
		if (a_nowMs < retryAtMs_) {
			return false;
		}
		const auto name = Narrow(regionName_);
		const auto size = static_cast<std::uint64_t>(kRegionSize);
		auto* mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32),
			static_cast<DWORD>(size), regionName_.c_str());
		const DWORD error = ::GetLastError();
		if (!mapping) {
			retryAtMs_ = a_nowMs + 1000;
			Problem(Level::Error, std::format("CreateFileMappingW({}) failed: error {}; retrying every 1 s", name, error));
			return false;
		}
		const bool created = error != ERROR_ALREADY_EXISTS;
		// Map exactly kRegionSize: an existing mapping that is too small fails here instead of faulting later.
		auto* view = static_cast<std::uint8_t*>(::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, kRegionSize));
		if (!view) {
			const DWORD mapError = ::GetLastError();
			::CloseHandle(mapping);
			retryAtMs_ = a_nowMs + 1000;
			Problem(Level::Error, std::format("MapViewOfFile({}, {:#x} bytes) failed: error {}; retrying every 1 s", name, kRegionSize, mapError));
			return false;
		}
		mapping_ = mapping;
		base_ = view;
		if (created) {
			// Zero-filled by Windows. Fill the shared fields, then publish magic last so an opener never sees half a header.
			At<std::uint32_t>(base_, kOffHeader + offsetof(Header, version)).store(kVersion, std::memory_order_relaxed);
			At<std::uint32_t>(base_, kOffHeader + offsetof(Header, header_size)).store(sizeof(Header), std::memory_order_relaxed);
			At<std::uint32_t>(base_, kOffHeader + offsetof(Header, region_size)).store(static_cast<std::uint32_t>(kRegionSize), std::memory_order_relaxed);
			for (const auto ring : { kOffRingSkyToEr, kOffRingErToSky }) {
				At<std::uint32_t>(base_, ring + offsetof(RingHeader, capacity)).store(kRingCapacity, std::memory_order_relaxed);
			}
			At<std::uint32_t>(base_, kOffHeader + offsetof(Header, magic)).store(kMagic, std::memory_order_release);
		}
		const bool sky = side_ == Side::Skyrim;
		tx_ = { base_ + (sky ? kOffRingSkyToEr : kOffRingErToSky), base_ + (sky ? kOffRingSkyToErData : kOffRingErToSkyData) };
		rx_ = { base_ + (sky ? kOffRingErToSky : kOffRingSkyToEr), base_ + (sky ? kOffRingErToSkyData : kOffRingSkyToErData) };
		Say(Level::Info, std::format("region {} {} ({:#x} bytes, protocol v{})", name, created ? "created" : "opened existing", kRegionSize, kVersion));
		return true;
	}

	bool Link::TryJoin(std::uint64_t a_nowMs)
	{
		const auto h = [&](std::size_t a_field) { return At<std::uint32_t>(base_, kOffHeader + a_field).load(std::memory_order_acquire); };
		const auto magic = h(offsetof(Header, magic));
		if (magic == 0) {
			Problem(Level::Info, "waiting for the region creator to finish initialising");
			return false;
		}
		const auto version = h(offsetof(Header, version));
		const auto headerSize = h(offsetof(Header, header_size));
		const auto regionSize = h(offsetof(Header, region_size));
		if (magic != kMagic || version != kVersion || headerSize != sizeof(Header) || regionSize != kRegionSize) {
			state_ = SideState::Faulted;
			Problem(Level::Error, std::format("PROTOCOL MISMATCH: region has magic={:#010x} v{} header={} size={:#x}, "
											  "ours magic={:#010x} v{} header={} size={:#x}. Rebuild both plugins. Staying idle.",
									  magic, version, headerSize, regionSize, kMagic, kVersion, sizeof(Header), kRegionSize));
			return false;
		}

		const auto skipped = SkipAll();
		tx_.seq = 0;
		const auto my = Fields(side_);
		const auto pid = ::GetCurrentProcessId();
		At<std::uint32_t>(base_, my.pid).store(pid, std::memory_order_relaxed);
		At<std::uint64_t>(base_, my.heartbeat).store(a_nowMs, std::memory_order_relaxed);
		const auto attach = At<std::uint32_t>(base_, my.attach).fetch_add(1, std::memory_order_relaxed) + 1;
		joined_ = true;
		// Only a validated layout is handed to the game threads, so a mismatch never gets slot writes.
		shared_.base.store(base_, std::memory_order_release);
		SetState(SideState::Ready);
		joinedAtMs_ = a_nowMs;
		lastProblem_.clear();
		Say(Level::Info, std::format("joined as {} pid={} attach#{} (skipped {} stale bytes); waiting for {}", Name(side_), pid, attach,
							 skipped, Name(Peer(side_))));
		return true;
	}

	std::uint64_t Link::SkipAll()
	{
		const auto w = At<std::uint64_t>(rx_.header, offsetof(RingHeader, write_pos)).load(std::memory_order_acquire);
		const auto r = At<std::uint64_t>(rx_.header, offsetof(RingHeader, read_pos)).exchange(w, std::memory_order_acq_rel);
		return w - r;
	}

	std::uint32_t Link::Dropped() const
	{
		return At<std::uint32_t>(tx_.header, offsetof(RingHeader, dropped)).load(std::memory_order_relaxed);
	}

	bool Link::Push(MsgType a_type, const void* a_payload, std::size_t a_size)
	{
		if (a_size > kMsgMaxPayload) {
			return false;
		}
		const auto total = TotalLen(a_size);
		auto writePos = At<std::uint64_t>(tx_.header, offsetof(RingHeader, write_pos));
		const auto w = writePos.load(std::memory_order_relaxed);
		const auto r = At<std::uint64_t>(tx_.header, offsetof(RingHeader, read_pos)).load(std::memory_order_acquire);
		const auto used = w - r;
		if (used > kRingCapacity || kRingCapacity - used < total) {
			At<std::uint32_t>(tx_.header, offsetof(RingHeader, dropped)).fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		tx_.seq = std::max<std::uint32_t>(tx_.seq + 1, 1);
		const MsgHeader header{ static_cast<std::uint16_t>(a_type), static_cast<std::uint16_t>(a_size), tx_.seq };
		CopyIn(tx_.data, w, &header, kMsgHeader);
		CopyIn(tx_.data, w + kMsgHeader, a_payload, a_size);
		writePos.store(w + total, std::memory_order_release);
		return true;
	}

	bool Link::SendHello()
	{
		Hello hello{};
		hello.protocol_version = kVersion;
		hello.pid = ::GetCurrentProcessId();
		std::copy(identity_.pluginVersion.begin(), identity_.pluginVersion.end(), hello.plugin_version);
		std::copy(identity_.gameVersion.begin(), identity_.gameVersion.end(), hello.game_version);
		return Send(hello);
	}

	void Link::Pump(std::uint64_t a_nowMs)
	{
		std::uint8_t payload[kMsgMaxPayload];
		auto readPos = At<std::uint64_t>(rx_.header, offsetof(RingHeader, read_pos));
		for (;;) {
			const auto r = readPos.load(std::memory_order_relaxed);
			const auto w = At<std::uint64_t>(rx_.header, offsetof(RingHeader, write_pos)).load(std::memory_order_acquire);
			if (w == r) {
				return;
			}
			const auto avail = w - r;
			MsgHeader header{};
			bool ok = avail >= kMsgHeader && avail <= kRingCapacity;
			if (ok) {
				CopyOut(rx_.data, r, &header, kMsgHeader);
				ok = header.size <= kMsgMaxPayload && TotalLen(header.size) <= avail;
			}
			if (!ok) {
				readPos.store(w, std::memory_order_release);
				Say(Level::Error, std::format("ring corrupt: skipped {} bytes", avail));
				continue;
			}
			CopyOut(rx_.data, r + kMsgHeader, payload, header.size);
			readPos.store(r + TotalLen(header.size), std::memory_order_release);

			// Seq restarts at 1 whenever the peer (re)attaches.
			if (header.seq != 1 && peerSeq_ != 0 && header.seq != peerSeq_ + 1) {
				Say(Level::Warn, std::format("seq gap: expected {} got {}", peerSeq_ + 1, header.seq));
			}
			peerSeq_ = header.seq;
			Handle(a_nowMs, header.msg_type, header.seq, payload, header.size);
		}
	}

	void Link::Handle(std::uint64_t a_nowMs, std::uint16_t a_type, std::uint32_t a_seq, const std::uint8_t* a_payload, std::size_t a_size)
	{
		const char* peer = Name(Peer(side_));
		const auto decode = [&]<class T>(T& a_out, const char* a_name) {
			if (a_size < sizeof(T)) {
				Say(Level::Error, std::format("{} seq={} too short ({} bytes)", a_name, a_seq, a_size));
				return false;
			}
			std::memcpy(&a_out, a_payload, sizeof(T));
			return true;
		};

		switch (static_cast<MsgType>(a_type)) {
		case MsgType::Hello:
			{
				Hello hello{};
				if (!decode(hello, "Hello")) {
					return;
				}
				if (hello.protocol_version != kVersion) {
					Say(Level::Error, std::format("PROTOCOL MISMATCH: {} speaks v{}, we speak v{}. Staying idle.", peer, hello.protocol_version, kVersion));
					Send(Bye{ static_cast<std::uint32_t>(ByeReason::Faulted), 0 });
					status_ = PeerStatus::Lost;
					SetState(SideState::Faulted);
					return;
				}
				const auto detail = std::format("{} pid={} plugin v{} game {} protocol v{}", peer, hello.pid, VersionString(hello.plugin_version),
					VersionString(hello.game_version), hello.protocol_version);
				if (status_ == PeerStatus::Connected) {
					// The peer lost us (e.g. we stalled past the timeout) and is reconnecting: greet it back.
					const bool sent = SendHello();
					Say(Level::Info, std::format("Hello again from {} seq={}; replied {}", detail, a_seq, sent ? "Ok" : "Err(Full)"));
				} else {
					status_ = PeerStatus::Connected;
					SetState(SideState::Running);
					nextBeatEventMs_ = a_nowMs + kHeartbeatEventIntervalMs;
					Say(Level::Info, std::format("CONNECTED: handshake ok with {} seq={}", detail, a_seq));
				}
				break;
			}
		case MsgType::Bye:
			{
				Bye bye{};
				if (!decode(bye, "Bye")) {
					return;
				}
				const auto reason = bye.reason == static_cast<std::uint32_t>(ByeReason::Quit)    ? std::string("Quit") :
				                    bye.reason == static_cast<std::uint32_t>(ByeReason::Faulted) ? std::string("Faulted") :
				                                                                                   std::format("unknown({})", bye.reason);
				if (status_ == PeerStatus::Connected) {
					Say(Level::Warn, std::format("LOST: {} said Bye (reason={}) seq={}; going idle", peer, reason, a_seq));
					status_ = PeerStatus::Lost;
					SetState(SideState::Ready);
				} else {
					Say(Level::Info, std::format("{} said Bye (reason={}) seq={}", peer, reason, a_seq));
				}
				break;
			}
		case MsgType::Heartbeat:
			{
				Heartbeat beat{};
				if (!decode(beat, "Heartbeat")) {
					return;
				}
				const auto peerBeat = At<std::uint64_t>(base_, Fields(Peer(side_)).heartbeat).load(std::memory_order_acquire);
				Say(Level::Info, std::format("{} heartbeat seq={} uptime={}ms frames={} beat_age={}ms dropped_by_us={}", peer, a_seq, beat.uptime_ms,
									 beat.frames, a_nowMs > peerBeat ? a_nowMs - peerBeat : 0, Dropped()));
				break;
			}
		default:
			Say(Level::Warn, std::format("unknown message type {} seq={} ({} bytes), ignored", a_type, a_seq, a_size));
			break;
		}
	}

	void Link::CheckPeer(std::uint64_t a_nowMs)
	{
		const char* peer = Name(Peer(side_));
		const auto pf = Fields(Peer(side_));
		const auto rawState = At<std::uint32_t>(base_, pf.state).load(std::memory_order_acquire);
		const auto beat = At<std::uint64_t>(base_, pf.heartbeat).load(std::memory_order_acquire);
		const auto pid = At<std::uint32_t>(base_, pf.pid).load(std::memory_order_relaxed);
		const auto attach = At<std::uint32_t>(base_, pf.attach).load(std::memory_order_relaxed);
		const auto age = a_nowMs > beat ? a_nowMs - beat : 0;
		const auto state = static_cast<SideState>(rawState);
		const bool live = state == SideState::Starting || state == SideState::Ready || state == SideState::Running;
		const bool alive = live && beat != 0 && age <= kHeartbeatTimeoutMs;

		const bool restarted = attach != peerAttach_;
		if (alive && !restarted && !peerAlive_ && status_ == PeerStatus::Connected) {
			// Back after a timeout and already reconnected (its Hello arrived meanwhile): nothing to redo (link.rs, same rule).
			Say(Level::Info, std::format("{} pid={} beats again (attach#{}); still connected", peer, pid, attach));
		} else if (alive && (!peerAlive_ || restarted)) {
			if (status_ == PeerStatus::Connected) {
				Say(Level::Warn, std::format("LOST: {} restarted (attach#{} → #{}); reconnecting", peer, peerAttach_, attach));
				status_ = PeerStatus::Lost;
			}
			if (state_ != SideState::Ready) {
				SetState(SideState::Ready);  // also clears Faulted: a new peer gets a fresh try
			}
			peerAttach_ = attach;
			helloSentAtMs_ = a_nowMs;
			helloRetries_ = 0;
			const bool sent = SendHello();
			Say(Level::Info, std::format("{} detected pid={} attach#{} state={}; Hello sent {}", peer, pid, attach, StateName(rawState),
								 sent ? std::format("Ok({})", tx_.seq) : std::string("Err(Full)")));
		} else if (!alive && peerAlive_ && status_ == PeerStatus::Connected) {
			const auto reason = state == SideState::ShuttingDown ? std::string("it is shutting down (clean exit, Bye follows)") :
			                    state == SideState::Faulted      ? std::string("it faulted") :
			                                                       std::format("heartbeat timeout (last beat {} ms ago, limit {} ms)", age, kHeartbeatTimeoutMs);
			Say(Level::Warn, std::format("LOST: {} pid={}: {}; going idle (fail-safe)", peer, pid, reason));
			status_ = PeerStatus::Lost;
			SetState(SideState::Ready);
		} else if (alive && status_ != PeerStatus::Connected && state_ != SideState::Faulted && a_nowMs - helloSentAtMs_ >= kHelloRetryMs) {
			// No handshake yet although the peer is alive: a Hello can be eaten by a side that connects on it without replying
			// (2026-10-05 deadlock after a 36 s ER stall), so keep re-sending until it answers (link.rs, same rule).
			helloSentAtMs_ = a_nowMs;
			++helloRetries_;
			const bool sent = SendHello();
			if (helloRetries_ == 1 || helloRetries_ % 10 == 0) {
				Say(Level::Warn, std::format("{} is alive but no handshake yet (state={}); Hello re-sent {} (retry {})", peer, StateName(rawState),
									 sent ? std::format("Ok({})", tx_.seq) : std::string("Err(Full)"), helloRetries_));
			}
		}
		peerAlive_ = alive;
	}

	void Link::SendHeartbeatEvent(std::uint64_t a_nowMs, std::uint64_t a_frames)
	{
		if (status_ != PeerStatus::Connected || a_nowMs < nextBeatEventMs_) {
			return;
		}
		nextBeatEventMs_ = a_nowMs + kHeartbeatEventIntervalMs;
		if (!Send(Heartbeat{ a_nowMs - joinedAtMs_, a_frames })) {
			Say(Level::Warn, std::format("Heartbeat event not sent: Full (dropped so far: {})", Dropped()));
		}
	}
}
