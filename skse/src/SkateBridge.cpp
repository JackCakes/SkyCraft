#include "SkateBridge.h"

#include <sddl.h>

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

		// Match SkyCraft's main shared-memory ACL. In particular, a Skyrim started
		// elevated must still be openable by the normal-integrity Rust host.
		PSECURITY_DESCRIPTOR SharedWithThisUser()
		{
			std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
			HANDLE       token = nullptr;
			if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
				DWORD size = 0;
				::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
				std::vector<std::uint8_t> buffer(size);
				if (size && ::GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
					LPWSTR sid = nullptr;
					if (::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) {
						sddl += std::wstring(L"(A;;GA;;;") + sid + L")";
						::LocalFree(sid);
					}
				}
				::CloseHandle(token);
			}
			sddl += L"S:(ML;;NW;;;ME)";
			PSECURITY_DESCRIPTOR descriptor = nullptr;
			if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
					sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
				logger::warn("Skate bridge: couldn't build shared-memory ACL ({})", ::GetLastError());
				return nullptr;
			}
			return descriptor;
		}
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
		SECURITY_ATTRIBUTES access{ sizeof(access), SharedWithThisUser(), FALSE };
		mapping_ = ::CreateFileMappingW(
			INVALID_HANDLE_VALUE,
			access.lpSecurityDescriptor ? &access : nullptr,
			PAGE_READWRITE,
			static_cast<DWORD>(size >> 32),
			static_cast<DWORD>(size & 0xFFFFFFFF),
			skateproto::kMappingName);
		const DWORD created = ::GetLastError();
		if (access.lpSecurityDescriptor) {
			::LocalFree(access.lpSecurityDescriptor);
		}
		if (!mapping_) {
			logger::warn("Skate bridge: CreateFileMapping failed ({})", created);
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

		// Present/player updates can stop while Skyrim is minimized or Alt-Tabbed.
		// Keep this process-liveness heartbeat independent of the render/game loop so
		// the external Skate host does not tear down and reconnect just because the
		// player is looking at another window.
		heartbeatThread_ = std::jthread([this](std::stop_token a_stop) {
			while (!a_stop.stop_requested()) {
				Heartbeat();
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
			}
		});

		logger::info("Skate bridge shared memory {} ({} MB, {})", "Local\\SkyCraftSkate_v1", size >> 20, created == ERROR_ALREADY_EXISTS ? "reused" : "created");
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

	bool SkateBridge::WriteCollision(skateproto::ColType a_type, const void* a_payload, std::uint32_t a_bytes)
	{
		if (!base_) {
			return false;
		}

		auto* ring = base_ + skateproto::kOffCollisionRing;
		auto& headRef = *reinterpret_cast<std::uint64_t*>(ring + skateproto::kColRingHeadOff);
		auto& tailRef = *reinterpret_cast<std::uint64_t*>(ring + skateproto::kColRingTailOff);
		auto* data = ring + skateproto::kColRingDataOff;
		constexpr auto size = skateproto::kColRingDataBytes;

		const std::uint64_t msgBytes = (sizeof(skateproto::ColMsgHeader) + a_bytes + 7) & ~7ull;
		if (msgBytes > size / 2) {
			return false;
		}

		auto head = Atomic(headRef).load(std::memory_order_relaxed);
		const auto tail = Atomic(tailRef).load(std::memory_order_acquire);
		auto pos = head % size;
		const auto padBytes = (pos + msgBytes > size) ? size - pos : 0;
		if (size - (head - tail) < msgBytes + padBytes) {
			return false;
		}

		if (padBytes) {
			*reinterpret_cast<skateproto::ColMsgHeader*>(data + pos) = { skateproto::kColPad, 0 };
			head += padBytes;
			pos = 0;
		}

		*reinterpret_cast<skateproto::ColMsgHeader*>(data + pos) = { a_type, a_bytes };
		if (a_bytes) {
			std::memcpy(data + pos + sizeof(skateproto::ColMsgHeader), a_payload, a_bytes);
		}
		Atomic(headRef).store(head + msgBytes, std::memory_order_release);
		return true;
	}

	void SkateBridge::ClearCollision(std::uint32_t a_epoch)
	{
		// The bridge is optional. Never let an absent host back-pressure SkyCraft's
		// existing Minecraft collision worker.
		if (!Valid() || !HostAlive()) {
			return;
		}
		if (!WriteCollision(skateproto::kColClear, &a_epoch, sizeof(a_epoch))) {
			logger::warn("Skate bridge: collision ring full; dropped clear epoch {}", a_epoch);
		}
	}

	void SkateBridge::WriteRegion(
		std::int32_t a_rx,
		std::int32_t a_ry,
		std::int32_t a_rz,
		std::uint32_t a_epoch,
		const proto::ColTri* a_tris,
		std::uint32_t a_count)
	{
		// The bridge is optional. Until a host is actually consuming the ring,
		// drop Skate copies immediately rather than filling the ring and sleeping.
		if (!Valid() || !HostAlive()) {
			return;
		}

		skateproto::ColRegion header{};
		header.rx = a_rx;
		header.ry = a_ry;
		header.rz = a_rz;
		header.count = a_count;
		header.epoch = a_epoch;
		header.worldId = worldId_.load(std::memory_order_acquire);

		std::vector<std::uint8_t> payload(sizeof(header) + std::size_t(a_count) * sizeof(skateproto::ColTri));
		std::memcpy(payload.data(), &header, sizeof(header));
		if (a_count) {
			static_assert(sizeof(proto::ColTri) == sizeof(skateproto::ColTri));
			std::memcpy(
				payload.data() + sizeof(header),
				a_tris,
				std::size_t(a_count) * sizeof(skateproto::ColTri));
		}

		if (!WriteCollision(
				skateproto::kColRegionTris,
				payload.data(),
				static_cast<std::uint32_t>(payload.size()))) {
			// Optional consumer: never block the existing SkyCraft collision worker.
			// Nearby regions refresh naturally, and a restarted host gets a fresh epoch.
			static std::atomic<std::uint32_t> dropped{ 0 };
			const auto n = dropped.fetch_add(1, std::memory_order_relaxed) + 1;
			if (n <= 5 || n % 100 == 0) {
				logger::warn(
					"Skate bridge: collision ring full; dropped region ({},{},{}) epoch {} ({} total)",
					a_rx,
					a_ry,
					a_rz,
					a_epoch,
					n);
			}
		}
	}
}
