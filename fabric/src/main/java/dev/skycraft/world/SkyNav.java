package dev.skycraft.world;

import java.util.List;

/**
 * Walking on Skyrim's ground for Minecraft mobs. Minecraft's own pathfinding works on blocks, and
 * Skyrim's land isn't made of blocks, so a mob can't find a path anywhere on it. This looks at
 * Skyrim's exact triangles instead ({@link TriCollider}, the same ones the mob collides with), one
 * short step at a time: is there ground ahead, is a wall in the way, is it a ledge the mob can jump
 * onto, is it a cliff or water the mob shouldn't walk into.
 *
 * <p>Nothing here knows about Minecraft's classes, so it can be tested on its own. All positions are
 * Minecraft's: feet at (x, y, z), y up.
 */
public final class SkyNav {
	/** The longest drop a mob will walk off, in blocks (Minecraft's own mobs avoid more than 3). */
	public static final double MAX_DROP = 3.0;
	/** The highest ledge a mob jumps up onto: a jump reaches about 1.25 blocks. */
	public static final double LEDGE = 1.1;
	/** How far ahead each step looks. Under a block, so that Minecraft's move control jumps in time (it only jumps when the target is within a block). */
	public static final double LEAD = 0.9;
	/** What the triangle list given to {@link Steer#next} must cover around the mob: this far sideways... */
	public static final double REACH = 2.5;
	/** ...this far below its feet... */
	public static final double BELOW = MAX_DROP + 0.7;
	/** ...and this far above its head. */
	public static final double ABOVE = LEDGE + 0.7;

	/** How far each sub-step of a step goes. A mob's own ticks move it about this much. */
	private static final double SUBSTEP = 0.3;
	/** A sub-step that gets at least this much of the way is not blocked: rough ground makes the collision slide a little. */
	private static final double BLOCKED = 0.85;

	/** How far round a point {@link #groundNear} looks when no triangle is under it: the hairline gaps where Skyrim's meshes meet. */
	private static final double SEAM = 0.12;

	/** Headings tried when the way ahead is shut, off the straight line. Up to and including right back, so a mob in a pocket can back out of it. */
	private static final double[] TURNS = { Math.PI / 8, Math.PI / 4, 3 * Math.PI / 8, Math.PI / 2, 3 * Math.PI / 4, 7 * Math.PI / 8, Math.PI };

	private SkyNav() {
	}

	/** Places a mob shouldn't walk into: water, lava, whatever the caller knows is bad to stand in. */
	@FunctionalInterface
	public interface Hazard {
		boolean at(double x, double y, double z);
	}

	/**
	 * Where a mob standing at (x, y, z) would stand after walking {@code (dx, dz)}: the height of its
	 * feet there, or NaN if it can't (a wall, a cliff, no ground known, a hazard).
	 */
	public static double step(
		List<SkyTri> tris, @org.jspecify.annotations.Nullable Hazard hazard, double x, double y, double z, double radius, double height, double stepUp,
		double dx, double dz
	) {
		if (tris.isEmpty() || Math.hypot(dx, dz) < 1.0e-9) {
			return Double.NaN;
		}
		double ground = walk(tris, x, y, z, radius, height, stepUp, dx, dz);
		if (Double.isNaN(ground)) {
			// A ledge the mob can jump up: ground ahead that's higher than a step but no higher than a jump.
			// Look for walls as if the mob were already standing on it (the ledge's own face doesn't count).
			// Its top must be something to stand on: a steep face is a wall the mob would only jump against.
			SkyTri top = topAt(tris, x + dx, z + dz, y + LEDGE);
			if (top == null || !top.walkable) {
				return Double.NaN;
			}
			double up = top.heightAt(x + dx, z + dz);
			if (up <= y + stepUp) {
				return Double.NaN;
			}
			ground = walk(tris, x, up, z, radius, height, stepUp, dx, dz);
			if (Double.isNaN(ground)) {
				return Double.NaN;
			}
		}
		if (hazard != null && hazard.at(x + dx, ground, z + dz)) {
			return Double.NaN;
		}
		return ground;
	}

	/** The triangle whose surface is highest at or below {@code limit} over (x, z), or null. */
	private static @org.jspecify.annotations.Nullable SkyTri topAt(List<SkyTri> tris, double x, double z, double limit) {
		SkyTri best = null;
		double bestHeight = Double.NEGATIVE_INFINITY;
		for (SkyTri t : tris) {
			if (t.minY > limit) {
				continue;
			}
			double h = t.heightAt(x, z);
			if (!Double.isNaN(h) && h <= limit && h > bestHeight) {
				best = t;
				bestHeight = h;
			}
		}
		return best;
	}

	/**
	 * One plain step, walked the way the mob really moves: in short sub-steps, each one collided from
	 * the height the mob has reached by then, and each one ending on the ground under it. Doing the
	 * whole step from the starting height would treat any slope that climbs more than the step height
	 * within it (about 27 degrees over {@link #LEAD}) as a wall, and cobbled roads and rubble, which are
	 * a run of small ups and downs, as a row of them: the mob really goes over those a tick at a time.
	 */
	private static double walk(List<SkyTri> tris, double x, double y, double z, double radius, double height, double stepUp, double dx, double dz) {
		double length = Math.hypot(dx, dz);
		int parts = Math.max(1, (int) Math.ceil(length / SUBSTEP));
		double sx = dx / parts, sz = dz / parts, sub = length / parts;
		double startY = y;
		for (int i = 0; i < parts; i++) {
			double[] moved = TriCollider.resolve(tris, x, y, z, radius, height, stepUp, true, sx, 0.0, sz);
			if (Math.hypot(moved[0], moved[2]) < sub * BLOCKED) {
				return Double.NaN; // a wall stops it
			}
			x += moved[0];
			z += moved[2];
			double ground = groundNear(tris, x, y, z, stepUp);
			if (Double.isNaN(ground) || y - ground > MAX_DROP || startY - ground > MAX_DROP) {
				return Double.NaN; // no ground, or a cliff
			}
			y = ground;
		}
		return y;
	}

	/**
	 * The ground under (x, y, z), or if no triangle is exactly there, the nearest ground a hand's breadth off. Skyrim's terrain is a
	 * patchwork of meshes whose edges don't always meet exactly; a mob stepping over one of those seams found "no ground", which
	 * read as a cliff, and it turned back from perfectly walkable land. A real cliff edge is much wider than a seam.
	 */
	private static double groundNear(List<SkyTri> tris, double x, double y, double z, double stepUp) {
		double ground = TriCollider.groundAt(tris, x, y, z, stepUp);
		if (!Double.isNaN(ground)) {
			return ground;
		}
		double best = Double.NaN;
		for (int i = 0; i < 4; i++) {
			double ox = (i & 1) == 0 ? SEAM : -SEAM, oz = (i & 2) == 0 ? SEAM : -SEAM;
			double g = TriCollider.groundAt(tris, x + ox, y, z + oz, stepUp);
			if (!Double.isNaN(g) && (Double.isNaN(best) || Math.abs(g - y) < Math.abs(best - y))) {
				best = g;
			}
		}
		return best;
	}

	/**
	 * Whether a mob at (x, y, z) can walk in a straight line to (tx, tz), step by step. The triangle list must cover the whole way.
	 */
	public static boolean canWalk(
		List<SkyTri> tris, @org.jspecify.annotations.Nullable Hazard hazard, double x, double y, double z, double radius, double height, double stepUp,
		double tx, double tz
	) {
		double length = Math.hypot(tx - x, tz - z);
		if (length < 1.0e-6) {
			return true;
		}
		double ux = (tx - x) / length, uz = (tz - z) / length;
		double walked = 0.0;
		while (walked < length - 1.0e-6) {
			double d = Math.min(LEAD, length - walked);
			double ground = step(tris, hazard, x, y, z, radius, height, stepUp, ux * d, uz * d);
			if (Double.isNaN(ground)) {
				return false;
			}
			x += ux * d;
			z += uz * d;
			y = ground;
			walked += d;
		}
		return true;
	}

	/**
	 * Steering toward a point, a step at a time, with no map of the place: straight when it can, and
	 * when something is in the way, along it (the side it last got by), in a turn it keeps for a moment so
	 * the mob doesn't flip back into the wall. That gets a mob round a rock, along a cliff edge, past a
	 * corner. It is not a path: a mob can still lose a maze to a dead end, and {@link #stuck()} says so.
	 */
	public static final class Steer {
		private static final int DETOUR_TICKS = 8;
		private static final int STUCK_WINDOW = 30;
		private static final double STUCK_DISTANCE = 0.4;

		private double detourHeading;
		private int detourTicks;
		private int side;
		private double markX, markZ;
		private int markTicks;
		private boolean stuck;

		public Steer(boolean rightFirst) {
			this.side = rightFirst ? 1 : -1;
		}

		public void reset(boolean rightFirst) {
			this.side = rightFirst ? 1 : -1;
			this.detourTicks = 0;
			this.markTicks = 0;
			this.stuck = false;
		}

		/** True once, for a mob that has barely moved in the last second and a half although it's been steering. */
		public boolean stuck() {
			return this.stuck;
		}

		/**
		 * The point to hand the mob's move control this tick: {x, y, z} a little way ahead on the ground,
		 * or null if there's no way forward from here. Call once per tick while steering.
		 */
		public double[] next(
			List<SkyTri> tris, @org.jspecify.annotations.Nullable Hazard hazard, double x, double y, double z, double radius, double height, double stepUp,
			double tx, double tz
		) {
			track(x, z);
			// Standing in a hazard (a puddle the mob was born in): get out, whichever way is clear.
			Hazard here = hazard != null && hazard.at(x, y, z) ? null : hazard;
			if (this.detourTicks > 0) {
				this.detourTicks--;
				double[] w = along(tris, here, x, y, z, radius, height, stepUp, this.detourHeading);
				if (w != null) {
					return w;
				}
				this.detourTicks = 0;
			}
			double base = Math.atan2(tz - z, tx - x);
			double[] straight = along(tris, here, x, y, z, radius, height, stepUp, base);
			if (straight != null) {
				return straight;
			}
			// Blocked: go round on one side, the one it last got by, from 22 degrees off up to 135. Only when
			// that whole side is closed does the other one get a try; mixing the two bounces it off the obstacle.
			for (int sign = 0; sign < 2; sign++) {
				double dir = sign == 0 ? this.side : -this.side;
				for (double turn : TURNS) {
					double heading = base + turn * dir;
					double[] w = along(tris, here, x, y, z, radius, height, stepUp, heading);
					if (w != null) {
						this.detourHeading = heading;
						this.detourTicks = DETOUR_TICKS;
						this.side = (int) dir;
						return w;
					}
				}
			}
			return null;
		}

		private double[] along(List<SkyTri> tris, Hazard hazard, double x, double y, double z, double radius, double height, double stepUp, double heading) {
			double dx = Math.cos(heading) * LEAD, dz = Math.sin(heading) * LEAD;
			double ground = step(tris, hazard, x, y, z, radius, height, stepUp, dx, dz);
			return Double.isNaN(ground) ? null : new double[] { x + dx, ground, z + dz };
		}

		private void track(double x, double z) {
			if (this.markTicks == 0) {
				this.markX = x;
				this.markZ = z;
			}
			this.stuck = false;
			if (++this.markTicks >= STUCK_WINDOW) {
				this.stuck = Math.hypot(x - this.markX, z - this.markZ) < STUCK_DISTANCE;
				this.markTicks = 0;
				if (this.stuck) {
					this.side = -this.side; // try the other way round
					this.detourTicks = 0;
				}
			}
		}
	}
}
