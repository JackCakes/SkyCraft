package dev.skycraft.combat;

import dev.skycraft.world.SkyCollision;
import dev.skycraft.world.SkyNav;
import dev.skycraft.world.SkyRay;
import dev.skycraft.world.SkyTri;
import dev.skycraft.world.SkyWater;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.ai.control.MoveControl;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.AABB;

/** What {@link SkyrimRoamGoal} and {@link SkyrimChaseGoal} share: which mobs they're for, and what a mob sees of Skyrim around it. */
public final class SkyrimMobs {
	/**
	 * Mobs that don't walk the way a plain land mob does, or that run their own brain and walk
	 * targets: left alone, they would fight what the goals ask of them.
	 */
	private static final Set<String> OWN_MOVEMENT = Set.of(
		"villager", "piglin", "piglin_brute", "zoglin", "hoglin", "warden", "breeze", "creaking", "allay", "axolotl", "frog", "tadpole", "goat", "camel",
		"camel_husk", "sniffer", "armadillo", "happy_ghast", "strider", "enderman", "shulker", "ender_dragon", "wither", "giant"
	);

	private SkyrimMobs() {
	}

	/** A mob the goals may help at all (the property {@code skycraft.mobsRoam=false} turns them off elsewhere). */
	static boolean eligible(Mob mob) {
		return !OWN_MOVEMENT.contains(BuiltInRegistries.ENTITY_TYPE.getKey(mob.getType()).getPath());
	}

	/**
	 * Mobs that get about on foot with the plain move control: zombies, skeletons, spiders, creepers,
	 * cows, pigs, sheep, wolves, horses. Flyers, swimmers, hoppers (slimes, rabbits) and the rest
	 * have their own move control, which ours would only fight. A mob only has its final move control
	 * once it's built, so this is asked when a goal runs, not when it's added.
	 */
	static boolean walker(Mob mob) {
		return mob.getMoveControl().getClass() == MoveControl.class;
	}

	/** Skyrim's triangles around the mob: {@code reach} blocks sideways, {@code below} under its feet and {@code above} over its head. */
	static List<SkyTri> trianglesAround(Mob mob, double reach, double below, double above) {
		List<SkyTri> tris = new ArrayList<>();
		SkyCollision.trianglesNear(
			new AABB(mob.getX() - reach, mob.getY() - below, mob.getZ() - reach, mob.getX() + reach, mob.getY() + mob.getBbHeight() + above, mob.getZ() + reach), tris
		);
		return tris;
	}

	/** Water (Skyrim's, or Minecraft's, lava too) deep enough that a land mob shouldn't walk into it. */
	static SkyNav.Hazard hazard(Mob mob) {
		return (x, y, z) -> {
			double surface = SkyWater.surfaceAt((int) Math.floor(x), (int) Math.floor(z));
			if (!Double.isNaN(surface) && surface > y + 0.4) {
				return true;
			}
			Level level = mob.level();
			return !level.getFluidState(BlockPos.containing(x, y + 0.1, z)).isEmpty();
		};
	}

	static boolean known(Mob mob) {
		return SkyCollision.isKnown((int) Math.floor(mob.getX()), (int) Math.floor(mob.getY()), (int) Math.floor(mob.getZ()));
	}

	/** How far up {@link #covered} looks for a roof (Skyrim sends geometry about this far above the player). */
	private static final double COVER_HEIGHT = 24.0;

	/**
	 * Whether {@code from} can see {@code to}: no Skyrim wall, hill or roof between their eyes.
	 * Minecraft's own sight check only knows blocks, and there are none in Skyrim's land, so a mob
	 * would see through every wall.
	 */
	static boolean canSee(LivingEntity from, LivingEntity to) {
		double fx = from.getX(), fy = from.getEyeY(), fz = from.getZ();
		double tx = to.getX(), ty = to.getY() + to.getBbHeight() * 0.85, tz = to.getZ();
		List<SkyTri> tris = new ArrayList<>();
		SkyCollision.trianglesNear(
			new AABB(Math.min(fx, tx) - 0.5, Math.min(fy, ty) - 0.5, Math.min(fz, tz) - 0.5, Math.max(fx, tx) + 0.5, Math.max(fy, ty) + 0.5, Math.max(fz, tz) + 0.5), tris
		);
		return tris.isEmpty() || SkyRay.cast(tris, fx, fy, fz, tx, ty, tz) == null;
	}

	/** The same, from a living thing's eyes to a point. */
	static boolean canSee(LivingEntity from, double tx, double ty, double tz) {
		double fx = from.getX(), fy = from.getEyeY(), fz = from.getZ();
		List<SkyTri> tris = new ArrayList<>();
		SkyCollision.trianglesNear(
			new AABB(Math.min(fx, tx) - 0.5, Math.min(fy, ty) - 0.5, Math.min(fz, tz) - 0.5, Math.max(fx, tx) + 0.5, Math.max(fy, ty) + 0.5, Math.max(fz, tz) + 0.5), tris
		);
		return tris.isEmpty() || SkyRay.cast(tris, fx, fy, fz, tx, ty, tz) == null;
	}

	/**
	 * Skyrim geometry over a point: a roof, a cave ceiling, a tree. Minecraft sees the sky through all
	 * of it (there are no blocks), so this is what tells "under cover" from "in the open".
	 */
	public static boolean covered(double x, double feetY, double z, double height) {
		double from = feetY + height;
		List<SkyTri> tris = new ArrayList<>();
		SkyCollision.trianglesNear(new AABB(x - 0.3, from, z - 0.3, x + 0.3, from + COVER_HEIGHT, z + 0.3), tris);
		return !tris.isEmpty() && SkyRay.cast(tris, x, from, z, x, from + COVER_HEIGHT, z) != null;
	}
}
