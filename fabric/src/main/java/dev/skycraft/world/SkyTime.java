package dev.skycraft.world;

import dev.skycraft.link.SkyLink;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.server.MinecraftServer;

/**
 * Minecraft's clock follows Skyrim's. Skyrim's hour (0 to 24) is mapped onto Minecraft's day the way
 * the two games line up on their own: Skyrim 6:00 is Minecraft tick 0 (sunrise), 12:00 is tick 6000
 * (noon), 18:00 is tick 12000 (sunset) and 24:00 is tick 18000 (midnight). So when it is day in
 * Skyrim it is day in Minecraft, and the same for night. Minecraft's own clock is stopped
 * ({@code advance_time} is off), Skyrim sets it: waiting, sleeping and the timescale all carry over.
 *
 * <p>Turn it off with the JVM argument {@code -Dskycraft.syncTime=false}.
 */
public final class SkyTime {
	private static final boolean ENABLED = !"false".equalsIgnoreCase(System.getProperty("skycraft.syncTime"));
	/** Minecraft's clock is only set once it's this many ticks off (it would be a command every few ticks otherwise). */
	private static final int THRESHOLD = 10;
	private static final SkyLink.SkyState STATE = new SkyLink.SkyState();

	private static volatile float hour = -1.0F;
	private static volatile boolean live;
	private static volatile boolean settlement;
	private static int lastTicks = -1;

	private SkyTime() {
	}

	public static void init() {
		ServerTickEvents.END_SERVER_TICK.register(SkyTime::tick);
		ServerTickEvents.END_SERVER_TICK.register(SkyTime::readSettlement);
	}

	/** The player is in a place people live, as Skyrim last said (false while there is no game to ask). Natural spawning keeps out of such places. */
	public static boolean inSettlement() {
		return settlement;
	}

	// Separate from tick(): the clock sync can be switched off (skycraft.syncTime=false), this can't.
	private static void readSettlement(MinecraftServer server) {
		if (server.getTickCount() % 5 != 0) {
			return;
		}
		SkyLink.SkyState state = new SkyLink.SkyState();
		settlement = SkyLink.active() && SkyLink.readSkyState(state) && state.inGame() && !state.loading() && state.inSettlement();
	}

	/** Skyrim's hour of the day (0 to 24), or -1 while there is no game to read it from. */
	public static float hour() {
		return live ? hour : -1.0F;
	}

	/** Skyrim's clock is known (a save is loaded and not loading). */
	public static boolean known() {
		return live;
	}

	/** Dark outside: from Minecraft's dusk (19:00) to its dawn (5:00), when monsters walk the open ground. */
	public static boolean isNight() {
		float h = hour();
		return h >= 0.0F && (h >= 19.0F || h < 5.0F);
	}

	/** Bright outside: the hours animals spawn in. */
	public static boolean isDay() {
		float h = hour();
		return h >= 6.0F && h < 18.5F;
	}

	/** Minecraft's time of day (0 to 23999) for a Skyrim hour. */
	public static int dayTicks(float skyrimHour) {
		float shifted = (skyrimHour - 6.0F + 24.0F) % 24.0F;
		return Math.min(23999, Math.max(0, (int) (shifted * 1000.0F)));
	}

	private static void tick(MinecraftServer server) {
		if (!ENABLED || server.getTickCount() % 5 != 0) {
			return;
		}
		if (!SkyLink.active() || !SkyLink.readSkyState(STATE) || !STATE.inGame() || STATE.loading()) {
			live = false;
			return;
		}
		float h = STATE.gameHour;
		if (!(h >= 0.0F && h < 24.0F)) { // also false for NaN
			live = false;
			return;
		}
		hour = h;
		live = true;
		int ticks = dayTicks(h);
		if (lastTicks >= 0) {
			int apart = Math.abs(ticks - lastTicks);
			if (Math.min(apart, 24000 - apart) < THRESHOLD) {
				return;
			}
		}
		lastTicks = ticks;
		server.getCommands().performPrefixedCommand(server.createCommandSourceStack().withSuppressedOutput(), "time set " + ticks);
	}
}
