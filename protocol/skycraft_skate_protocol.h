// SkyCraft Skate bridge shared-memory protocol.
//
// This is mirrored in JackCakes/SkyCraft-SkateBridge:
// crates/protocol/src/lib.rs
//
// Coordinates are SkyCraft MC-space: blocks, Y up, Z south.
#pragma once

#include <cstdint>

namespace skycraft::skateproto
{
	inline constexpr std::uint32_t kMagic = 0x4B534353;  // bytes: "SCSK"
	inline constexpr std::uint32_t kVersion = 2;
	inline constexpr wchar_t       kMappingName[] = L"Local\\SkyCraftSkate_v1";

	inline constexpr std::uint64_t kOffHeader = 0x0000;
	inline constexpr std::uint64_t kOffSkyState = 0x0100;
	inline constexpr std::uint64_t kOffSkateState = 0x0200;
	inline constexpr std::uint64_t kOffInputState = 0x0300;
	inline constexpr std::uint64_t kOffCollisionRing = 0x1000;

	inline constexpr std::uint64_t kCollisionRingBytes = 16ull << 20;
	inline constexpr std::uint64_t kColRingHeadOff = 0x00;
	inline constexpr std::uint64_t kColRingTailOff = 0x40;
	inline constexpr std::uint64_t kColRingDataOff = 0x80;
	inline constexpr std::uint64_t kColRingDataBytes = kCollisionRingBytes - kColRingDataOff;
	inline constexpr std::uint64_t kMappingBytes = kOffCollisionRing + kCollisionRingBytes;

	enum SkyFlags : std::uint32_t
	{
		kSkyInGame = 1u << 0,
		kSkyMenuOpen = 1u << 1,
		kSkyLoading = 1u << 2,
	};

	enum Mode : std::uint32_t
	{
		kModeMinecraft = 0,
		kModeSkate = 1,
	};

	enum HostFlags : std::uint32_t
	{
		kHostReady = 1u << 0,
		kHostActive = 1u << 1,
		kHostError = 1u << 2,
		kHostOnGround = 1u << 3,
		kHostGrinding = 1u << 4,
		kHostManual = 1u << 5,
		kHostBail = 1u << 6,
		kHostCameraValid = 1u << 7,
	};

	struct Header
	{
		std::uint32_t magic;
		std::uint32_t version;
		std::uint32_t skyrimPid;
		std::uint32_t hostPid;
		std::uint64_t skyrimHeartbeatMs;
		std::uint64_t hostHeartbeatMs;
		std::uint64_t mappingBytes;
		std::uint64_t reserved[3];
	};
	static_assert(sizeof(Header) == 64);

	struct SkyState
	{
		std::uint32_t seq;
		std::uint32_t flags;
		std::uint32_t worldId;
		std::uint32_t collisionEpoch;
		double        x, y, z;
		float         yaw;
		std::uint32_t viewportW;
		std::uint32_t viewportH;
		float         aspect;
		std::uint32_t requestedMode;
	};
	static_assert(sizeof(SkyState) == 64);

	struct SkateState
	{
		std::uint32_t seq;
		std::uint32_t flags;
		std::uint32_t errorCode;
		std::uint32_t reserved0;
		double        x, y, z;
		float         quat[4];
		float         velocity[3];
		std::uint32_t reserved1;
		double        cameraPos[3];
		float         cameraForward[3];
		float         cameraUp[3];
		float         fovDeg;
		std::uint32_t stateCode;
	};
	static_assert(sizeof(SkateState) == 128);

	enum InputFlags : std::uint32_t
	{
		kInputValid = 1u << 0,
		kInputKeyboardFallback = 1u << 1,
	};

	// XInput-shaped controller snapshot. This mirrors the raw controller shape
	// consumed by the Skate host so the eventual retail-backed Session can use
	// the same transport without another protocol redesign.
	struct InputState
	{
		std::uint32_t seq;
		std::uint32_t flags;
		std::uint16_t buttons;
		std::uint8_t  triggers[2];
		std::int16_t  left[2];
		std::int16_t  right[2];
		std::uint32_t packet;
		float         frameSeconds;
		std::uint32_t reserved[9];
	};
	static_assert(sizeof(InputState) == 64);

	enum ColType : std::uint32_t
	{
		kColPad = 0,
		kColClear = 1,
		kColRegionTris = 2,
	};

	struct ColMsgHeader
	{
		std::uint32_t type;
		std::uint32_t payloadBytes;
	};
	static_assert(sizeof(ColMsgHeader) == 8);

	struct ColRegion
	{
		std::int32_t  rx, ry, rz;
		std::uint32_t count;
		std::uint32_t epoch;
		std::uint32_t worldId;
	};
	static_assert(sizeof(ColRegion) == 24);

	struct ColTri
	{
		float         v[9];
		std::uint32_t flags;
	};
	static_assert(sizeof(ColTri) == 40);
}
