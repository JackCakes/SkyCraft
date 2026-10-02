package dev.skycraft.magic;

import dev.skycraft.SkyCraft;
import dev.skycraft.combat.SkyCombat;
import dev.skycraft.combat.SkyrimActorEntity;
import java.util.HashMap;
import java.util.Map;
import java.util.UUID;
import net.fabricmc.fabric.api.command.v2.CommandRegistrationCallback;
import net.minecraft.commands.Commands;
import net.minecraft.core.particles.ParticleTypes;
import net.minecraft.network.chat.Component;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.phys.Vec3;

/**
 * First Minecraft-side ranged magic prototype.
 *
 * <p>The spell resolver deliberately targets SkyCombat's mirrored Skyrim actors, so magic uses the
 * same proven damage bridge as Minecraft weapons and projectiles. Commands are the initial test
 * harness; item/right-click bindings can call {@link #cast} without duplicating combat logic.
 */
public final class RangedMagic {
	private static final double RANGE = 28.0;
	private static final long COOLDOWN_TICKS = 8;
	private static final Map<UUID, Long> LAST_CAST = new HashMap<>();

	private RangedMagic() {}

	public static void init() {
		CommandRegistrationCallback.EVENT.register((dispatcher, registryAccess, environment) -> {
			dispatcher.register(Commands.literal("skymagic")
				.then(Commands.literal("fire").executes(context ->
					cast(context.getSource().getPlayerOrException(), Spell.FIRE) ? 1 : 0))
				.then(Commands.literal("frost").executes(context ->
					cast(context.getSource().getPlayerOrException(), Spell.FROST) ? 1 : 0)));
		});
	}

	/** Cast one ranged spell from a Minecraft player toward their crosshair. */
	public static boolean cast(ServerPlayer player, Spell spell) {
		ServerLevel level = player.level();
		long now = level.getGameTime();
		long previous = LAST_CAST.getOrDefault(player.getUUID(), Long.MIN_VALUE / 2);
		if (now - previous < COOLDOWN_TICKS) {
			return false;
		}

		SkyrimActorEntity target = SkyCombat.findTarget(player, RANGE);
		if (target == null) {
			player.sendSystemMessage(Component.literal("No Skyrim target in spell range."));
			return false;
		}

		LAST_CAST.put(player.getUUID(), now);
		Vec3 from = player.getEyePosition();
		Vec3 to = target.getBoundingBox().getCenter();
		spawnTrail(level, from, to, spell);

		boolean hurt = target.hurtServer(level, level.damageSources().magic(), spell.damage);
		if (hurt && spell == Spell.FIRE) {
			target.igniteForSeconds(3.0F);
		}
		SkyCraft.LOG.info("SkyCraft magic: {} cast {} at {} for {}", player.getName().getString(), spell.id,
			target.getName().getString(), spell.damage);
		return hurt;
	}

	private static void spawnTrail(ServerLevel level, Vec3 from, Vec3 to, Spell spell) {
		Vec3 delta = to.subtract(from);
		double length = delta.length();
		int steps = Math.max(2, (int) Math.ceil(length * 3.0));
		for (int i = 1; i <= steps; i++) {
			Vec3 p = from.add(delta.scale((double) i / steps));
			level.sendParticles(spell == Spell.FIRE ? ParticleTypes.FLAME : ParticleTypes.SNOWFLAKE,
				p.x, p.y, p.z, 1, 0.015, 0.015, 0.015, 0.0);
		}
	}

	public enum Spell {
		FIRE("fire", 5.0F),
		FROST("frost", 4.0F);

		final String id;
		final float damage;

		Spell(String id, float damage) {
			this.id = id;
			this.damage = damage;
		}
	}
}
