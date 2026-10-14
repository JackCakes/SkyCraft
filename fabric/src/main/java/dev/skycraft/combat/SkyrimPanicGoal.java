package dev.skycraft.combat;

import dev.skycraft.link.SkyLink;
import dev.skycraft.world.SkyNav;
import dev.skycraft.world.SkyTri;
import java.util.EnumSet;
import java.util.List;
import java.util.Set;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.ai.goal.Goal;

/**
 * Minecraft's farm animals run when they are hit: pigs, sheep, cows, chickens and horses bolt for a few
 * seconds, away from whatever struck them. Minecraft's own panic picks a spot and asks the pathfinder for
 * a way there, which can't see Skyrim's land, so on Skyrim's ground they just stood and took it. This does
 * the same thing the way {@link SkyrimRoamGoal} walks: it picks a spot away from the attacker that the mob
 * can reach over Skyrim's triangles and goes there with the mob's own move control, faster than a stroll.
 *
 * <p>Turn it off with the JVM argument {@code -Dskycraft.mobsPanic=false}.
 */
public final class SkyrimPanicGoal extends Goal {
	private static final boolean ENABLED = !"false".equalsIgnoreCase(System.getProperty("skycraft.mobsPanic"));
	/** The animals whose Minecraft brain has a panic goal. (Wolves, bees and the like fight back instead.) */
	private static final Set<String> PANICS = Set.of("chicken", "cow", "mooshroom", "pig", "sheep", "horse", "donkey", "mule");
	private static final double MIN_DISTANCE = 5.0;
	private static final double MAX_DISTANCE = 9.0;
	private static final int TRIES = 10;
	private static final int MAX_TICKS = 100;
	private static final double ARRIVED = 0.8;

	private final Mob mob;
	private final SkyNav.Steer steer = new SkyNav.Steer(true);
	private double goalX, goalZ;
	private int ticks;
	private boolean finished;
	private double[] wanted;
	private double speed = 1.25;

	public SkyrimPanicGoal(Mob mob) {
		this.mob = mob;
		this.setFlags(EnumSet.of(Goal.Flag.MOVE));
	}

	public static boolean wants(Mob mob) {
		return ENABLED && SkyrimMobs.eligible(mob) && PANICS.contains(BuiltInRegistries.ENTITY_TYPE.getKey(mob.getType()).getPath());
	}

	/** The thing that just hit the mob, if something did (Minecraft forgets the last blow after two seconds). */
	private Entity attacker() {
		DamageSource source = this.mob.getLastDamageSource();
		return source == null ? null : source.getEntity();
	}

	@Override
	public boolean canUse() {
		if (!SkyLink.active() || !(this.mob.level() instanceof ServerLevel) || this.mob.isPassenger() || !this.mob.onGround() || this.mob.isInWater()
			|| !SkyrimMobs.walker(this.mob) || !SkyrimMobs.known(this.mob)) {
			return false;
		}
		Entity attacker = attacker();
		return attacker != null && attacker != this.mob && choose(attacker);
	}

	private boolean choose(Entity attacker) {
		double x = this.mob.getX(), y = this.mob.getY(), z = this.mob.getZ();
		double awayX = x - attacker.getX(), awayZ = z - attacker.getZ();
		double length = Math.hypot(awayX, awayZ);
		double away = length > 1.0E-4 ? Math.atan2(awayZ, awayX) : this.mob.getRandom().nextDouble() * (Math.PI * 2.0);
		List<SkyTri> tris = SkyrimMobs.trianglesAround(this.mob, MAX_DISTANCE + 1.5, SkyNav.BELOW + 3.0, SkyNav.ABOVE + 3.0);
		if (tris.isEmpty()) {
			return false;
		}
		var hazard = SkyrimMobs.hazard(this.mob);
		var random = this.mob.getRandom();
		double radius = this.mob.getBbWidth() * 0.5, height = this.mob.getBbHeight(), stepUp = this.mob.maxUpStep();
		for (int i = 0; i < TRIES; i++) {
			// Straight away from the attacker first, then fanning out when something is in the way.
			double spread = (Math.PI / 3.0) * (1.0 + i / 4.0);
			double angle = away + (random.nextDouble() * 2.0 - 1.0) * spread;
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
		return !this.finished && this.ticks < MAX_TICKS && SkyLink.active() && !this.mob.isPassenger() && this.mob.isAlive();
	}

	@Override
	public void start() {
		this.ticks = 0;
		this.finished = false;
		this.wanted = null;
		this.steer.reset(this.mob.getRandom().nextBoolean());
		String type = BuiltInRegistries.ENTITY_TYPE.getKey(this.mob.getType()).getPath();
		this.speed = type.equals("cow") || type.equals("mooshroom") ? 2.0 : type.equals("chicken") ? 1.4 : 1.25;
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
		this.ticks++;
		double x = this.mob.getX(), y = this.mob.getY(), z = this.mob.getZ();
		if (Math.hypot(this.goalX - x, this.goalZ - z) < ARRIVED) {
			// Got there: still being chased (hit again a moment ago)? Pick another spot further on.
			Entity attacker = attacker();
			if (attacker != null && attacker != this.mob && this.ticks < MAX_TICKS - 20 && this.mob.onGround() && choose(attacker)) {
				this.steer.reset(this.mob.getRandom().nextBoolean());
				return;
			}
			this.finished = true;
			return;
		}
		if (!this.mob.onGround()) {
			if (this.wanted != null) { // mid-jump or falling: carry on the way it was going
				this.mob.getMoveControl().setWantedPosition(this.wanted[0], this.wanted[1], this.wanted[2], this.speed);
			}
			return;
		}
		List<SkyTri> tris = SkyrimMobs.trianglesAround(this.mob, SkyNav.REACH, SkyNav.BELOW, SkyNav.ABOVE);
		double[] next = this.steer.next(
			tris, SkyrimMobs.hazard(this.mob), x, y, z, this.mob.getBbWidth() * 0.5, this.mob.getBbHeight(), this.mob.maxUpStep(), this.goalX, this.goalZ
		);
		if (next == null || this.steer.stuck()) {
			this.finished = true;
			return;
		}
		this.wanted = next;
		this.mob.getMoveControl().setWantedPosition(next[0], next[1], next[2], this.speed);
	}
}
