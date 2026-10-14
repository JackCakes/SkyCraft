package dev.skycraft.mixin;

import dev.skycraft.world.SkyCollision;
import dev.skycraft.world.SkyMobCollider;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Minecraft mobs collide with Skyrim's exact triangles like the player does (SkyMobCollider). While
 * vanilla works out a mob's movement the voxel version of Skyrim's geometry is left out of it
 * (BlockCollisionsMixin); everything else the mob's AI asks about still sees the voxels.
 */
@Mixin(Entity.class)
public abstract class MobCollideMixin {
	@Inject(method = "collide", at = @At("HEAD"))
	private void skycraft$beginMobCollide(Vec3 movement, CallbackInfoReturnable<Vec3> cir) {
		SkyCollision.setSmoothMove(SkyMobCollider.applies((Entity) (Object) this));
	}

	@Inject(method = "collide", at = @At("RETURN"), cancellable = true)
	private void skycraft$endMobCollide(Vec3 movement, CallbackInfoReturnable<Vec3> cir) {
		if (!SkyCollision.smoothMove()) {
			return;
		}
		SkyCollision.setSmoothMove(false);
		cir.setReturnValue(SkyMobCollider.collide((Entity) (Object) this, cir.getReturnValue()));
	}
}
