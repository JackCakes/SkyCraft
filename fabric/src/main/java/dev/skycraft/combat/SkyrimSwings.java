package dev.skycraft.combat;

import dev.skycraft.SkyCraft;
import dev.skycraft.link.Proto;
import dev.skycraft.world.SkyClip;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Optional;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

/**
 * The player hits Minecraft's mobs with Skyrim's own weapons, in Skyrim's control mode. There Skyrim
 * swings the sword, draws the bow and moves the camera, and Minecraft's player only rides along, so
 * Minecraft never sees an attack. Skyrim tells it when its player swings or shoots
 * ({@link Proto#IN_SWING}: what kind, how hard by Skyrim's reckoning, and where the player faces) and the
 * mobs in the way take the hit through Minecraft's own damage, so armour, knockback, hurt sounds and
 * death work as they do for any hit of the player's.
 *
 * <ul>
 * <li>A melee swing hits the nearest mob in front of the player within arm's reach; a power attack
 * hits up to three. Skyrim's walls and hills stop a blow as they would a player's eyes.
 * <li>A bow or crossbow shot is traced along the way the player faces, since Skyrim's arrow flies in
 * Skyrim and never meets a Minecraft mob: the first mob on the line, in the open, takes it, harder the
 * further the bow was drawn.
 * </ul>
 */
public final class SkyrimSwings {
	/** Skyrim weapon damage -> Minecraft damage for a hit on a mob (an iron sword's 7 is near Minecraft's 6). */
	private static final float SKYRIM_TO_MC = 0.8F;
	private static final double MELEE_REACH = 3.4;
	/** The mob must be within this cosine of the way the player faces (about 63 degrees either side). */
	private static final double MELEE_CONE = 0.45;
	private static final double ARROW_RANGE = 40.0;

	private SkyrimSwings() {
	}

	/** Runs on the server thread. {@code yaw} and {@code pitch} are Minecraft's, in degrees; {@code charge} 0 to 1. */
	public static void swing(ServerPlayer player, int kind, float skyrimDamage, float yaw, float pitch, float charge) {
		if (!player.isAlive() || player.isSpectator() || skyrimDamage <= 0.0F) {
			return;
		}
		ServerLevel level = player.level();
		double yawRad = Math.toRadians(yaw), pitchRad = Math.toRadians(pitch);
		Vec3 look = new Vec3(-Math.sin(yawRad) * Math.cos(pitchRad), -Math.sin(pitchRad), Math.cos(yawRad) * Math.cos(pitchRad));
		Vec3 eye = player.getEyePosition();
		float damage = skyrimDamage * SKYRIM_TO_MC;
		if (kind == Proto.SWING_ARROW) {
			arrow(player, level, eye, look, damage * (0.25F + 0.75F * Math.max(0.0F, Math.min(1.0F, charge))));
		} else {
			melee(player, level, eye, look, damage, kind == Proto.SWING_POWER ? 3 : 1);
		}
	}

	/**
	 * Skyrim hurt the stand-in of a mob (see {@link MobProxies}). If the player did it (a spell, say, cast at the mob they locked on to) the
	 * mob takes it as the player's own hit, so it drops what it drops and turns on them. Otherwise an NPC did it, and the mob takes the
	 * blow as from the NPC nearest it, so it turns on that NPC the way it would on one that hit it.
	 */
	public static void proxyHurt(ServerPlayer player, int mobId, float skyrimDamage, boolean byPlayer) {
		ServerLevel level = player.level();
		if (!(level.getEntity(mobId) instanceof Mob mob) || !mob.isAlive() || skyrimDamage <= 0.0F) {
			if (proxyLogs++ < 60) {
				SkyCraft.LOG.info("SkyCraft: Skyrim hurt stand-in of mob {} for {}, but there is no live mob to take it", mobId, skyrimDamage);
			}
			return;
		}
		if (byPlayer) {
			boolean hurt = mob.hurtServer(level, level.damageSources().playerAttack(player), skyrimDamage * SKYRIM_TO_MC);
			if (proxyLogs++ < 60) {
				SkyCraft.LOG.info("SkyCraft: Skyrim player hurt {} for {}: {}", mob.getName().getString(), skyrimDamage * SKYRIM_TO_MC, hurt ? "landed" : "refused");
			}
			return;
		}
		SkyrimActorEntity attacker = null;
		double best = 12.0 * 12.0;
		for (SkyrimActorEntity npc : level.getEntitiesOfClass(SkyrimActorEntity.class, mob.getBoundingBox().inflate(12.0))) {
			double d = npc.isAlive() ? npc.distanceToSqr(mob) : Double.MAX_VALUE;
			if (d < best) {
				best = d;
				attacker = npc;
			}
		}
		var source = attacker != null ? level.damageSources().mobAttack(attacker) : level.damageSources().generic();
		// An NPC at arm's length struck it; one further off shot it (and the shot keeps its knockback).
		boolean closeBlow = attacker != null && Math.sqrt(best) < 3.5;
		boolean hurt = closeBlow ? NoShove.hurt(level, mob, source, skyrimDamage * SKYRIM_TO_MC) : mob.hurtServer(level, source, skyrimDamage * SKYRIM_TO_MC);
		if (proxyLogs++ < 60) {
			SkyCraft.LOG.info("SkyCraft: Skyrim NPC hurt {} for {}: {}", mob.getName().getString(), skyrimDamage * SKYRIM_TO_MC, hurt ? "landed" : "refused");
		}
	}

	private static int proxyLogs;

	private static void melee(ServerPlayer player, ServerLevel level, Vec3 eye, Vec3 look, float damage, int most) {
		List<Mob> inReach = new ArrayList<>();
		for (Mob mob : level.getEntitiesOfClass(Mob.class, player.getBoundingBox().inflate(MELEE_REACH + 2.0))) {
			if (!mob.isAlive() || mob.isAlliedTo(player)) {
				continue;
			}
			AABB box = mob.getBoundingBox();
			double gap = Math.sqrt(box.distanceToSqr(eye));
			if (gap > MELEE_REACH) {
				continue;
			}
			Vec3 to = box.getCenter().subtract(eye);
			double length = to.length();
			if (gap > 1.0 && length > 1.0E-6 && to.scale(1.0 / length).dot(look) < MELEE_CONE) {
				continue;
			}
			if (SkyrimMobs.canSee(player, mob)) {
				inReach.add(mob);
			}
		}
		inReach.sort(Comparator.comparingDouble(mob -> mob.distanceToSqr(player)));
		for (int i = 0; i < Math.min(most, inReach.size()); i++) {
			Mob mob = inReach.get(i);
			boolean hurt = NoShove.hurt(level, mob, level.damageSources().playerAttack(player), damage);
			SkyCraft.LOG.info("SkyCraft: Skyrim swing hit {} for {}{}", mob.getName().getString(), damage, hurt ? "" : " (no effect)");
		}
	}

	private static void arrow(ServerPlayer player, ServerLevel level, Vec3 eye, Vec3 look, float damage) {
		Vec3 end = eye.add(look.scale(ARROW_RANGE));
		Mob best = null;
		double bestDistance = Double.MAX_VALUE;
		for (Mob mob : level.getEntitiesOfClass(Mob.class, new AABB(eye, end).inflate(1.0))) {
			if (!mob.isAlive() || mob.isAlliedTo(player)) {
				continue;
			}
			Optional<Vec3> at = mob.getBoundingBox().inflate(0.2).clip(eye, end);
			if (at.isEmpty()) {
				continue;
			}
			double distance = eye.distanceToSqr(at.get());
			if (distance < bestDistance && SkyClip.cast(eye, at.get()) == null) {
				best = mob;
				bestDistance = distance;
			}
		}
		if (best != null) {
			boolean hurt = best.hurtServer(level, level.damageSources().playerAttack(player), damage);
			SkyCraft.LOG.info("SkyCraft: Skyrim arrow hit {} for {}{}", best.getName().getString(), damage, hurt ? "" : " (no effect)");
		}
	}
}
