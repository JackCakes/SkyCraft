#pragma once

#include "Link.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace skycraft
{
	// Shared runtime state between the per-frame update, input sink and renderer hook.
	struct Runtime
	{
		// Minecraft is connected, in its world, has acknowledged our last teleport, and Skyrim
		// isn't loading: MC's player position drives Skyrim's player.
		std::atomic<bool> puppeting{ false };
		// The same, or waiting for Minecraft to arrive after a teleport: Skyrim's own controls
		// don't move its player either way (it would wander off from where Minecraft is going).
		std::atomic<bool> minecraftOwnsPlayer{ false };
		// A Minecraft GUI screen (inventory, chat, ...) is open: mouse moves MC's cursor.
		std::atomic<bool> mcScreenOpen{ false };
		// A Skyrim menu (journal, dialogue, console, loading, ...) owns input.
		std::atomic<bool> skyrimMenuOpen{ false };
		// The player switched to Skyrim's own controls (the toggle key, Input.cpp): Skyrim moves, looks
		// and fights as if Minecraft weren't there, Minecraft's HUD and blocks are left out of the
		// frame, and Minecraft's player just follows Skyrim's. Set by the toggle (or SkyCraft.ini
		// bStartInSkyrimMode), applied by the per-frame update as a takeover that never ends by itself.
		std::atomic<bool> skyrimMode{ false };
		// Minecraft's world things (placed blocks, mobs, dropped items, arrows, particles, chests, ...) are
		// drawn into Skyrim's frame: while Minecraft drives the player (puppeting), and also in Skyrim's
		// control mode, where Minecraft's player only follows Skyrim's but its world is still there to see.
		// (Not Minecraft's hand, HUD or screens, which are the overlay's and stay off in Skyrim's mode.)
		std::atomic<bool> worldVisible{ false };
		// A Minecraft world reset (the reset key, Input.cpp): 0 none, 1 asked and waiting for Minecraft
		// to say it's on it, 2 Minecraft is throwing its world away and loading a fresh one. The
		// per-frame update (Game.cpp) runs it: Skyrim's own controls until Minecraft has its world back.
		std::atomic<int> worldReset{ 0 };
		// MC reports its player is in a world (overlay should be drawn).
		std::atomic<bool> mcInWorld{ false };

		// Look direction in MC degrees; integrated from raw mouse input (main thread).
		float yaw{ 0.0f };
		float pitch{ 0.0f };
		bool  lookInitialized{ false };
		float sensitivity{ 0.5f };

		// Virtual MC cursor (overlay pixels) while an MC screen is open.
		std::atomic<int> cursorX{ 0 };
		std::atomic<int> cursorY{ 0 };
		std::atomic<int> viewportW{ 1920 };
		std::atomic<int> viewportH{ 1080 };

		// The player's feet as the camera sees them this frame (Minecraft coordinates, Skyrim's
		// own interpolation of Minecraft's ticks). The third-person body is drawn here. Main thread.
		double feetX{ 0.0 }, feetY{ 0.0 }, feetZ{ 0.0 };
		bool   feetValid{ false };

		// Minecraft's crosshair is on screen (first person, no Minecraft screen open) at this GUI
		// scale: the overlay draws it inverted against Skyrim's picture, as Minecraft does.
		std::atomic<bool> mcCrosshair{ false };
		std::atomic<int>  mcGuiScale{ 0 };
	};

	Runtime& State();

	namespace Game
	{
		void Install();
		void OnGameLoaded();
		// A Skyrim menu that takes the mouse or pauses the game is open (checked live: while it
		// pauses the game, the per-frame update that normally tracks it doesn't run).
		bool SkyrimMenuOpen();
		// Called from Present: logs rendered camera vs Minecraft eye when a check is due.
		void CheckRenderedCamera();
		// Third-person diagnostics, from where the blocks are drawn: the camera Skyrim renders this
		// frame, and the order of the frame's steps ('P' player update, 'H' camera update hook,
		// 'D' block drawing, 'X' Present).
		void NoteRenderedCamera(const RE::NiPoint3& a_pos, const RE::NiMatrix3& a_rot);
		void NoteFrameStep(char a_step);
	}

	// SkyCraft.ini [Debug] bDiagnostics: the detailed per-frame timing, camera, combat and lighting logs
	// used while developing (several lines a second). Off by default.
	bool DiagnosticsEnabled();

	// SkyCraft.ini [Mobs] sStandInHide: how a Minecraft mob's invisible Skyrim stand-in is hidden.
	// 0 none (shown), 1 geometry (the default: its body is hidden, the actor is left showing so it can be locked on to),
	// 2 root (its whole 3D is culled), 3 alpha.
	int StandInHideMode();
	// SkyCraft.ini [Mobs] sStandInBase: the creature (name, or "Plugin.esp:0xFORMID") mob stand-ins are made from; empty = the default (a rabbit).
	std::string StandInBaseSetting();

	namespace CrashLog
	{
		// Logs where Skyrim crashed (and writes a minidump) if it does.
		void Install();
	}

	namespace Launcher
	{
		// At plugin load: starts Minecraft (per SkyCraft.ini) unless it's already running.
		void StartMinecraft();

		enum class Status
		{
			kOff,         // not started by SkyCraft (bStartWithSkyrim = 0)
			kRunning,     // was already running
			kStarting,    // being started
			kSignIn,      // first start of the bundled Minecraft: Prism asks for a Microsoft account
			kNoLauncher,  // nothing to start it with
			kFailed,      // starting it failed
		};
		Status GetStatus();

		// A Minecraft with the SkyCraft mod is running (it holds SkyCraft's mutex from early on).
		bool MinecraftRunning();
		// Prism Launcher is running (downloading, waiting on a sign-in or showing an error).
		bool PrismRunning();
	}

	namespace Input
	{
		void Install();
		// Adds the input sink early (kInputLoaded) so it runs ahead of other mods' sinks.
		void InstallSink();
		// Mouse deltas accumulated since the last frame (main thread).
		void ConsumeLook(float& a_dx, float& a_dy);
		// Tells MC to release everything (input focus moved to Skyrim).
		void ReleaseAll();
		// The key that switches between Minecraft's controls and Skyrim's, as the player wrote it in
		// SkyCraft.ini ("F8"), for the corner messages.
		const std::string& ToggleKeyName();
	}

	namespace Input
	{
		// While Minecraft drives the player, Skyrim's Activate prompt shows G (our activate key).
		void SetActivatePromptKey(bool a_minecraftControls);
	}

	namespace LockOn
	{
		// At kDataLoaded: finds True Directional Movement's virtual-target interface, if it has one.
		void Install();
		// Main thread, every frame: hands Minecraft's mobs to True Directional Movement as lock-on targets while a_active (Skyrim's
		// own control mode, Minecraft's world around the player), and takes them back when not.
		void Update(RE::PlayerCharacter* a_player, bool a_active);
		// Main thread: tells True Directional Movement which actors it must never lock on to (Combat.cpp's stand-ins). Called whenever the set changes.
		void SetIgnoredActors(const std::vector<std::uint32_t>& a_formIds);
	}

	namespace Combat
	{
		void Install();
		// Main thread, once per frame: actor table out, Minecraft's hits in, player damage bridged.
		void PerFrame(RE::PlayerCharacter* a_player, bool a_puppeting, float a_delta);
		// Someone nearby is fighting the player (in combat with them as its target). Main thread.
		bool PlayerEngaged();
		// Is this actor a Minecraft mob's stand-in (main thread)? Blocks never push those: the mob they stand for decides where they are.
		bool IsStandIn(RE::FormID a_id);
		// Every stand-in's form id, for True Directional Movement's never-lock-on list (main thread).
		void GetStandInIds(std::vector<std::uint32_t>& a_out);
		// The attack button (left mouse) went down or up while Skyrim has the controls (input sink). Combat works out
		// from it, once per frame, what the player swung or shot and has Minecraft hit the mobs in the way.
		void NoteSkyrimAttackButton(bool a_down);
	}

	namespace BlockLights
	{
		// Render messages (main thread): a section's light-emitting blocks; a world change.
		void OnLights(const std::uint8_t* a_data, std::uint32_t a_bytes);
		void Clear();
		// Main thread, every frame: Skyrim point lights on the brightest emitters near the player
		// (Minecraft coords; null turns them all off).
		void Update(const McVec* a_player, float a_delta);
		// One of those lights (the block shader leaves them out: Minecraft's blocks carry
		// Minecraft's own block light).
		bool IsOurs(const RE::NiLight* a_light);
		// What a Minecraft block does to whoever stands in it (proto::BlockHazard: lava, fire, magma).
		std::uint8_t HazardAt(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z);
	}

	namespace PathAvoid
	{
		// Hooks Skyrim's NPC path setup so paths route around Minecraft's solid blocks.
		void Install();
		// The goal of the NPC's latest path (Skyrim position), if one was seen. Any thread.
		bool GoalOf(RE::FormID a_actor, RE::NiPoint3& a_out);
	}

	namespace NpcBlocks
	{
		// Render messages (main thread): a section's solid Minecraft blocks; a world change.
		void OnSolids(const std::uint8_t* a_data, std::uint32_t a_bytes);
		void Clear();
		bool SolidAt(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z);
		// Minecraft's exact collision boxes (blocks, absolute Minecraft coordinates, y up). A block that
		// isn't a full cube (slab, stair, fence, carpet...) has its own boxes; a full one has one cube.
		struct BlockBox
		{
			float lo[3], hi[3];
		};
		static constexpr int kMaxBoxes = 16;  // (a block with more has the rest dropped; vanilla blocks have at most a handful)
		// Boxes of block (x, y, z) in block-local coordinates (0..1, fences up to 1.5); 0 if not solid.
		int BoxesAt(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z, BlockBox a_out[kMaxBoxes]);
		// Every box of every solid block in the given block range (inclusive), absolute coordinates.
		void GatherBoxes(int a_x0, int a_x1, int a_y0, int a_y1, int a_z0, int a_z1, std::vector<BlockBox>& a_out);
		// Every section that has solid blocks, with a stamp that changes whenever its blocks (or their shapes) do.
		struct SectionStamp
		{
			std::int32_t  sx, sy, sz;
			std::uint32_t stamp;
		};
		void ListSections(std::vector<SectionStamp>& a_out);
		// Is the face of a_box (absolute coords) on a_axis (0 x, 1 y, 2 z), direction a_sign, flush against
		// another solid box that covers it (so the face is inside a wall and mustn't snag anything)?
		bool FaceCovered(const BlockBox& a_box, int a_axis, int a_sign);
		void OnShapes(const std::uint8_t* a_data, std::uint32_t a_bytes);
		// For the blocks' contact shadows (any thread): sets a_bit in a_out[x + w*(y + h*z)] for
		// each solid block of the box at a_origin (Minecraft block coords) of a_size blocks. The
		// generation changes whenever any section's blocks do.
		void CopySolids(const std::int32_t a_origin[3], const std::int32_t a_size[3], std::uint32_t* a_out, std::uint32_t a_bit);
		std::uint32_t Generation();
		// Main thread, every frame: NPCs overlapping solid Minecraft blocks are pushed back out.
		void PushActorsOut(RE::PlayerCharacter* a_player, float a_delta);
		// Main thread, every frame in Skyrim's control mode: the player walking into a block no higher than Minecraft's step (0.6) is lifted onto it,
		// as Minecraft's player is (stairs, slabs, carpets), without a jump.
		void StepUpPlayer(RE::PlayerCharacter* a_player, float a_delta);
	}

	// The key of a block's collision shape: FNV-1a over "x0,y0,z0,x1,y1,z1;..." (boxes in sixteenths of a block, sorted by y0, x0, z0, y1,
	// x1, z1), as 8 hex digits. It is ShapeCatalog.canonical/key in the Fabric mod and fnv_key in tools/make_collision_library.py,
	// and names the library file shp_<key>.nif.
	struct ShapeBox16
	{
		int v[6];  // x0 y0 z0 x1 y1 z1
	};
	inline std::string ShapeKey(std::vector<ShapeBox16> a_boxes)
	{
		std::sort(a_boxes.begin(), a_boxes.end(), [](const ShapeBox16& a, const ShapeBox16& b) {
			static constexpr int order[6] = { 1, 0, 2, 4, 3, 5 };
			for (const int k : order) {
				if (a.v[k] != b.v[k]) {
					return a.v[k] < b.v[k];
				}
			}
			return false;
		});
		std::uint32_t h = 0x811C9DC5u;
		bool          first = true;
		for (const auto& box : a_boxes) {
			for (int k = 0; k < 6; ++k) {
				const std::string part = std::to_string(box.v[k]) + (k == 5 ? "" : ",");
				for (const char c : (first || k ? std::string() : std::string(";")) + part) {
					h = (h ^ static_cast<std::uint8_t>(c)) * 0x01000193u;
				}
			}
			first = false;
		}
		char out[9];
		std::snprintf(out, sizeof out, "%08x", h);
		return out;
	}

	// Minecraft's mobs as things that physically exist in Skyrim's world: one invisible collision box per mob, the size of the mob's bounding
	// box, made from a library built at plugin load from the mob catalogue Minecraft writes (MobBodies.cpp). Replaces the creature stand-ins.
	namespace MobBodies
	{
		bool Enabled();
		bool KeepActors();
		// At plugin load, before the game reads its files (Skyrim loads only model files that existed when it started).
		void PrepareFiles();
		void OnProxy(RE::PlayerCharacter* a_player, const proto::McEvent& a_ev);
		void OnVitals(RE::PlayerCharacter* a_player, const proto::McEvent& a_ev);
		void Update(RE::PlayerCharacter* a_player, float a_delta);
		std::int32_t MobOfRef(RE::FormID a_refId);
		bool         AnyBodies();
		std::string  KeyFor(std::int32_t a_mobId, float a_height);
		bool         CustomSkeletonOn();
		std::string  SkeletonModel(const std::string& a_key, int a_w16, int a_h16);
		bool         ParseKey(const std::string& a_key, int& a_w16, int& a_h16);
		void         ArmSkeletonTrial();
		void         SkeletonTrialPassed();
		void         SkeletonTrialFailed();
		void Clear();
	}

	namespace BlockCollision
	{
		// Skyrim's own control mode: real collision for every Minecraft block Minecraft has sent, as models of merged boxes loaded as
		// invisible statics (BlockCollision.cpp), so that the player, NPCs and projectiles all meet the blocks. False when it is off in
		// SkyCraft.ini or has given up on this install; BlockCrates then holds the player up instead.
		bool Wanted();
		// A section model has been seen with its collision in Skyrim's Havok world, so the models work on this install; until then the crates
		// BlockCrates.cpp makes round the player are kept as well, so there is always something to stand on.
		bool Verified();
		bool NeedCrates();
		// At plugin load, before the game reads its files: Skyrim loads only model files that existed when it started, so the files the
		// section models will be written into are made now (BlockCollision.cpp).
		void PrepareFiles();
		// Main thread, every frame, while Skyrim's controls are on.
		void Update(RE::PlayerCharacter* a_player, float a_delta);
		// Puts every section model away (leaving Skyrim's control mode, another cell).
		void Release();
	}

	namespace WorldRender
	{
		// Hooks Skyrim's frame so blocks are drawn into its scene before post-processing.
		void Install();
		// A Minecraft arrow stuck in a Skyrim actor where it hit (MC coords, flight yaw/pitch):
		// pinned to the nearest bone and drawn from then on. Main thread.
		void StickArrow(RE::FormID a_actor, float a_x, float a_y, float a_z, float a_yawDeg, float a_pitchDeg);
		// Main thread, every frame: once the Skyrim player is dead, their Minecraft body replaces
		// Skyrim's on its ragdoll (a_minecraftBody: Minecraft is connected and in its world).
		void UpdateRagdoll(RE::PlayerCharacter* a_player, bool a_minecraftBody);
		// Present hook, before the overlay: drains Minecraft's meshes; draws the blocks, arrows,
		// items and the outline here only if they couldn't be drawn inside Skyrim's frame.
		void Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, IDXGISwapChain* a_swapChain);
		// Debug: saves the frame as a PNG when <SKSE log dir>/skycraft_capture.request exists.
		void CaptureIfRequested(ID3D11DeviceContext* a_context, IDXGISwapChain* a_swapChain);
	}

	namespace Overlay
	{
		void Install();
	}
}
