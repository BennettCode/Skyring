#pragma once

// The Skyrim end of the shared-memory link. C++ mirror of protocol/src/link.rs: keep both in step
// (same steps, same log wording). Depends only on Win32, spdlog and the generated protocol header, so the
// same code runs inside the plugin and inside skyrimxer_link_test.exe.
//
// Per Tick: attach (retry every 1 s) → refresh own heartbeat → check the peer → read events → maybe send a Heartbeat event.
// Not thread-safe: call Tick/Shutdown from one thread at a time.

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include "skyrimxer_protocol.h"

namespace sxer
{
	enum class Side
	{
		Skyrim,
		EldenRing
	};

	enum class PeerStatus
	{
		Waiting,    // no handshake yet since this side attached
		Connected,
		Lost        // was connected, then timed out / faulted / said Bye; waiting for a new peer
	};

	struct Identity
	{
		std::array<std::uint16_t, 3> pluginVersion{};
		std::array<std::uint16_t, 4> gameVersion{};
	};

	// What the game threads need from the link without taking its lock (mirror of link.rs LinkShared).
	// Lives inside the Link: valid while the Link lives. The plugin never destroys its Link (Bridge.cpp), so there it's
	// valid for the whole process.
	struct LinkShared
	{
		// The mapped region once this side joined a matching layout (for SlotWriter/SlotReader); nullptr before that or on a mismatch.
		std::atomic<std::uint8_t*> base{ nullptr };
		// Handshake done and the peer is alive, as of the link's last tick.
		std::atomic<bool> connected{ false };
	};

	// GetTickCount64(): the clock both sides use for heartbeats.
	std::uint64_t NowMs();

	class Link
	{
	public:
		Link(Side a_side, Identity a_identity, std::wstring a_regionName = proto::kRegionName);
		~Link();
		Link(const Link&) = delete;
		Link& operator=(const Link&) = delete;

		// a_nowMs = NowMs() in the game; tests pass their own clock. a_frames = game frames run so far.
		void Tick(std::uint64_t a_nowMs, std::uint64_t a_frames);
		// Clean exit: Bye to a connected peer, then state ShuttingDown. a_log = false during process exit
		// (the logger may already be gone). Safe to call more than once.
		void Shutdown(bool a_log = true);

		PeerStatus Status() const { return status_; }
		bool Connected() const { return status_ == PeerStatus::Connected; }
		const LinkShared& Shared() const { return shared_; }

	private:
		struct Ring
		{
			std::uint8_t* header = nullptr;
			std::uint8_t* data = nullptr;
			std::uint32_t seq = 0;  // writer only
		};
		enum class Level
		{
			Info,
			Warn,
			Error
		};

		bool TryOpen(std::uint64_t a_nowMs);
		bool TryJoin(std::uint64_t a_nowMs);
		void CheckPeer(std::uint64_t a_nowMs);
		void Pump(std::uint64_t a_nowMs);
		void Handle(std::uint64_t a_nowMs, std::uint16_t a_type, std::uint32_t a_seq, const std::uint8_t* a_payload, std::size_t a_size);
		void SendHeartbeatEvent(std::uint64_t a_nowMs, std::uint64_t a_frames);
		bool SendHello();
		bool Push(proto::MsgType a_type, const void* a_payload, std::size_t a_size);
		template <class T>
		bool Send(const T& a_payload)
		{
			return Push(proto::MsgTypeOf<T>::value, &a_payload, sizeof(T));
		}
		std::uint32_t Dropped() const;
		std::uint64_t SkipAll();
		void SetState(proto::SideState a_state);
		void Close();

		void Say(Level a_level, const std::string& a_message);
		void Problem(Level a_level, const std::string& a_message);

		Side side_;
		Identity identity_;
		LinkShared shared_;
		std::wstring regionName_;
		void* mapping_ = nullptr;
		std::uint8_t* base_ = nullptr;
		bool joined_ = false;
		Ring tx_;
		Ring rx_;
		std::uint64_t retryAtMs_ = 0;
		std::string lastProblem_;
		std::uint64_t joinedAtMs_ = 0;
		proto::SideState state_ = proto::SideState::Absent;
		PeerStatus status_ = PeerStatus::Waiting;
		bool peerAlive_ = false;
		std::uint32_t peerAttach_ = 0;
		std::uint32_t peerSeq_ = 0;
		std::uint64_t peerSeenAtMs_ = 0;
		bool helloMissingWarned_ = false;
		std::uint64_t nextBeatEventMs_ = 0;
	};
}
