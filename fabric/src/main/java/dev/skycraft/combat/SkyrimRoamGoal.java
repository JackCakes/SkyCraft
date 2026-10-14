package dev.skycraft.combat;

import dev.skycraft.link.SkyLink;
import dev.skycraft.world.SkyNav;
import dev.skycraft.world.SkyTri;
import java.util.EnumSet;
import java.util.List;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.MobCategory;
import net.minecraft.world.entity.ai.goal.Goal;

/**
 * Minecraft mobs wander about on Skyrim's ground as they would on Minecraft's. Minecraft's own
 * strolling picks a spot and asks the pathfinder for a way there, which can't see Skyrim's land, so
 * mobs stood where they spawned. This picks a spot a few blocks off that the mob can walk to in a
 * straight line over Skyrim's triangles (no wall, cliff or deep water on the way, ledges it can jump
 * allowed), and walks there with the mob's own move control, going round what's in the way ({@link SkyNav}).
 * About every six seconds an idle mob sets off, like Minecraft's.
 *
 * <p>Turn it off with the JVM argument {@code -Dskycraft.mobsRoam=false}.
 */
public final class SkyrimRoamGoal extends Goal {
	private static final boolean ENABLED = !"false".equalsIgnoreCase(System.getProperty("skycraft.mobsRoam"));
	private static final double MIN_DISTANCE = 3.0;
	private static final double MAX_DISTANCE = 8.0;
	private static final int TRIES = 8;
	private static final int MAX_TICKS = 400;
	private static final double ARRIVED = 0.8;

	private final Mob mob;
	private final SkyNav.Steer steer = new SkyNav.Steer(true);
	private double goalX, goalZ;
	private int ticks;
	private boolean finished;
	private double[] wanted;

	public SkyrimRoamGoal(Mob mob) {
		this.mob = mob;
		this.setFlags(EnumSet.of(Goal.Flag.MOVE));
	}

	/** Land animals and monsters. */
	public static boolean wants(Mob mob) {
		if (!ENABLED || !SkyrimMobs.eligible(mob)) {
			return false;
		}
		MobCategory category = mob.getType().getCategory();
		return category == MobCategory.MONSTER || category == MobCategory.CREATURE;
	}

	@Override
	public boolean canUse() {
		if (!SkyLink.active() || !(this.mob.level() instanceof ServerLevel) || this.mob.getTarget() != null || this.mob.isPassenger() || !this.mob.onGround()
			|| this.mob.isInWater() || !this.mob.getNavigation().isDone() || this.mob.getRandom().nextInt(60) != 0 || !SkyrimMobs.walker(this.mob)) {
			return false;
		}
		return choose();
	}

	/** A spot the mob can walk to, if one of a few tries finds it. */
	private boolean choose() {
		if (!SkyrimMobs.known(this.mob)) {
			return false;
		}
		double x = this.mob.getX(), y = this.mob.getY(), z = this.mob.getZ();
		List<SkyTri> tris = SkyrimMobs.trianglesAround(this.mob, MAX_DISTANCE + 1.5, SkyNav.BELOW + 3.0, SkyNav.ABOVE + 3.0);
		if (tris.isEmpty()) {
			return false;
		}
		var hazard = SkyrimMobs.hazard(this.mob);
		var random = this.mob.getRandom();
		double radius = this.mob.getBbWidth() * 0.5, height = this.mob.getBbHeight(), stepUp = this.mob.maxUpStep();
		for (int i = 0; i < TRIES; i++) {
			double angle = random.nextDouble() * (Math.PI * 2.0);
			double distance = MIN_DISTANCE + random.nextDouble() * (MAX_DISTANCE - MIN_DISTANCE);
			double tx = x + Math.cos(angle) * distance, tz = z + Math.sin(angle) * distance;
			if (SkyNav.canWalk(tris, hazard, x, y, z, radius, height, stepUp, tx, tz)) {
				this.goalX = tx;
				this.goalZ = tz;
				return true;
			}
		}
		return false;
	}

	@Override
	public boolean canContinueToUse() {
		return !this.finished && this.ticks < MAX_TICKS && SkyLink.active() && this.mob.getTarget() == null && !this.mob.isPassenger()
			&& this.mob.getNavigation().isDone();
	}

	@Override
	public void start() {
		this.ticks = 0;
		this.finished = false;
		this.wanted = null;
		this.steer.reset(this.mob.getRandom().nextBoolean());
	}

	@Override
	public void stop() {
		this.wanted = null;
	}

	// No @Override: not every version of Minecraft has this one. Minecraft's move control walks one
	// tick per order, so without it the mob would only move every other tick.
	public boolean requiresUpdateEveryTick() {
		return true;
	}

	@Override
	public void tick() {
		this.ticks++;
		double x = this.mob.getX(), y = this.mob.getY(), z = this.mob.getZ();
		if (Math.hypot(this.goalX - x, this.goalZ - z) < ARRIVED) {
			this.finished = true;
			return;
		}
		if (!this.mob.onGround()) {
			if (this.wanted != null) { // mid-jump or falling: carry on the way it was going
				this.mob.getMoveControl().setWantedPosition(this.wanted[0], this.wanted[1], this.wanted[2], 1.0);
			}
			return;
		}
		List<SkyTri> tris = SkyrimMobs.trianglesAround(this.mob, SkyNav.REACH, SkyNav.BELOW, SkyNav.ABOVE);
		double[] next = this.steer.next(
			tris, SkyrimMobs.hazard(this.mob), x, y, z, this.mob.getBbWidth() * 0.5, this.mob.getBbHeight(), this.mob.maxUpStep(), this.goalX, this.goalZ
		);
		if (next == null || this.steer.stuck()) {
			this.finished = true; // no way on from here, or going nowhere: it'll pick another spot later
			return;
		}
		this.wanted = next;
		this.mob.getMoveControl().setWantedPosition(next[0], next[1], next[2], 1.0);
	}
}
