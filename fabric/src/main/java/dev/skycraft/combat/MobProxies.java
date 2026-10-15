package dev.skycraft.combat;

import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Collection;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.MobCategory;

/**
 * Gives Minecraft's mobs a presence Skyrim understands. Skyrim's combat AI, its lock-on mods and its spells can only deal with an
 * actor, so every mob that matters is reported to Skyrim ({@link Proto#EV_MOB_PROXY}), which keeps an invisible stand-in actor where
 * the mob stands. A mob gets one for either of two reasons:
 *
 * <ul>
 * <li>It is a hostile mob near one of Skyrim's NPCs. The NPCs fight the stand-in exactly as they would fight a wolf or a bandit:
 * weapon out, closing in, blocking, shooting, civilians running.
 * <li>It is near the player, whatever it is. The stand-in is what the player locks on to and what Skyrim's spells hit. These are
 * flagged {@link Proto#MOB_PROXY_TARGET_ONLY} when no NPC has business with the mob, so Skyrim's NPCs leave them alone.
 * </ul>
 *
 * What Skyrim does to a stand-in comes back as damage to the real mob ({@link Proto#IN_PROXY_HURT}), from the player when the player
 * did it; what the mob does to an NPC is Minecraft's, as before.
 *
 * <p>An NPC Skyrim hasn't put into such a fight is still handled by {@link NpcFightsBack}, so if Skyrim can't make stand-ins nothing
 * is lost. Turn this off with {@code -Dskycraft.mobProxies=false}. {@code -Dskycraft.mobTargets=hostile} gives the player's
 * stand-ins to hostile mobs only (the default is every mob), {@code =off} to none.
 */
final class MobProxies {
	private static final boolean ENABLED = !"false".equalsIgnoreCase(System.getProperty("skycraft.mobProxies"));
	private static final String TARGETS = System.getProperty("skycraft.mobTargets", "all").toLowerCase(java.util.Locale.ROOT);
	/** Mobs this close to an NPC (blocks) get a stand-in. */
	private static final double RANGE = 30.0;
	/** Mobs this close to the player (blocks) get one to be locked on to and hit by spells. */
	private static final double PLAYER_RANGE = 40.0;
	/** At most this many mob bodies at once, the ones that matter most first. A body is a collision box, cheap beside the actor stand-in it replaces. */
	private static final int MAX = 48;
	private static final Set<Integer> LIVE = new HashSet<>();
	/** Mob id -> game time its name was last sent. Skyrim names each stand-in after its mob (it would otherwise show as a skeever). */
	private static final Map<Integer, Long> NAME_SENT = new HashMap<>();
	private static final long NAME_RESEND_TICKS = 100L;

	private MobProxies() {
	}

	/** One mob that is to have a stand-in. */
	private static final class Want {
		final Mob mob;
		/** Skyrim's NPCs may fight it (a hostile mob near one of them). */
		boolean npcFight;
		/** Squared distance to whoever it matters to (the nearest NPC it is a threat to, or the player). */
		double near = Double.MAX_VALUE;

		Want(Mob mob) {
			this.mob = mob;
		}
	}

	static void tick(ServerLevel level, Collection<SkyrimActorEntity> actors, List<ServerPlayer> players) {
		final boolean forPlayer = !"off".equals(TARGETS) && !players.isEmpty();
		if (!ENABLED || !SkyLink.active() || (actors.isEmpty() && !forPlayer)) {
			if (SkyLink.active()) {
				for (Integer id : LIVE) {
					SkyLink.pushEvent(Proto.EV_MOB_PROXY, id, 0.0F, 0.0F, 0.0F, 0.0F, Proto.MOB_PROXY_GONE); // nobody to need them: the stand-ins go, not left standing
				}
			}
			LIVE.clear();
			NAME_SENT.clear();
			return;
		}
		Map<Mob, Want> found = new LinkedHashMap<>();
		for (SkyrimActorEntity npc : actors) {
			if (!npc.isAlive()) {
				continue;
			}
			for (Mob mob : level.getEntitiesOfClass(Mob.class, npc.getBoundingBox().inflate(RANGE, 6.0, RANGE))) {
				if (!mob.isAlive() || !(mob.getType().getCategory() == MobCategory.MONSTER || mob.getTarget() instanceof SkyrimActorEntity)) {
					continue;
				}
				Want want = found.computeIfAbsent(mob, Want::new);
				want.npcFight = true;
				want.near = Math.min(want.near, mob.distanceToSqr(npc));
			}
		}
		if (forPlayer) {
			final boolean hostileOnly = "hostile".equals(TARGETS);
			for (ServerPlayer player : players) {
				if (player.level() != level || !player.isAlive() || player.isSpectator()) {
					continue;
				}
				for (Mob mob : level.getEntitiesOfClass(Mob.class, player.getBoundingBox().inflate(PLAYER_RANGE, 16.0, PLAYER_RANGE))) {
					if (!mob.isAlive() || mob.isInvisible() || mob.isAlliedTo(player) || mob.hasPassenger(player)) {
						continue;
					}
					if (hostileOnly && mob.getType().getCategory() != MobCategory.MONSTER) {
						continue;
					}
					Want want = found.computeIfAbsent(mob, Want::new);
					want.near = Math.min(want.near, mob.distanceToSqr(player));
				}
			}
		}
		// Hysteresis: a mob that already has a stand-in keeps it until it is well out of range (and sorts ahead of newcomers
		// below), so mobs drifting across the edge or a busier moment don't swap stand-ins in and out (no stable lock-on).
		if (forPlayer) {
			for (Integer id : LIVE) {
				if (!(level.getEntity(id) instanceof Mob mob) || !mob.isAlive() || found.containsKey(mob)) {
					continue;
				}
				// A mob that has a stand-in keeps it for as long as it lives and is loaded, however far it has wandered: the stand-in goes
				// when the mob does, not when it passes some distance. (Distance only decides who is dropped if there are more than MAX.)
				Want want = found.computeIfAbsent(mob, Want::new);
				for (ServerPlayer player : players) {
					if (player.level() == level) {
						want.near = Math.min(want.near, mob.distanceToSqr(player));
					}
				}
			}
		}
		// Mobs that are after one of Skyrim's NPCs come first (a skeleton shooting from across a field must get a stand-in, so
		// the NPC it shoots can turn on it, before a mob that is only standing near one), then the nearest to whoever needs it.
		List<Want> ordered = new ArrayList<>(found.values());
		ordered.sort((a, b) -> {
			boolean aFoe = a.mob.getTarget() instanceof SkyrimActorEntity, bFoe = b.mob.getTarget() instanceof SkyrimActorEntity;
			if (aFoe != bFoe) {
				return aFoe ? -1 : 1;
			}
			boolean aLive = LIVE.contains(a.mob.getId()), bLive = LIVE.contains(b.mob.getId());
			return aLive != bLive ? (aLive ? -1 : 1) : Double.compare(a.near, b.near);
		});
		Map<Integer, Want> wanted = new LinkedHashMap<>();
		for (int i = 0; i < Math.min(MAX, ordered.size()); i++) {
			wanted.put(ordered.get(i).mob.getId(), ordered.get(i));
		}
		final long now = level.getGameTime();
		for (Want want : wanted.values()) {
			Mob mob = want.mob;
			Long lastName = NAME_SENT.get(mob.getId());
			final boolean roomy = SkyLink.eventRingFree() > 160; // Skyrim is behind: position updates matter, names and vitals can wait
			if (roomy && (lastName == null || now - lastName >= NAME_RESEND_TICKS)) {  // before the position, so a new stand-in is named at once
				sendName(mob);
				NAME_SENT.put(mob.getId(), now);
			}
			// The mob's vitals go first, so Skyrim knows the mob's width before it makes the stand-in (it is sized from width and height). Its
			// health (1:1: Skyrim's lock-on bar shows exactly this), where its eyes are, and which way it is moving.
			if (roomy) {
				var motion = mob.getDeltaMovement();
				SkyLink.pushEvent(Proto.EV_MOB_VITALS, mob.getId(), mob.getHealth(), mob.getMaxHealth(), mob.getEyeHeight(), mob.getBbWidth(),
					Float.floatToRawIntBits((float) (motion.x * 20.0)), Float.floatToRawIntBits((float) (motion.z * 20.0)));
			}
			// The mob's height goes in the weapon field (the stand-in is sized to it, so the head is where the mob's is).
			SkyLink.pushEvent(Proto.EV_MOB_PROXY, mob.getId(), (float) mob.getX(), (float) mob.getY(), (float) mob.getZ(), mob.getYRot(),
				want.npcFight ? 0 : Proto.MOB_PROXY_TARGET_ONLY, Float.floatToRawIntBits(mob.getBbHeight()));
		}

		for (Integer id : new ArrayList<>(LIVE)) {
			if (!wanted.containsKey(id)) {
				SkyLink.pushEvent(Proto.EV_MOB_PROXY, id, 0.0F, 0.0F, 0.0F, 0.0F, Proto.MOB_PROXY_GONE); // dead or out of range: Skyrim drops the stand-in
			}
		}
		LIVE.clear();
		LIVE.addAll(wanted.keySet());
		NAME_SENT.keySet().retainAll(wanted.keySet());
	}

	/** Tells Skyrim the mob's name (its custom name if it has one), up to 24 plain ASCII characters. */
	private static void sendName(Mob mob) {
		byte[] raw = new byte[24];
		String name = mob.getName().getString();
		if (!mob.hasCustomName() && !name.isEmpty()) {
			name = Character.toUpperCase(name.charAt(0)) + name.substring(1); // some mobs' names come through lowercase ("chicken")
		}
		int n = 0;
		for (int i = 0; i < name.length() && n < raw.length; i++) {
			char c = name.charAt(i);
			raw[n++] = (byte) (c >= 32 && c < 127 ? c : '?');
		}
		ByteBuffer b = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN);
		SkyLink.pushEvent(Proto.EV_MOB_NAME, mob.getId(), Float.intBitsToFloat(b.getInt()), Float.intBitsToFloat(b.getInt()),
			Float.intBitsToFloat(b.getInt()), Float.intBitsToFloat(b.getInt()), b.getInt(), b.getInt());
	}
}
