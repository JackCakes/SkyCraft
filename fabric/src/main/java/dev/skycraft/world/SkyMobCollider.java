package dev.skycraft.world;

import dev.skycraft.link.SkyLink;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

/**
 * Minecraft mobs against Skyrim's world the way the player does it: Minecraft still works out every
 * velocity (walking, jumping, gravity, knockback), and {@link TriCollider} stops that movement
 * against Skyrim's exact triangles, so mobs follow slopes and slide along walls instead of climbing
 * voxel stair-steps. The movement is then collided with Minecraft's own blocks once more.
 *
 * <p>A mob outside the part of Skyrim's world that has been streamed in is held still, like the
 * player is until their ground arrives: without collision there it would fall out of the world.
 */
public final class SkyMobCollider {
	private SkyMobCollider() {
	}

	/** Mobs on a server or client that is linked to Skyrim (not the player: SkyCollider does that). */
	public static boolean applies(Entity entity) {
		return entity instanceof Mob && !entity.noPhysics && SkyLink.active();
	}

	public static Vec3 collide(Entity mob, Vec3 move) {
		if (!SkyCollision.isKnown((int) Math.floor(mob.getX()), (int) Math.floor(mob.getY()), (int) Math.floor(mob.getZ()))) {
			mob.setDeltaMovement(Vec3.ZERO);
			return Vec3.ZERO;
		}
		AABB box = mob.getBoundingBox();
		double step = mob.maxUpStep();
		List<SkyTri> tris = new ArrayList<>();
		SkyCollision.trianglesNear(box.expandTowards(move).inflate(1.0, 1.0 + step, 1.0), tris);
		if (tris.isEmpty()) {
			return move;
		}
		double[] r = TriCollider.resolve(
			tris, (box.minX + box.maxX) * 0.5, box.minY, (box.minZ + box.maxZ) * 0.5, box.getXsize() * 0.5, box.getYsize(), step, mob.onGround(),
			move.x, move.y, move.z
		);
		if (r[0] == move.x && r[1] == move.y && r[2] == move.z) {
			return move;
		}
		// The triangle pass can move the mob into a Minecraft block placed on the terrain.
		return Entity.collideBoundingBox(mob, new Vec3(r[0], r[1], r[2]), box, mob.level(), List.of());
	}
}
