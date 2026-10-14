package dev.skycraft.combat;

import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.phys.Vec3;

/**
 * Minecraft shoves whatever it hurts. For a melee blow that is just in the way: a mob that is hit
 * flies back out of reach of the fight, and Skyrim's NPCs have to chase it. A blow through here does
 * all the damage, armor, hurt sounds and anger of any other, and leaves the victim's movement alone.
 * Shots keep their knockback: it is what holds an enemy off an archer.
 */
final class NoShove {
	private NoShove() {
	}

	static boolean hurt(ServerLevel level, LivingEntity victim, DamageSource source, float damage) {
		Vec3 before = victim.getDeltaMovement();
		boolean hurt = victim.hurtServer(level, source, damage);
		if (hurt && !victim.isDeadOrDying()) {
			victim.setDeltaMovement(before);
		}
		return hurt;
	}
}
