#include "Game.h"
#include "Dig.h"

#include <SimpleIni.h>

// Skyrim's NPCs don't walk through Minecraft's blocks: Minecraft sends which blocks of each section
// have a collision shape (proto::kRenSolids), and every frame any NPC overlapping one is pushed
// back out sideways, so they stop at walls and slide along them.
namespace skycraft
{
	namespace
	{
		std::unordered_map<std::uint64_t, std::array<std::uint64_t, 64>> solids;  // section -> bit x + 16z + 256y
		// Which sections have solids, and when each last changed (the generation at that moment): BlockCollision.cpp rebuilds a section's model when it moves on.
		std::unordered_map<std::uint64_t, NpcBlocks::SectionStamp> stamps;  // guarded by solidsLock
		// Written on the main thread; read there and by Skyrim's AI threads planning NPC paths.
		std::shared_mutex solidsLock;
		std::atomic<std::uint32_t> solidsGeneration{ 0 };  // bumped on every change

		// Blocks that aren't whole cubes: section -> block index (x + 16z + 256y) -> their boxes (sixteenths).
		struct ShapeBlock
		{
			std::uint8_t n{ 0 };
			std::uint8_t b[16][6]{};  // kMaxBoxes
		};
		std::unordered_map<std::uint64_t, std::unordered_map<std::uint16_t, ShapeBlock>> shapes;  // guarded by solidsLock

		std::uint64_t SectionKey(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z)
		{
			return (std::uint64_t(std::uint32_t(a_x) & 0x3FFFFF) << 42) | (std::uint64_t(std::uint32_t(a_z) & 0x3FFFFF) << 20) |
			       (std::uint64_t(std::uint32_t(a_y) & 0xFFFFF));
		}

		int FloorDiv16(int a_v) { return a_v >> 4; }  // arithmetic shift: floor for negatives too

		constexpr float kRange = 4000.0f;          // Skyrim units from the player
		constexpr float kMaxRadiusBlocks = 0.45f;  // wider actors still fit through a one-block gap
		constexpr float kSlideSpeed = 3.2f;        // blocks a second along a wall
		// Havok keeps NPCs just clear of blocks now (DigPhysics.cpp); a wall this close still counts
		// as walked into, so they follow it round.
		constexpr double kTouchBlocks = 0.1;

		// An NPC following a wall around: which way (+1/-1 along the wall) and for how long more.
		struct Detour
		{
			int   side = 0;
			float time = 0.0f;
		};
		std::unordered_map<RE::FormID, Detour> detours;

		bool ColumnBlocked(int a_x, int a_z, int a_y0, int a_y1);

		// A circle (an actor seen from above) against a box's footprint: adds the push out to a_px, a_pz.
		void PushOutOfRect(double a_cx, double a_cz, double a_r, double a_x0, double a_x1, double a_z0, double a_z1, double& a_px, double& a_pz)
		{
			const double qx = std::clamp(a_cx, a_x0, a_x1), qz = std::clamp(a_cz, a_z0, a_z1);
			double       dx = a_cx - qx, dz = a_cz - qz;
			const double d = std::sqrt(dx * dx + dz * dz);
			if (d >= a_r) {
				return;
			}
			if (d < 1e-6) {
				// Centre inside the footprint: out through the nearest side.
				const double toW = a_cx - a_x0, toE = a_x1 - a_cx, toN = a_cz - a_z0, toS = a_z1 - a_cz;
				const double m = std::min({ toW, toE, toN, toS });
				dx = m == toW ? -1.0 : m == toE ? 1.0 : 0.0;
				dz = m == toN ? -1.0 : m == toS ? 1.0 : 0.0;
				a_px += dx * (m + a_r);
				a_pz += dz * (m + a_r);
			} else {
				a_px += dx / d * (a_r - d);
				a_pz += dz / d * (a_r - d);
			}
		}
	}

	namespace NpcBlocks
	{
		bool SolidAt(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z);
	}

	namespace
	{
		bool ColumnBlocked(int a_x, int a_z, int a_y0, int a_y1)
		{
			for (int y = a_y0; y <= a_y1; ++y) {
				if (NpcBlocks::SolidAt(a_x, y, a_z)) {
					return true;
				}
			}
			return false;
		}
	}

	namespace NpcBlocks
	{
		void OnSolids(const std::uint8_t* a_data, std::uint32_t a_bytes)
		{
			if (a_bytes < sizeof(proto::RenSolids)) {
				return;
			}
			const auto* hdr = reinterpret_cast<const proto::RenSolids*>(a_data);
			const auto  key = SectionKey(hdr->sx, hdr->sy, hdr->sz);
			std::unique_lock lock(solidsLock);
			++solidsGeneration;
			if (hdr->count == 0 || a_bytes < sizeof(proto::RenSolids) + 512) {
				solids.erase(key);
				shapes.erase(key);
				stamps.erase(key);
				return;
			}
			std::memcpy(solids[key].data(), a_data + sizeof(proto::RenSolids), 512);
			stamps[key] = { hdr->sx, hdr->sy, hdr->sz, solidsGeneration.load() };
		}

	void Clear()
		{
			std::unique_lock lock(solidsLock);
			++solidsGeneration;
			solids.clear();
			shapes.clear();
			stamps.clear();
		}

		void ListSections(std::vector<SectionStamp>& a_out)
		{
			std::shared_lock lock(solidsLock);
			a_out.reserve(a_out.size() + stamps.size());
			for (const auto& entry : stamps) {
				a_out.push_back(entry.second);
			}
		}

		std::uint32_t Generation() { return solidsGeneration.load(); }

		void CopySolids(const std::int32_t a_origin[3], const std::int32_t a_size[3], std::uint32_t* a_out, std::uint32_t a_bit)
		{
			std::shared_lock lock(solidsLock);
			const int x0 = a_origin[0], y0 = a_origin[1], z0 = a_origin[2];
			const int w = a_size[0], h = a_size[1], d = a_size[2];
			for (int sy = FloorDiv16(y0); sy <= FloorDiv16(y0 + h - 1); ++sy) {
				for (int sz = FloorDiv16(z0); sz <= FloorDiv16(z0 + d - 1); ++sz) {
					for (int sx = FloorDiv16(x0); sx <= FloorDiv16(x0 + w - 1); ++sx) {
						const auto it = solids.find(SectionKey(sx, sy, sz));
						if (it == solids.end()) {
							continue;
						}
						const auto& bits = it->second;
						for (int ly = 0; ly < 16; ++ly) {
							const int y = sy * 16 + ly - y0;
							if (y < 0 || y >= h) {
								continue;
							}
							for (int lz = 0; lz < 16; ++lz) {
								const int z = sz * 16 + lz - z0;
								if (z < 0 || z >= d) {
									continue;
								}
								for (int lx = 0; lx < 16; ++lx) {
									const int x = sx * 16 + lx - x0;
									const int bit = lx + 16 * lz + 256 * ly;
									if (x >= 0 && x < w && ((bits[bit >> 6] >> (bit & 63)) & 1)) {
										a_out[x + w * (y + h * z)] |= a_bit;
									}
								}
							}
						}
					}
				}
			}
		}


		// Boxes of one block, block-local, in blocks. The caller holds solidsLock.
		int BoxesLocked(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z, BlockBox a_out[kMaxBoxes])
		{
			const auto key = SectionKey(FloorDiv16(a_x), FloorDiv16(a_y), FloorDiv16(a_z));
			const auto it = solids.find(key);
			if (it == solids.end()) {
				return 0;
			}
			const int bit = (a_x & 15) + 16 * (a_z & 15) + 256 * (a_y & 15);
			if (!((it->second[bit >> 6] >> (bit & 63)) & 1)) {
				return 0;
			}
			const auto sit = shapes.find(key);
			if (sit != shapes.end()) {
				const auto bit2 = sit->second.find(std::uint16_t(bit));
				if (bit2 != sit->second.end()) {
					const auto& sb = bit2->second;
					for (int i = 0; i < sb.n && i < kMaxBoxes; ++i) {
						for (int k = 0; k < 3; ++k) {
							a_out[i].lo[k] = sb.b[i][k] / 16.0f;
							a_out[i].hi[k] = sb.b[i][3 + k] / 16.0f;
						}
					}
					return std::min<int>(sb.n, kMaxBoxes);
				}
			}
			a_out[0] = { { 0, 0, 0 }, { 1, 1, 1 } };
			return 1;
		}

		int BoxesAt(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z, BlockBox a_out[kMaxBoxes])
		{
			std::shared_lock lock(solidsLock);
			return BoxesLocked(a_x, a_y, a_z, a_out);
		}

		void GatherBoxes(int a_x0, int a_x1, int a_y0, int a_y1, int a_z0, int a_z1, std::vector<BlockBox>& a_out)
		{
			std::shared_lock lock(solidsLock);
			if (solids.empty()) {
				return;
			}
			BlockBox local[kMaxBoxes];
			for (int y = a_y0; y <= a_y1; ++y) {
				for (int z = a_z0; z <= a_z1; ++z) {
					for (int x = a_x0; x <= a_x1; ++x) {
						const int n = BoxesLocked(x, y, z, local);
						for (int i = 0; i < n; ++i) {
							BlockBox abs = local[i];
							abs.lo[0] += float(x), abs.hi[0] += float(x);
							abs.lo[1] += float(y), abs.hi[1] += float(y);
							abs.lo[2] += float(z), abs.hi[2] += float(z);
							a_out.push_back(abs);
						}
					}
				}
			}
		}

		bool FaceCovered(const BlockBox& a_box, int a_axis, int a_sign)
		{
			constexpr float eps = 1e-3f;
			const int       a1 = (a_axis + 1) % 3, a2 = (a_axis + 2) % 3;
			const float     c = a_sign > 0 ? a_box.hi[a_axis] : a_box.lo[a_axis];
			int             lo[3], hi[3];
			lo[a_axis] = hi[a_axis] = int(std::floor(c + (a_sign > 0 ? 1e-4f : -1e-4f)));
			lo[a1] = int(std::floor(a_box.lo[a1] + 1e-4f)), hi[a1] = int(std::floor(a_box.hi[a1] - 1e-4f));
			lo[a2] = int(std::floor(a_box.lo[a2] + 1e-4f)), hi[a2] = int(std::floor(a_box.hi[a2] - 1e-4f));
			std::shared_lock lock(solidsLock);
			BlockBox         local[kMaxBoxes];
			for (int z = lo[2]; z <= hi[2]; ++z) {
				for (int y = lo[1]; y <= hi[1]; ++y) {
					for (int x = lo[0]; x <= hi[0]; ++x) {
						const int n = BoxesLocked(x, y, z, local);
						const int cell[3] = { x, y, z };
						for (int i = 0; i < n; ++i) {
							const float opposite = (a_sign > 0 ? local[i].lo[a_axis] : local[i].hi[a_axis]) + float(cell[a_axis]);
							if (std::fabs(opposite - c) > eps) {
								continue;
							}
							if (local[i].lo[a1] + float(cell[a1]) <= a_box.lo[a1] + eps && local[i].hi[a1] + float(cell[a1]) >= a_box.hi[a1] - eps &&
								local[i].lo[a2] + float(cell[a2]) <= a_box.lo[a2] + eps && local[i].hi[a2] + float(cell[a2]) >= a_box.hi[a2] - eps) {
								return true;
							}
						}
					}
				}
			}
			return false;
		}

		void OnShapes(const std::uint8_t* a_data, std::uint32_t a_bytes)
		{
			if (a_bytes < sizeof(proto::RenShapes)) {
				return;
			}
			const auto* hdr = reinterpret_cast<const proto::RenShapes*>(a_data);
			const auto  key = SectionKey(hdr->sx, hdr->sy, hdr->sz);
			std::unordered_map<std::uint16_t, ShapeBlock> parsed;
			std::uint32_t                                  at = sizeof(proto::RenShapes);
			for (std::uint32_t i = 0; i < hdr->count; ++i) {
				if (at + 3 > a_bytes) {
					break;
				}
				const std::uint16_t index = std::uint16_t(a_data[at] | (a_data[at + 1] << 8));
				const std::uint8_t  n = a_data[at + 2];
				at += 3;
				if (n > kMaxBoxes || at + std::uint32_t(n) * 6 > a_bytes || index >= 4096) {
					break;
				}
				ShapeBlock sb;
				sb.n = n;
				std::memcpy(sb.b, a_data + at, std::size_t(n) * 6);
				at += std::uint32_t(n) * 6;
				parsed[index] = sb;
			}
			std::unique_lock lock(solidsLock);
			++solidsGeneration;
			logger::info("shapes: section ({}, {}, {}) now holds {} non-cube blocks (generation {})", hdr->sx, hdr->sy, hdr->sz, parsed.size(), solidsGeneration.load());
			if (parsed.empty()) {
				shapes.erase(key);
			} else {
				shapes[key] = std::move(parsed);
			}
			if (const auto it = stamps.find(key); it != stamps.end()) {
				it->second.stamp = solidsGeneration.load();  // its model is built again with the shapes
			}
		}

		bool SolidAt(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z)
		{
			std::shared_lock lock(solidsLock);
			const auto       it = solids.find(SectionKey(FloorDiv16(a_x), FloorDiv16(a_y), FloorDiv16(a_z)));
			if (it == solids.end()) {
				return false;
			}
			const int bit = (a_x & 15) + 16 * (a_z & 15) + 256 * (a_y & 15);
			return (it->second[bit >> 6] >> (bit & 63)) & 1;
		}

		// How far (blocks, up to a_max) along direction (tx, tz) from (cx, cz) until the wall on the
		// side of normal (-nx, -nz) ends, so the NPC can turn the corner there.
		double WallEnd(double cx, double cz, double tx, double tz, double nx, double nz, int y0, int y1, double a_max)
		{
			for (double s = 0.5; s <= a_max; s += 0.5) {
				const double px = cx + tx * s, pz = cz + tz * s;
				if (ColumnBlocked(int(std::floor(px)), int(std::floor(pz)), y0, y1)) {
					return a_max + 1.0;  // walled in that way too
				}
				const double wx = px - nx * 1.0, wz = pz - nz * 1.0;  // one block into the wall's side
				if (!ColumnBlocked(int(std::floor(wx)), int(std::floor(wz)), y0, y1)) {
					return s;
				}
			}
			return a_max + 1.0;
		}

		void PushActorsOut(RE::PlayerCharacter* a_player, float a_delta)
		{
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists || solids.empty()) {
				return;
			}
			const auto playerPos = a_player->GetPosition();
			for (auto& handle : lists->highActorHandles) {
				auto actorPtr = handle.get();
				auto* actor = actorPtr.get();
				if (!actor || actor == a_player || actor->IsDead() || !actor->Is3DLoaded() || actor->GetPosition().GetDistance(playerPos) > kRange || Combat::IsStandIn(actor->GetFormID())) {
					continue;
				}
				const auto   sky = actor->GetPosition();
				const auto   mc = SkyToMc(sky);
				const double r = std::clamp(double(actor->GetBoundRadius()) / proto::kUnitsPerBlock, 0.2, double(kMaxRadiusBlocks)) + kTouchBlocks;
				const double h = std::clamp(double(actor->GetHeight()) / proto::kUnitsPerBlock, 0.5, 4.0);
				// From just above the feet (standing on a block is fine) to the head.
				const int y0 = int(std::floor(mc.y + 0.3)), y1 = int(std::floor(mc.y + h - 0.1));
				double    cx = mc.x, cz = mc.z;
				bool      moved = false;
				// The exact boxes around the actor that reach into its height (a fence from the block below too).
				std::vector<BlockBox> nearby;
				GatherBoxes(int(std::floor(cx - r - 1.0)), int(std::floor(cx + r + 1.0)), y0 - 1, y1, int(std::floor(cz - r - 1.0)), int(std::floor(cz + r + 1.0)), nearby);
				const float bodyLo = float(mc.y + 0.3), bodyHi = float(mc.y + h - 0.1);
				std::erase_if(nearby, [&](const BlockBox& a_box) { return a_box.hi[1] <= bodyLo || a_box.lo[1] >= bodyHi; });
				for (int pass = 0; pass < 3 && !nearby.empty(); ++pass) {
					double pushX = 0.0, pushZ = 0.0;
					for (const auto& box : nearby) {
						PushOutOfRect(cx, cz, r, box.lo[0], box.hi[0], box.lo[2], box.hi[2], pushX, pushZ);
					}
					if (std::abs(pushX) < 1e-4 && std::abs(pushZ) < 1e-4) {
						break;
					}
					cx += pushX;
					cz += pushZ;
					moved = true;
				}
				auto& detour = detours[actor->GetFormID()];
				detour.time -= a_delta;
				if (moved) {
					// Walking into the wall (pushed back against its heading)? Follow it around.
					const double pushX = cx - mc.x, pushZ = cz - mc.z, pushLen = std::sqrt(pushX * pushX + pushZ * pushZ);
					if (pushLen > 1e-4) {
						const double nx = pushX / pushLen, nz = pushZ / pushLen;  // out of the wall
						double       gx = 0.0, gz = 0.0;
						RE::NiPoint3 goalSky;
						if (PathAvoid::GoalOf(actor->GetFormID(), goalSky)) {
							const auto goal = SkyToMc(goalSky);
							gx = goal.x - cx, gz = goal.z - cz;
						} else {
							const float heading = actor->GetAngleZ();  // Skyrim heading -> Minecraft x, z
							gx = std::sin(heading), gz = -std::cos(heading);
						}
						const double gl = std::sqrt(gx * gx + gz * gz);
						if (gl > 1e-3 && (gx * -nx + gz * -nz) / gl > 0.2) {
							const double tx = -nz, tz = nx;  // along the wall
							if (detour.time <= 0.0f || detour.side == 0) {
								const double endA = WallEnd(cx, cz, tx, tz, nx, nz, y0, y1, 12.0);
								const double endB = WallEnd(cx, cz, -tx, -tz, nx, nz, y0, y1, 12.0);
								const double along = (gx * tx + gz * tz) / gl;  // which end is toward the goal
								detour.side = (endA - 2.0 * along) <= (endB + 2.0 * along) ? 1 : -1;
							}
							detour.time = 1.5f;
							const double step = kSlideSpeed * a_delta;
							cx += tx * detour.side * step;
							cz += tz * detour.side * step;
						}
					}
					auto pos = McToSky(cx, mc.y, cz);
					pos.z = sky.z;
					actor->SetPosition(pos, true);
				} else if (detour.time <= 0.0f) {
					detours.erase(actor->GetFormID());
				}
			}
			if (detours.size() > 256) {
				detours.clear();
			}
		}
	}

	// ---- walking up steps the way Minecraft does ----------------------------------------------------------------------------
	//
	// Minecraft lifts a walking player over anything up to 0.6 of a block (a stair, a slab, a carpet) without a jump. Skyrim's character
	// controller can't: it stops at the riser, which is what made stairs feel wrong in Skyrim's control mode though their collision is
	// exact. So each frame, when the player stands on the ground and is walking into a Minecraft block whose top is within that step,
	// with room above it for the whole body, the player is set on top of it, as Minecraft would. The block data is the same as the
	// collision models are made from (the exact boxes of every block), so it is the same stairs the model has.
	namespace
	{
		struct StepCfg
		{
			bool  on{ true };
			float height{ 0.6f };   // blocks: the highest step taken without a jump (Minecraft: 0.6)
			float radius{ 0.3f };   // blocks: how wide the player's body is, from its middle
			float reach{ 0.14f };   // blocks: how far from the body a step is looked for (the controller stops a little short of a wall)
			float bodyHeight{ 1.8f };  // blocks: room needed above the step
		};

		const StepCfg& StepSettings()
		{
			static const StepCfg settings = [] {
				CSimpleIniA ini;
				ini.SetUnicode();
				ini.LoadFile("Data/SKSE/Plugins/SkyCraft.ini");
				StepCfg c;
				c.on = ini.GetBoolValue("Collision", "bStepUp", true);
				c.height = static_cast<float>(std::clamp(ini.GetDoubleValue("Collision", "fStepHeight", 0.6), 0.0, 1.0));
				c.radius = static_cast<float>(std::clamp(ini.GetDoubleValue("Collision", "fStepRadius", 0.3), 0.1, 0.6));
				c.reach = static_cast<float>(std::clamp(ini.GetDoubleValue("Collision", "fStepReach", 0.14), 0.0, 0.5));
				return c;
			}();
			return settings;
		}

		// The way the player is pushing to move, from Skyrim's own movement input (relative to the way the body faces, as Skyrim reads it).
		// False when this build of the game library doesn't expose it.
		template <class Controls>
		bool ReadMoveInput(Controls* a_controls, float& a_x, float& a_y)
		{
			if constexpr (requires { a_controls->data.moveInputVec.x; a_controls->data.moveInputVec.y; }) {
				if (!a_controls) {
					return false;
				}
				a_x = a_controls->data.moveInputVec.x;
				a_y = a_controls->data.moveInputVec.y;
				return true;
			} else {
				return false;
			}
		}
	}

	namespace NpcBlocks
	{
		void StepUpPlayer(RE::PlayerCharacter* a_player, float a_delta)
		{
			const auto& cfg = StepSettings();
			if (!cfg.on || !a_player || a_player->IsDead()) {
				return;
			}

			static double lastX = 0.0, lastZ = 0.0, memX = 0.0, memZ = 0.0;
			static bool   haveLast = false;
			static float  memory = 0.0f, cooldown = 0.0f;
			static int    logged = 0;
			static bool   inputKnown = true, inputNoted = false;

			const auto sky = a_player->GetPosition();
			const auto mc = SkyToMc(sky);
			cooldown = std::max(0.0f, cooldown - a_delta);

			// Where the player has really been going lately (used when the movement input can't be read).
			if (haveLast) {
				const double dx = mc.x - lastX, dz = mc.z - lastZ;
				const double len = std::sqrt(dx * dx + dz * dz);
				if (len > 0.01 && len < 3.0) {
					memX = dx / len;
					memZ = dz / len;
					memory = 0.35f;
				} else {
					memory = std::max(0.0f, memory - a_delta);
				}
			}
			lastX = mc.x;
			lastZ = mc.z;
			haveLast = true;

			auto* controller = a_player->GetCharController();
			if (!controller || controller->context.currentState != RE::hkpCharacterStateType::kOnGround || cooldown > 0.0f) {
				return;
			}

			// The way it is trying to go, in Minecraft's horizontal axes (x east, z south).
			double wantX = 0.0, wantZ = 0.0;
			float  inX = 0.0f, inY = 0.0f;
			if (ReadMoveInput(RE::PlayerControls::GetSingleton(), inX, inY)) {
				if (!inputNoted) {
					inputNoted = true;
					logger::info("Step up: the movement input is readable, so the step is taken in the way the player is pushing");
				}
				if (std::sqrt(inX * inX + inY * inY) < 0.2f) {
					return;
				}
				const double h = a_player->GetAngleZ();
				// forward is (sin h, -cos h) and right is (cos h, sin h) in Minecraft's x, z (Skyrim's y is Minecraft's -z)
				wantX = std::sin(h) * inY + std::cos(h) * inX;
				wantZ = -std::cos(h) * inY + std::sin(h) * inX;
			} else {
				inputKnown = false;
				if (!inputNoted) {
					inputNoted = true;
					logger::info("Step up: the movement input can't be read in this build, so the step is taken in the way the player has just been moving");
				}
				if (memory <= 0.0f) {
					return;
				}
				wantX = memX;
				wantZ = memZ;
			}
			const double wantLen = std::sqrt(wantX * wantX + wantZ * wantZ);
			if (wantLen < 1e-3) {
				return;
			}
			wantX /= wantLen;
			wantZ /= wantLen;

			// Every box near the body, from a block under the feet (fences and walls are 1.5 tall) to the head after a step.
			std::vector<BlockBox> boxes;
			GatherBoxes(int(std::floor(mc.x - 1.0)), int(std::floor(mc.x + 1.0)), int(std::floor(mc.y)) - 1, int(std::floor(mc.y + cfg.height + cfg.bodyHeight)),
				int(std::floor(mc.z - 1.0)), int(std::floor(mc.z + 1.0)), boxes);
			if (boxes.empty()) {
				return;
			}

			const double feet = mc.y;
			double       bestTop = -1.0;
			for (const auto& b : boxes) {
				const double top = b.hi[1];
				if (top <= feet + 0.02 || top > feet + cfg.height + 1e-3 || top <= bestTop) {
					continue;  // not higher than where the feet are, or higher than a step, or no better than one already found
				}
				// Is the box close to the body, and in the way the player is going?
				const double qx = std::clamp(mc.x, double(b.lo[0]), double(b.hi[0]));
				const double qz = std::clamp(mc.z, double(b.lo[2]), double(b.hi[2]));
				const double vx = qx - mc.x, vz = qz - mc.z;
				const double dist = std::sqrt(vx * vx + vz * vz);
				if (dist < 1e-4 || dist >= cfg.radius + cfg.reach) {
					continue;
				}
				if ((vx * wantX + vz * wantZ) / dist < 0.5) {
					continue;  // beside the body (sliding along it), not ahead
				}
				// Room for the whole body standing on it, where the body is now?
				const double newFeet = top + 0.01;
				const double half = cfg.radius - 0.02;
				bool         roomy = true;
				for (const auto& o : boxes) {
					if (o.lo[0] < mc.x + half && o.hi[0] > mc.x - half && o.lo[2] < mc.z + half && o.hi[2] > mc.z - half && o.lo[1] < newFeet + cfg.bodyHeight &&
						o.hi[1] > newFeet + 0.001) {
						roomy = false;
						break;
					}
				}
				if (roomy) {
					bestTop = top;
				}
			}
			if (bestTop < 0.0) {
				return;
			}

			auto up = sky;
			up.z = McToSky(mc.x, bestTop + 0.01, mc.z).z;
			a_player->SetPosition(up, true);
			controller->fallStartHeight = up.z;
			controller->fallTime = 0.0f;
			cooldown = 0.05f;
			if (logged < 12) {
				++logged;
				logger::info("Step up: lifted the player {:.2f} blocks onto a step (feet {:.2f} -> {:.2f}), going ({:.2f}, {:.2f}), input {}", bestTop + 0.01 - feet, feet, bestTop + 0.01,
					wantX, wantZ, inputKnown ? "read" : "not readable");
			}
		}
	}
}
