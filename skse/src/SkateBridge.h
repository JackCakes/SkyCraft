#pragma once

#include "Link.h"
#include "skycraft_skate_protocol.h"

#include <thread>

namespace skycraft
{
	class SkateBridge
	{
	public:
		static SkateBridge& Get();

		bool Create();
		bool Valid() const { return base_ != nullptr; }
		bool HostAlive() const;
		std::uint32_t HostPid() const;
		void Heartbeat();

		void WriteSkyState(const skateproto::SkyState& a_state);
		bool ReadSkateState(skateproto::SkateState& a_out) const;
		void SetWorld(std::uint32_t a_worldId) { worldId_.store(a_worldId, std::memory_order_release); }

		void ClearCollision(std::uint32_t a_epoch);
		void WriteRegion(
			std::int32_t a_rx,
			std::int32_t a_ry,
			std::int32_t a_rz,
			std::uint32_t a_epoch,
			const proto::ColTri* a_tris,
			std::uint32_t a_count);

	private:
		template <class T>
		T* At(std::uint64_t a_off) const
		{
			return reinterpret_cast<T*>(base_ + a_off);
		}

		bool WriteCollision(skateproto::ColType a_type, const void* a_payload, std::uint32_t a_bytes);

		HANDLE mapping_{ nullptr };
		std::uint8_t* base_{ nullptr };
		std::atomic<std::uint32_t> worldId_{ 0 };
		std::jthread heartbeatThread_{};
	};
}
