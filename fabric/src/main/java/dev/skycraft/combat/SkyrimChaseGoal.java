package dev.skycraft.combat;

import dev.skycraft.link.SkyLink;
import dev.skycraft.world.SkyNav;
import dev.skycraft.world.SkyTri;
import java.util.List;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.ai.goal.Goal;
import net.minecraft.world.item.BowItem;
import net.minecraft.world.item.CrossbowItem;

/**
 * A Minecraft mob with something to fight walks at it over Skyrim's ground, whoever it is (a player,
 * one of Skyrim's NPCs). Minecraft's attack goals walk a mob by asking the pathfinder for a route, and
 * it can't see Skyrim's land, so a mob after a player stood still until the player came into its reach.
 * When Minecraft has no route of its own, this walks the mob at its target with the mob's own move
 * control, over Skyrim's triangles and round what's in the way ({@link SkyNav}); the mob's attack
 * goals do the rest once it's close. This goal holds no goal flags, so it never keeps those from running.
 *
 * <p>Turn it off with the JVM argument {@code -Dskycraft.mobsRoam=false}.
 */
public final class SkyrimChaseGoal extends Goal {
	private static final boolean ENABLED = !"false".equalsIgnoreCase(System.getProperty("skycraft.mobsRoam"));
	/** A target farther than this is left alone (Minecraft's own targeting gives up around here too). */
	private static final double GIVE_UP = 40.0;
	/** Archers stop walking at this distance if they can see the target, and shoot. */
	private static final double SHOOT_FROM = 11.0;

	private final Mob mob;
	/** Minecraft's route is given up on when the mob has moved less than this (blocks) over {@link #STALL_CHECK_TICKS} ticks. */
	private static final double STALL_DISTANCE = 0.3;
	private static final int STALL_CHECK_TICKS = 20;
	/** After giving up on Minecraft's route, this many ticks of walking by Skyrim's ground only, whatever the attack goals ask of the navigation. */
	private static final int TAKEOVER_TICKS = 60;
	/** Boxed in (every heading shut) this long, the mob jumps and shoves at its target rather than wait. */
	private static final int BOXED_TICKS = 20;

	private final SkyNav.Steer steer = new SkyNav.Steer(true);
	private double[] wanted;
	private double stallX = Double.NaN, stallZ;
	private int stallTicks, takeover, boxed;

	public SkyrimChaseGoal(Mob mob) {
		this.mob = mob;
	}

	public static boolean wants(Mob mob) {
		return ENABLED && SkyrimMobs.eligible(mob);
	}

	@Override
	public boolean canUse() {
		LivingEntity target = this.mob.getTarget();
		return SkyLink.active() && this.mob.level() instanceof ServerLevel && target != null && target.isAlive() && !this.mob.isPassenger()
			&& SkyrimMobs.walker(this.mob) && (this.mob.getNavigation().isDone() || this.takeover > 0 || this.stalled());
	}

	/**
	 * Minecraft's pathfinder works on blocks, and on Skyrim's land it can still find a "route" (over blocks the player placed, say)
	 * that the mob can't follow, and then it pushes at a hillside forever with this goal waiting for the route to finish. True when
	 * the mob has a route and hasn't got anywhere for a second.
	 */
	private boolean stalled() {
		double x = this.mob.getX(), z = this.mob.getZ();
		if (Double.isNaN(this.stallX)) {
			this.stallX = x;
			this.stallZ = z;
		}
		if (++this.stallTicks < STALL_CHECK_TICKS) {
			return false;
		}
		boolean stuck = Math.hypot(x - this.stallX, z - this.stallZ) < STALL_DISTANCE;
		this.stallX = x;
		this.stallZ = z;
		this.stallTicks = 0;
		if (stuck) {
			this.takeover = TAKEOVER_TICKS;
		}
		return stuck;
	}

	@Override
	public boolean canContinueToUse() {
		LivingEntity target = this.mob.getTarget();
		return SkyLink.active() && target != null && target.isAlive() && !this.mob.isPassenger() && this.mob.distanceToSqr(target) < GIVE_UP * GIVE_UP;
	}

	@Override
	public void start() {
		this.wanted = null;
		this.boxed = 0;
		this.steer.reset(this.mob.getRandom().nextBoolean());
	}

	@Override
	public void stop() {
		this.wanted = null;
	}

	// No @Override: not every version of Minecraft has this one (see SkyrimRoamGoal).
	public boolean requiresUpdateEveryTick() {
		return true;
	}

	@Override
	public void tick() {
		LivingEntity target = this.mob.getTarget();
		if (target == null) {
			return;
		}
		if (!this.mob.getNavigation().isDone()) {
			if (this.takeover <= 0 && !this.stalled()) {
				return; // Minecraft found a route of its own and is following it
			}
			this.mob.getNavigation().stop(); // it isn't getting anywhere on that route: Skyrim's ground takes over
		}
		if (this.takeover > 0) {
			this.takeover--;
		}
		double distanceSqr = this.mob.distanceToSqr(target);
		double closeEnough = this.mob.getBbWidth() * 1.2 + target.getBbWidth() * 0.5;
		if (distanceSqr < closeEnough * closeEnough) {
			return; // arrived: the mob's attack goals take over
		}
		var held = this.mob.getMainHandItem().getItem();
		if ((held instanceof BowItem || held instanceof CrossbowItem) && distanceSqr < SHOOT_FROM * SHOOT_FROM && this.mob.getSensing().hasLineOfSight(target)) {
			return; // archers keep their distance and shoot
		}
		if (!this.mob.onGround()) {
			if (this.wanted != null) { // mid-jump or falling: carry on the way it was going
				this.mob.getMoveControl().setWantedPosition(this.wanted[0], this.wanted[1], this.wanted[2], 1.0);
			}
			return;
		}
		List<SkyTri> tris = SkyrimMobs.trianglesAround(this.mob, SkyNav.REACH, SkyNav.BELOW, SkyNav.ABOVE);
		double[] next = this.steer.next(
			tris, SkyrimMobs.hazard(this.mob), this.mob.getX(), this.mob.getY(), this.mob.getZ(), this.mob.getBbWidth() * 0.5, this.mob.getBbHeight(),
			this.mob.maxUpStep(), target.getX(), target.getZ()
		);
		if (next == null) {
			// Boxed in (a cliff edge, a wall all round). It waits for the target to move, but not for ever: Skyrim's triangles can
			// shut every way when the real ground would let it by (a lip a step too high, a gap in the mesh), so after a second it
			// jumps and pushes at the target, and Minecraft's own movement and the collision settle what's really there.
			if (++this.boxed >= BOXED_TICKS && distanceSqr < 12.0 * 12.0) {
				this.boxed = 0;
				this.mob.getJumpControl().jump();
				this.mob.getMoveControl().setWantedPosition(target.getX(), target.getY(), target.getZ(), 1.0);
				this.steer.reset(this.mob.getRandom().nextBoolean());
			}
			return;
		}
		this.boxed = 0;
		if (this.steer.stuck()) { // steering but going nowhere: hop, and try the other way round
			this.mob.getJumpControl().jump();
			this.steer.reset(this.mob.getRandom().nextBoolean());
		}
		this.wanted = next;
		this.mob.getMoveControl().setWantedPosition(next[0], next[1], next[2], 1.0);
	}
}
