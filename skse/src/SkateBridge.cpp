#include "SkateBridge.h"

namespace skycraft
{
	namespace
	{
		template <class T>
		std::atomic_ref<T> Atomic(T& a_value)
		{
			return std::atomic_ref<T>(a_value);
		}

		constexpr std::uint64_t kHostTimeoutMs = 3000;
	}

	SkateBridge& SkateBridge::Get()
	{
		static SkateBridge bridge;
		return bridge;
	}

	bool SkateBridge::Create()
	{
		if (base_) {
			return true;
		}

		const auto size = skateproto::kMappingBytes;
		mapping_ = ::CreateFileMappingW(
			INVALID_HANDLE_VALUE,
			nullptr,
			PAGE_READWRITE,
			static_cast<DWORD>(size >> 32),
			static_cast<DWORD>(size & 0xFFFFFFFF),
			skateproto::kMappingName);
		if (!mapping_) {
			logger::warn("Skate bridge: CreateFileMapping failed ({})", ::GetLastError());
			return false;
		}

		base_ = static_cast<std::uint8_t*>(::MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
		if (!base_) {
			logger::warn("Skate bridge: MapViewOfFile failed ({})", ::GetLastError());
			::CloseHandle(mapping_);
			mapping_ = nullptr;
			return false;
		}

		std::memset(base_, 0, size);
		auto* header = At<skateproto::Header>(skateproto::kOffHeader);
		header->version = skateproto::kVersion;
		header->skyrimPid = ::GetCurrentProcessId();
		header->mappingBytes = size;
		header->skyrimHeartbeatMs = ::GetTickCount64();
		Atomic(header->magic).store(skateproto::kMagic, std::memory_order_release);

		logger::info("Skate bridge shared memory created");
		return true;
	}

	bool SkateBridge::HostAlive() const
	{
		if (!base_) {
			return false;
		}
		auto& beat = At<skateproto::Header>(skateproto::kOffHeader)->hostHeartbeatMs;
		const auto last = Atomic(beat).load(std::memory_order_acquire);
		return last != 0 && ::GetTickCount64() - last < kHostTimeoutMs;
	}

	std::uint32_t SkateBridge::HostPid() const
	{
		if (!base_) {
			return 0;
		}
		return Atomic(At<skateproto::Header>(skateproto::kOffHeader)->hostPid).load(std::memory_order_acquire);
	}

	void SkateBridge::Heartbeat()
	{
		if (!base_) {
			return;
		}
		Atomic(At<skateproto::Header>(skateproto::kOffHeader)->skyrimHeartbeatMs)
			.store(::GetTickCount64(), std::memory_order_release);
	}

	void SkateBridge::WriteSkyState(const skateproto::SkyState& a_state)
	{
		if (!base_) {
			return;
		}
		auto* dst = At<skateproto::SkyState>(skateproto::kOffSkyState);
		auto seq = Atomic(dst->seq);
		const auto s = seq.load(std::memory_order_relaxed);
		seq.store(s + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		std::memcpy(
			reinterpret_cast<std::uint8_t*>(dst) + 4,
			reinterpret_cast<const std::uint8_t*>(&a_state) + 4,
			sizeof(skateproto::SkyState) - 4);
		seq.store(s + 2, std::memory_order_release);
	}

	bool SkateBridge::ReadSkateState(skateproto::SkateState& a_out) const
	{
		if (!base_) {
			return false;
		}
		auto* src = At<skateproto::SkateState>(skateproto::kOffSkateState);
		auto seq = Atomic(src->seq);
		for (int attempt = 0; attempt < 32; ++attempt) {
			const auto s1 = seq.load(std::memory_order_acquire);
			if (s1 & 1) {
				_mm_pause();
				continue;
			}
			std::memcpy(&a_out, src, sizeof(a_out));
			std::atomic_thread_fence(std::memory_order_acquire);
			if (seq.load(std::memory_order_relaxed) == s1) {
				return true;
			}
		}
		return false;
	}

	bool SkateBridge::WriteCollision(skateproto::ColType, const void*, std::uint32_t)
	{
		return false;
	}

	void SkateBridge::ClearCollision(std::uint32_t)
	{}

	void SkateBridge::WriteRegion(
		std::int32_t,
		std::int32_t,
		std::int32_t,
		std::uint32_t,
		const proto::ColTri*,
		std::uint32_t)
	{}
}
