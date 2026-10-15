package dev.skycraft.combat;

import dev.skycraft.link.SkyLink;
import dev.skycraft.world.SkyCollision;
import dev.skycraft.world.SkyRay;
import dev.skycraft.world.SkyTime;
import dev.skycraft.world.SkyTri;
import dev.skycraft.world.SkyWater;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.util.RandomSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntitySpawnReason;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.MobCategory;
import net.minecraft.world.entity.SpawnGroupData;
import net.minecraft.world.phys.AABB;

/**
 * Minecraft's mobs spawn in Skyrim's world the way they do in Minecraft. Minecraft's own spawner
 * looks for solid blocks to stand on and finds none (Skyrim's land isn't made of blocks), so this
 * does what it does over Skyrim's ground instead:
 *
 * <ul>
 * <li>Hostile mobs (zombies, skeletons, spiders, creepers) spawn in packs, 24 to 38 blocks from the
 * player, where it is dark: on open ground at night (Skyrim's night, see {@link SkyTime}) and under
 * cover (caves, ruins, any roof) at any hour. Under cover they don't burn in the sun.
 * <li>Animals (sheep, pigs, chickens, cows, horses) spawn in packs on Skyrim's open land by day.
 * <li>Neither spawns on water, in a spot the player has no line of sight to (it might be on the far
 * side of a wall or on a roof), or near Skyrim's NPCs (towns and their houses are lit and lived in).
 * <li>Like Minecraft's, each kind has a cap on how many live around the player, and mobs left far
 * behind are removed (Skyrim only sends the land near the player, and a mob past it would fall).
 * </ul>
 *
 * <p>Only mobs this spawned are counted and removed. Turn it off with the JVM argument
 * {@code -Dskycraft.mobSpawning=false}; {@code -Dskycraft.monsterCap=N} and {@code -Dskycraft.animalCap=N}
 * change the caps (Minecraft's 70 and 10 are for an area some thirty times bigger than Skyrim's land here).
 */
public final class SkyrimSpawner {
	private static final boolean ENABLED = !"false".equalsIgnoreCase(System.getProperty("skycraft.mobSpawning"));
	private static final int MONSTER_CAP = Integer.getInteger("skycraft.monsterCap", 8);
	private static final int ANIMAL_CAP = Integer.getInteger("skycraft.animalCap", 6);
	private static final String TAG = "skycraft_natural";

	private static final int MONSTER_EVERY = 20;
	private static final int ANIMAL_EVERY = 60;
	private static final int UPKEEP_EVERY = 10;
	private static final double MIN_DISTANCE = 24.0;
	private static final double ANIMAL_MIN_DISTANCE = 16.0;
	private static final double MAX_DISTANCE = 38.0;
	/** Farther than this from every player, a spawned mob is removed. */
	private static final double DESPAWN_DISTANCE = 52.0;
	/** Where a spot's ground is looked for: this far above and below the player. */
	private static final double ABOVE = 8.0;
	private static final double CEILING_REACH = 3.5;
	private static final double BELOW = 16.0;
	private static final double NPC_BUFFER = 16.0;
	/** No mob spawns within this many blocks of a friendly person (a village is that wide, and a mob spawns 24 or more blocks from the player). */
	private static final double PEOPLE_BUFFER = 45.0;

	private record Kind(String id, int weight, int min, int max) {
	}

	// Minecraft's overworld spawn lists (weights and pack sizes).
	private static final List<Kind> MONSTERS = List.of(
		new Kind("spider", 100, 4, 4), new Kind("zombie", 95, 4, 4), new Kind("zombie_villager", 5, 1, 1),
		new Kind("skeleton", 100, 4, 4), new Kind("creeper", 100, 4, 4)
	);
	private static final List<Kind> ANIMALS = List.of(
		new Kind("sheep", 12, 2, 4), new Kind("pig", 10, 2, 4), new Kind("chicken", 10, 4, 4),
		new Kind("cow", 8, 2, 4), new Kind("horse", 5, 2, 6), new Kind("donkey", 1, 1, 3)
	);

	private static final Map<String, EntityType<?>> RESOLVED = new HashMap<>();

	/** The entity type for a spawn-list name, looked up in the registry (null if this Minecraft has none by that name). */
	@SuppressWarnings("unchecked")
	private static EntityType<? extends Mob> typeOf(Kind kind) {
		return (EntityType<? extends Mob>) RESOLVED.computeIfAbsent(
			kind.id(), id -> BuiltInRegistries.ENTITY_TYPE.getOptional(Identifier.withDefaultNamespace(id)).orElse(null)
		);
	}

	// Monsters standing under Skyrim's roofs: Minecraft would burn them in daylight, there being no blocks over them.
	private static final List<Mob> SHADED = new ArrayList<>();

	// Why spots were turned down, and how many mobs went in, reported to the log every half minute.
	private static final Map<String, Integer> REJECTS = new HashMap<>();
	private static int spawned, attempts;
	private static boolean waitingLogged;

	private static double[] reject(String why) {
		REJECTS.merge(why, 1, Integer::sum);
		return null;
	}

	private SkyrimSpawner() {
	}

	public static void init() {
		ServerTickEvents.END_SERVER_TICK.register(SkyrimSpawner::tick);
	}

	private static void tick(MinecraftServer server) {
		for (int i = SHADED.size() - 1; i >= 0; i--) {
			Mob mob = SHADED.get(i);
			if (!mob.isAlive()) {
				SHADED.remove(i);
			} else if (mob.isOnFire() && !mob.isInLava()) {
				mob.setRemainingFireTicks(0); // every tick, so it never gets to take the burn damage
			}
		}
		if (!SkyLink.active() || !SkyTime.known()) {
			if (SkyLink.active() && !waitingLogged && server.getTickCount() % 200 == 0) {
				waitingLogged = true;
				dev.skycraft.SkyCraft.LOG.info("SkyCraft spawner: waiting for Skyrim's clock (SkyTime isn't live yet)");
			}
			return;
		}
		int ticks = server.getTickCount();
		if (ticks % 600 == 0) {
			dev.skycraft.SkyCraft.LOG.info("SkyCraft spawner: hour {}, {} attempts, {} mobs spawned, turned-down spots: {}", SkyTime.hour(), attempts, spawned, REJECTS);
			REJECTS.clear();
			attempts = 0;
		}
		boolean upkeep = ticks % UPKEEP_EVERY == 0;
		if (upkeep) {
			SHADED.clear();
		}
		for (ServerPlayer player : server.getPlayerList().getPlayers()) {
			if (!player.isAlive() || player.isSpectator()) {
				continue;
			}
			ServerLevel level = player.level();
			if (upkeep) {
				upkeep(level, player);
			}
			if (ENABLED && ticks % MONSTER_EVERY == 0) {
				attempt(level, player, true);
			}
			if (ENABLED && ticks % ANIMAL_EVERY == 0) {
				attempt(level, player, false);
			}
		}
	}

	/** Takes mobs that have been left behind out of the world, and notes which monsters are under cover. */
	private static void upkeep(ServerLevel level, ServerPlayer player) {
		AABB around = player.getBoundingBox().inflate(DESPAWN_DISTANCE + 8.0, 64.0, DESPAWN_DISTANCE + 8.0);
		for (Mob mob : level.getEntitiesOfClass(Mob.class, around)) {
			if (mob.entityTags().contains(TAG)) {
				boolean far = level.getNearestPlayer(mob, DESPAWN_DISTANCE) == null;
				if (far || !SkyrimMobs.known(mob)) {
					mob.discard();
					continue;
				}
			}
			if (mob.getType().getCategory() == MobCategory.MONSTER && !mob.fireImmune()
				&& SkyrimMobs.covered(mob.getX(), mob.getY(), mob.getZ(), mob.getBbHeight())) {
				SHADED.add(mob);
			}
		}
	}

	private static void attempt(ServerLevel level, ServerPlayer player, boolean monsters) {
		// Not where people live: in a town, city, village, farm or inn nothing spawns, whatever the hour.
		if (SkyTime.inSettlement()) {
			REJECTS.merge("player is in a settlement", 1, Integer::sum);
			return;
		}
		int cap = monsters ? MONSTER_CAP : ANIMAL_CAP;
		MobCategory category = monsters ? MobCategory.MONSTER : MobCategory.CREATURE;
		int living = level.getEntitiesOfClass(
			Mob.class, player.getBoundingBox().inflate(DESPAWN_DISTANCE, 64.0, DESPAWN_DISTANCE),
			mob -> mob.getType().getCategory() == category && mob.entityTags().contains(TAG)
		).size();
		if (living >= cap) {
			return;
		}
		RandomSource random = level.getRandom();
		Kind kind = pick(monsters ? MONSTERS : ANIMALS, random);
		EntityType<? extends Mob> type = typeOf(kind);
		if (type == null) {
			return;
		}
		attempts++;
		for (int tries = 0; tries < 10; tries++) {
			double angle = random.nextDouble() * (Math.PI * 2.0);
			double distance = (monsters ? MIN_DISTANCE : ANIMAL_MIN_DISTANCE) + random.nextDouble() * (MAX_DISTANCE - (monsters ? MIN_DISTANCE : ANIMAL_MIN_DISTANCE));
			double[] spot = findSpot(level, player, player.getX() + Math.cos(angle) * distance, player.getZ() + Math.sin(angle) * distance, type.getHeight(), monsters);
			if (spot == null) {
				continue;
			}
			int size = kind.min() + (kind.max() > kind.min() ? random.nextInt(kind.max() - kind.min() + 1) : 0);
			SpawnGroupData group = null;
			int placed = 0;
			for (int i = 0; i < size && living + placed < cap; i++) {
				double[] at = i == 0 ? spot : findSpot(level, player, spot[0] + (random.nextDouble() - 0.5) * 8.0, spot[2] + (random.nextDouble() - 0.5) * 8.0, type.getHeight(), monsters);
				if (at == null) {
					continue;
				}
				Entity made = type.create(level, EntitySpawnReason.NATURAL);
				if (!(made instanceof Mob mob)) {
					continue;
				}
				mob.snapTo(at[0], at[1], at[2], random.nextFloat() * 360.0F, 0.0F);
				// Standing exactly on Skyrim's surface can clip its voxelised collision; start a little
				// higher if so and let the mob drop onto the ground.
				boolean room = level.noCollision(mob);
				for (double lift : new double[] { 0.15, 0.35, 0.7 }) {
					if (room) {
						break;
					}
					mob.snapTo(at[0], at[1] + lift, at[2], mob.getYRot(), 0.0F);
					room = level.noCollision(mob);
				}
				if (!room) {
					REJECTS.merge("no room for the mob", 1, Integer::sum);
					mob.discard();
					continue;
				}
				// Something under its feet within a block and a half, or it would appear in mid-air.
				if (!mob.isNoGravity() && level.noCollision(mob, mob.getBoundingBox().expandTowards(0.0, -1.5, 0.0))) {
					REJECTS.merge("nothing under its feet", 1, Integer::sum);
					mob.discard();
					continue;
				}
				mob.addTag(TAG);
				mob.setPersistenceRequired(); // Minecraft's own despawning would thin them out; upkeep() handles distance
				group = mob.finalizeSpawn(level, level.getCurrentDifficultyAt(mob.blockPosition()), EntitySpawnReason.NATURAL, group);
				level.addFreshEntityWithPassengers(mob);
				if (!mob.isRemoved()) {
					placed++;
					spawned++;
				}
			}
			if (placed > 0) {
				return;
			}
		}
	}

	private static Kind pick(List<Kind> kinds, RandomSource random) {
		int total = 0;
		for (Kind kind : kinds) {
			total += kind.weight();
		}
		int roll = random.nextInt(total);
		for (Kind kind : kinds) {
			roll -= kind.weight();
			if (roll < 0) {
				return kind;
			}
		}
		return kinds.getFirst();
	}

	/** Ground to stand on at (x, z), if a mob of this kind may spawn there: {x, feet y, z}. */
	private static double[] findSpot(ServerLevel level, ServerPlayer player, double x, double z, float height, boolean monster) {
		int bx = (int) Math.floor(x), bz = (int) Math.floor(z);
		if (!SkyCollision.isKnown(bx, (int) Math.floor(player.getY()), bz)) {
			return reject("geometry not sent there");
		}
		// The ground is looked for from a little above the player's head down, not from far above: a roof, cave ceiling or
		// treetop higher up is not somewhere a mob can stand (and Skyrim's one-sided surfaces would read as floor from above).
		double top = player.getY() + CEILING_REACH, bottom = player.getY() - BELOW;
		List<SkyTri> tris = new ArrayList<>();
		SkyCollision.trianglesNear(new AABB(x - 1.0, bottom, z - 1.0, x + 1.0, top, z + 1.0), tris);
		SkyRay.Hit ground = tris.isEmpty() ? null : SkyRay.cast(tris, x, top, z, x, bottom, z);
		if (ground == null) {
			return reject("no ground found");
		}
		if (ground.ny() < SkyTri.WALKABLE_NY || !ground.tri().walkable) {
			return reject("too steep / roof above");
		}
		double y = ground.y();
		// Under a roof of its own (a cave, a building) the player stands on a floor; never on what is above it.
		if (y > player.getY() + 2.0 && SkyrimMobs.covered(player.getX(), player.getY(), player.getZ(), 1.8F)) {
			return reject("above a covered player");
		}
		double surface = SkyWater.surfaceAt(bx, bz);
		if (!Double.isNaN(surface) && surface > y - 0.2) {
			return reject("water");
		}
		boolean covered = SkyrimMobs.covered(x, y, z, height);
		if (monster ? !(SkyTime.isNight() || covered) : (covered || !SkyTime.isDay() || !ground.tri().terrain)) {
			return reject(monster ? "monster: open ground by day" : "animal: night / covered / not terrain");
		}
		// Room for the mob, and a spot the player could walk or look to, not the far side of a wall or the top of a roof.
		List<SkyTri> room = new ArrayList<>();
		SkyCollision.trianglesNear(new AABB(x - 0.4, y + 0.1, z - 0.4, x + 0.4, y + height, z + 0.4), room);
		if (!room.isEmpty() && SkyRay.cast(room, x, y + 0.1, z, x, y + height, z) != null) {
			return reject("blocked headroom");
		}
		if (!SkyrimMobs.canSee(player, x, y + height, z)) {
			return reject("out of player's sight");
		}
		AABB nearNpcs = new AABB(x - NPC_BUFFER, y - 8.0, z - NPC_BUFFER, x + NPC_BUFFER, y + 8.0, z + NPC_BUFFER);
		if (!level.getEntitiesOfClass(SkyrimActorEntity.class, nearNpcs, npc -> npc.isAlive()).isEmpty()) {
			return reject("near NPCs");
		}
		// Anywhere people are going about their day (a town's streets, a farmstead, a camp, an inn's yard), not only right beside them.
		// Skyrim's NPCs are known out to 80 blocks, so a settlement the player stands outside of (its gate in sight) is still seen.
		AABB peopleAround = new AABB(x - PEOPLE_BUFFER, y - 16.0, z - PEOPLE_BUFFER, x + PEOPLE_BUFFER, y + 16.0, z + PEOPLE_BUFFER);
		if (!level.getEntitiesOfClass(SkyrimActorEntity.class, peopleAround, npc -> npc.isAlive() && npc.person() && !npc.hostileToPlayer()).isEmpty()) {
			return reject("friendly people nearby (a settlement)");
		}
		return new double[] { x, y, z };
	}
}
