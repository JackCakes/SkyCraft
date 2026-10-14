package dev.skycraft.mixin;

import dev.skycraft.combat.AttackSkyrimActorsGoal;
import dev.skycraft.combat.SkyrimChaseGoal;
import dev.skycraft.combat.SkyrimNoticeGoal;
import dev.skycraft.combat.SkyrimPanicGoal;
import dev.skycraft.combat.SkyrimRoamGoal;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.ai.goal.GoalSelector;
import net.minecraft.world.level.Level;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Minecraft mobs live in Skyrim's world: hostile ones go after Skyrim's NPCs (their invisible
 * stand-ins) when no player is in reach, and every mob that walks gets about on Skyrim's ground,
 * wandering when idle and walking at whatever it's fighting (SkyrimRoamGoal, SkyrimChaseGoal),
 * since Minecraft's own pathfinding can't see Skyrim's land. Mobs also take notice of Skyrim's NPCs
 * the way they do a nearby player, and look at them (SkyrimNoticeGoal).
 */
@Mixin(Mob.class)
public abstract class MobSkyrimFoesMixin {
	@Shadow
	@Final
	protected GoalSelector goalSelector;

	@Inject(method = "<init>", at = @At("TAIL"))
	private void skycraft$attackSkyrimActors(EntityType<? extends Mob> type, Level level, CallbackInfo ci) {
		Mob self = (Mob) (Object) this;
		if (AttackSkyrimActorsGoal.wants(self)) {
			this.goalSelector.addGoal(6, new AttackSkyrimActorsGoal(self));
		}
		if (SkyrimChaseGoal.wants(self)) {
			this.goalSelector.addGoal(6, new SkyrimChaseGoal(self));
		}
		if (SkyrimNoticeGoal.wants(self)) {
			this.goalSelector.addGoal(8, new SkyrimNoticeGoal(self));
		}
		if (SkyrimPanicGoal.wants(self)) {
			this.goalSelector.addGoal(1, new SkyrimPanicGoal(self));
		}
		if (SkyrimRoamGoal.wants(self)) {
			this.goalSelector.addGoal(5, new SkyrimRoamGoal(self));
		}
	}
}
