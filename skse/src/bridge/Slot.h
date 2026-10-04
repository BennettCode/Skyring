#pragma once

// Seqlock slots: latest-value state lanes between the game threads (SLOT_* blocks in the schema).
// C++ mirror of protocol/src/slot.rs: keep the two in step.
//
// One writer, any number of readers. The struct starts with `seq` (odd = being written; protogen checks this).
// - Writer: seq+1 (relaxed), release fence, copy the body, seq+2 (release).
// - Reader: seq (acquire), retry while odd, copy the body, acquire fence, accept if seq is unchanged (at most kSlotReadTries).
// The body is copied as u32 words through atomic_ref, so a read racing a write is never a data race; the seq check throws
// away any torn copy. Data is stale when its time_ms is older than kSlotStaleMs (Fresh) or the link isn't connected.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>

#include "skyrimxer_protocol.h"

namespace sxer
{
	// True if slot data written at a_timeMs is recent enough to act on at a_nowMs.
	inline bool Fresh(std::uint64_t a_timeMs, std::uint64_t a_nowMs)
	{
		return a_nowMs <= a_timeMs || a_nowMs - a_timeMs <= proto::kSlotStaleMs;
	}

	namespace detail
	{
		template <class T>
		class Slot
		{
			static_assert(offsetof(T, seq) == 0 && sizeof(T::seq) == 4, "slot struct must start with seq: u32");
			static_assert(sizeof(T) % 4 == 0 && std::is_trivially_copyable_v<T>, "slot struct must be whole u32 words");

		protected:
			static constexpr std::size_t kWords = sizeof(T) / 4;

			// a_base = the mapped region (LinkShared::base), a_offset = proto::kOffSlot*.
			Slot(std::uint8_t* a_base, std::size_t a_offset) :
				words_(reinterpret_cast<std::uint32_t*>(a_base + a_offset))
			{}

			std::atomic_ref<std::uint32_t> Word(std::size_t a_i) const { return std::atomic_ref<std::uint32_t>(words_[a_i]); }

		private:
			std::uint32_t* words_;
		};
	}

	template <class T>
	class SlotWriter : detail::Slot<T>
	{
		using Base = detail::Slot<T>;

	public:
		// There must be only one writer per slot (Skyrim's game thread for InputState, ER's for PlayerState).
		SlotWriter(std::uint8_t* a_base, std::size_t a_offset) :
			Base(a_base, a_offset), seq_(this->Word(0).load(std::memory_order_relaxed))
		{
			if (seq_ % 2 == 1) {
				// A previous writer died mid-write: make it even again so readers stop waiting.
				++seq_;
				this->Word(0).store(seq_, std::memory_order_release);
			}
		}

		// Publishes a_value (its own seq field is ignored).
		void Write(const T& a_value)
		{
			std::uint32_t src[Base::kWords];
			std::memcpy(src, &a_value, sizeof(T));
			this->Word(0).store(seq_ + 1, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);
			for (std::size_t i = 1; i < Base::kWords; ++i) {
				this->Word(i).store(src[i], std::memory_order_relaxed);
			}
			seq_ += 2;
			this->Word(0).store(seq_, std::memory_order_release);
		}

	private:
		std::uint32_t seq_;
	};

	template <class T>
	class SlotReader : detail::Slot<T>
	{
		using Base = detail::Slot<T>;

	public:
		SlotReader(std::uint8_t* a_base, std::size_t a_offset) :
			Base(a_base, a_offset)
		{}

		// The latest complete value, or nullopt if nothing was ever written or every try was torn.
		std::optional<T> Read() const
		{
			std::uint32_t dst[Base::kWords];
			for (std::uint32_t attempt = 0; attempt < proto::kSlotReadTries; ++attempt) {
				const auto s1 = this->Word(0).load(std::memory_order_acquire);
				if (s1 == 0) {
					return std::nullopt;
				}
				if (s1 % 2 == 1) {
					continue;
				}
				for (std::size_t i = 1; i < Base::kWords; ++i) {
					dst[i] = this->Word(i).load(std::memory_order_relaxed);
				}
				std::atomic_thread_fence(std::memory_order_acquire);
				if (this->Word(0).load(std::memory_order_relaxed) == s1) {
					dst[0] = s1;
					T value;
					std::memcpy(&value, dst, sizeof(T));
					return value;
				}
			}
			return std::nullopt;
		}
	};
}
