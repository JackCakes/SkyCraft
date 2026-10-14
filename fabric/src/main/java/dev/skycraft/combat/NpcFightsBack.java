package dev.skycraft.combat;

import dev.skycraft.SkyCraft;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import java.util.ArrayList;
import java.util.Collection;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Set;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.MobCategory;

/**
 * Skyrim's NPCs fight the Minecraft mobs that come for them, the way they would fight a Skyrim enemy.
 * Skyrim's own AI has no idea a zombie exists, so this does what its combat would, step by step:
 *
 * <ol>
 * <li>The moment a hostile mob is about (seen, or already after the NPC), the NPC goes on guard:
 * Skyrim is told ({@link Proto#EV_NPC_STANCE}) to take the weapon out and turn the NPC to face the
 * danger, and to keep turning as it moves. It stays on guard, weapon out, for as long as any mob is
 * around and for a good few seconds after, the way Skyrim's NPCs linger after a fight.
 * <li>Whoever is hitting the NPC is dealt with first; otherwise the nearest. A blow lands on the first
 * mob in reach, and from an archer's range on one in the open. Each blow is Skyrim's own attack
 * animation ({@link Proto#EV_NPC_SWING}); its damage comes from the weapon in the NPC's hand
 * (a guard's sword hits harder than a farmer's fists) and is dealt through Minecraft's damage
 * ({@code mobAttack}: armour, knockback, the mob turning on the NPC in return).
 * <li>When the last mob is dead or gone, the NPC stands down: the weapon goes away.
 * </ol>
 *
 * <p>Anyone not a fighter (a shopkeeper, a farmer) still defends itself against a mob that is on top of it.
 * An NPC in a fight Skyrim runs itself is left alone. Turn it off with {@code -Dskycraft.npcsFightBack=false}.
 */
final class NpcFightsBack {
	private static final boolean ENABLED = !"false".equalsIgnoreCase(System.getProperty("skycraft.npcsFightBack"));
	/** How far past its own body an NPC's blow reaches, in blocks. */
	private static final double REACH = 2.4;
	/** A hostile mob this close makes a fighter take out its weapon, in sight or not (it hears it). */
	private static final double ALERT = 22.0;
	/** A mob after the NPC makes any NPC defend itself from this far. */
	private static final double SELF_DEFENCE = 8.0;
	/** Anyone shorter than this (children, small creatures) keeps out of it. */
	private static final float MIN_HEIGHT = 1.2F;
	/** Ticks the weapon takes to come out before the first blow. */
	private static final long DRAW_TICKS = 22;
	/** Ticks without a mob about before the weapon goes away (Skyrim's NPCs keep it out a good while after a fight). */
	private static final long STAND_DOWN_TICKS = 160;
	/** How often a ready NPC is told where to look, so it keeps facing the danger. */
	private static final long FACE_EVERY = 4;
	/** An archer shoots from this far. */
	private static final double BOW_RANGE = 20.0;
	/** Ticks from a swing starting to it landing (melee), and from the string being drawn to the arrow arriving (bow). */
	private static final long MELEE_DELAY = 7, BOW_DELAY = 22;
	/** If Skyrim keeps the weapon away, the NPC fights anyway after this long. */
	private static final long STUCK_TICKS = 100;
	/** Skyrim weapon damage to Minecraft damage (an iron sword's 7 is about a stone sword's 5). */
	private static final float SKYRIM_TO_MC = 0.85F;
	private static int swingLogs;

	/** One NPC's fight so far. */
	private static final class Stance {
		long lastThreat;
		long readyAt;
		long nextSwing;
		long lastFaced = Long.MIN_VALUE;
		Mob victim;
		float damage;
		long landAt;
	}

	private static final Map<Integer, Stance> STANCES = new HashMap<>();

	private NpcFightsBack() {
	}

	static void tick(ServerLevel level, Collection<SkyrimActorEntity> actors) {
		if (!ENABLED || !SkyLink.active()) {
			STANCES.clear();
			return;
		}
		long now = level.getGameTime();
		Set<Integer> present = new HashSet<>();
		for (SkyrimActorEntity npc : actors) {
			present.add(npc.formId());
			if (!npc.isAlive() || npc.getBbHeight() < MIN_HEIGHT || npc.inSkyrimCombat()) {
				STANCES.remove(npc.formId()); // (Skyrim looks after the weapon of one in a fight of its own)
				continue;
			}
			List<Mob> threats = threats(level, npc);
			Stance stance = STANCES.get(npc.formId());
			if (threats.isEmpty()) {
				if (stance != null && now - stance.lastThreat >= STAND_DOWN_TICKS) {
					SkyLink.pushEvent(Proto.EV_NPC_STANCE, npc.formId(), (float) npc.getX(), (float) npc.getY(), (float) npc.getZ(), 0.0F, 0);
					STANCES.remove(npc.formId());
				}
				continue;
			}
			if (stance == null) {
				stance = new Stance();
				stance.readyAt = now + DRAW_TICKS;
				STANCES.put(npc.formId(), stance);
			}
			stance.lastThreat = now;
			boolean bow = npc.bow();
			double reach = bow ? BOW_RANGE : REACH;
			// Whom to deal with: of those in reach, whoever is hitting this NPC, else the nearest; failing that, face the nearest.
			Mob victim = null;
			double victimGap = Double.MAX_VALUE;
			boolean victimAttacker = false;
			for (Mob mob : threats) {
				double gap = gap(npc, mob);
				boolean attacker = mob.getTarget() == npc;
				boolean inReach = gap < reach && Math.abs(mob.getY() - npc.getY()) < (bow ? 6.0 : npc.getBbHeight() + 0.5);
				if (!inReach) {
					continue;
				}
				if (victim == null || (attacker && !victimAttacker) || (attacker == victimAttacker && gap < victimGap)) {
					victim = mob;
					victimGap = gap;
					victimAttacker = attacker;
				}
			}
			Mob focus = victim != null ? victim : threats.getFirst();
			if (now - stance.lastFaced >= FACE_EVERY) {
				stance.lastFaced = now;
				SkyLink.pushEvent(Proto.EV_NPC_STANCE, npc.formId(), (float) focus.getX(), (float) focus.getY(), (float) focus.getZ(), 1.0F, 0);
			}
			// A blow already swung lands now (a moment after the animation began, as it would in Skyrim).
			if (stance.victim != null && now >= stance.landAt) {
				if (stance.victim.isAlive()) {
					var blow = level.damageSources().mobAttack(npc);
					boolean hurt = npc.bow() ? stance.victim.hurtServer(level, blow, stance.damage) : NoShove.hurt(level, stance.victim, blow, stance.damage);
					if (swingLogs++ < 60) {
						SkyCraft.LOG.info("SkyCraft: NPC {} hit {} for {}: {}", String.format("%08X", npc.formId()), stance.victim.getName().getString(), stance.damage,
							hurt ? "landed" : "refused");
					}
				}
				stance.victim = null;
			}
			boolean out = npc.weaponDrawn() || now > stance.readyAt - DRAW_TICKS + STUCK_TICKS;
			if (victim == null || !out || stance.victim != null || now < stance.readyAt || now < stance.nextSwing) {
				continue; // weapon still coming out, or nothing in reach yet: the NPC waits for it, weapon out
			}
			SkyLink.pushEvent(Proto.EV_NPC_SWING, npc.formId(), (float) victim.getX(), (float) victim.getY(), (float) victim.getZ(), bow ? 1.0F : 0.0F, 0);
			stance.victim = victim;
			float base = Math.max(3.0F, npc.weaponDamage() * SKYRIM_TO_MC);
			stance.damage = (bow ? base + 1.0F : base) + npc.getRandom().nextFloat() * 2.0F;
			stance.landAt = now + (bow ? BOW_DELAY : MELEE_DELAY);
			stance.nextSwing = now + (bow ? 45 : 16) + npc.getRandom().nextInt(8);
		}
		for (Iterator<Integer> it = STANCES.keySet().iterator(); it.hasNext(); ) {
			if (!present.contains(it.next())) {
				it.remove(); // gone from the world (died, or out of range)
			}
		}
	}

	private static double gap(SkyrimActorEntity npc, Mob mob) {
		return Math.hypot(mob.getX() - npc.getX(), mob.getZ() - npc.getZ()) - npc.getBbWidth() * 0.5 - mob.getBbWidth() * 0.5;
	}

	/**
	 * The hostile mobs this NPC has to deal with, the ones after it first and then by distance. A fighter
	 * counts any hostile mob it can see or that is near; anyone else only a mob that is after it.
	 */
	private static List<Mob> threats(ServerLevel level, SkyrimActorEntity npc) {
		boolean fighter = npc.fighter();
		List<Mob> found = new ArrayList<>();
		for (Mob mob : level.getEntitiesOfClass(Mob.class, npc.getBoundingBox().inflate(ALERT, 4.0, ALERT))) {
			if (!mob.isAlive()) {
				continue;
			}
			boolean after = mob.getTarget() == npc;
			if (!after && mob.getType().getCategory() != MobCategory.MONSTER) {
				continue;
			}
			double distance = Math.hypot(mob.getX() - npc.getX(), mob.getZ() - npc.getZ());
			if (Math.abs(mob.getY() - npc.getY()) > npc.getBbHeight() + 3.0) {
				continue;
			}
			if (after ? distance < SELF_DEFENCE * (fighter ? 2.5 : 1.0) : fighter && distance < ALERT && (distance < 8.0 || SkyrimMobs.canSee(npc, mob))) {
				found.add(mob);
			}
		}
		found.sort((a, b) -> {
			boolean aa = a.getTarget() == npc, ba = b.getTarget() == npc;
			if (aa != ba) {
				return aa ? -1 : 1;
			}
			return Double.compare(a.distanceToSqr(npc), b.distanceToSqr(npc));
		});
		return found;
	}
}
