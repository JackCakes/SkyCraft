package dev.skycraft.client;

import dev.skycraft.SkyCraft;
import dev.skycraft.world.SkyDig;
import java.io.IOException;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.channels.OverlappingFileLockException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.Comparator;
import java.util.List;
import java.util.Set;
import java.util.stream.Stream;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.screens.TitleScreen;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.world.Difficulty;
import net.minecraft.world.level.GameType;
import net.minecraft.world.level.LevelSettings;
import net.minecraft.world.level.WorldDataConfiguration;
import net.minecraft.world.level.levelgen.WorldOptions;
import net.minecraft.world.level.levelgen.presets.WorldPreset;

/** Opens (or creates) the void "mirror" world automatically once Skyrim is connected. */
public final class MirrorWorld {
	private static final ResourceKey<WorldPreset> PRESET =
		ResourceKey.create(Registries.WORLD_PRESET, Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "mirror"));
	private static boolean attempted;
	private static long lastLog;
	// /join: a friend's world for this session (the e4mc link their "Open to LAN" shows); null: our own.
	private static @org.jspecify.annotations.Nullable String sessionJoin;
	// Shown in chat once the player is in a world again (why they're back in their own, ...).
	private static @org.jspecify.annotations.Nullable String pendingNote;

	// ---- world reset (the reset key: Skyrim sends Proto.IN_RESET_WORLD) -----------------------
	// 0 none; 1 asked: Skyrim is told (Proto.MC_RESETTING) and given a few frames to put the player
	// on its own controls; 2 the world is closed and its saved chunks are being deleted; 3 the world
	// is opened again and we're waiting for the player to be back in it.
	private static int resetStage;
	private static boolean resetClosing;
	private static int resetFrames;
	private static int resetTries;
	private static boolean resetWarned;
	private static long resetSince;
	private static @org.jspecify.annotations.Nullable String resetNote;
	// A chunk's blocks, its dug Skyrim cells (an attachment on the chunk), and the entities and points
	// of interest around it, are all saved per chunk in folders with these names. Everything else in
	// the world's folder (the player and their inventory, level.dat, settings) stays.
	private static final Set<String> CHUNK_FOLDERS = Set.of("region", "entities", "poi");
	private static final Set<String> KEEP_FOLDERS = Set.of("datapacks", "players", "playerdata", "advancements", "stats", "resources");

	private MirrorWorld() {
	}

	/**
	 * The address in config/skycraft.properties ({@code join=abc-def.e4mc.link}), if any. Written
	 * with the template below the first time, so there's something to fill in.
	 */
	private static @org.jspecify.annotations.Nullable String joinAddress(Minecraft minecraft) {
		java.nio.file.Path file = minecraft.gameDirectory.toPath().resolve("config").resolve("skycraft.properties");
		java.util.Properties props = new java.util.Properties();
		try {
			if (!java.nio.file.Files.exists(file)) {
				java.nio.file.Files.createDirectories(file.getParent());
				java.nio.file.Files.writeString(file, """
					# SkyCraft
					# To play in a friend's world instead of your own: put their address after join=
					# (the link e4mc shows them when they open their world to LAN), then restart Minecraft.
					join=
					""");
			}
			try (var in = java.nio.file.Files.newBufferedReader(file)) {
				props.load(in);
			}
		} catch (java.io.IOException e) {
			SkyCraft.LOG.warn("SkyCraft: couldn't read {}", file, e);
			return null;
		}
		String join = props.getProperty("join", "").trim();
		return join.isEmpty() ? null : join;
	}

	/** /join: leave this world and play in a friend's (their e4mc link, or any server address). */
	public static void joinFriend(Minecraft minecraft, String link) {
		// People paste all sorts: "https://abc-def.e4mc.link/", " abc-def.e4mc.link ".
		String address = link.trim().replaceFirst("^[A-Za-z]+://", "").replaceAll("/+$", "");
		if (address.isEmpty()) {
			return;
		}
		SkyCraft.LOG.info("SkyCraft: /join {}", address);
		sessionJoin = address;
		leaveWorld(minecraft);
	}

	/** The friend's world we're in (the address we joined), or null in our own. */
	public static @org.jspecify.annotations.Nullable String friendAddress(Minecraft minecraft) {
		if (sessionJoin != null) {
			return sessionJoin;
		}
		var server = minecraft.isLocalServer() ? null : minecraft.getCurrentServer();
		return server != null ? server.ip : null;
	}

	/** /leave: back to our own world. */
	public static void leaveFriend(Minecraft minecraft) {
		if (sessionJoin == null) {
			minecraft.gui.hud.getChat().addClientSystemMessage(net.minecraft.network.chat.Component.literal("You're already in your own world."));
			return;
		}
		SkyCraft.LOG.info("SkyCraft: /leave {}", sessionJoin);
		sessionJoin = null;
		pendingNote = "Back in your own world.";
		leaveWorld(minecraft);
	}

	private static void leaveWorld(Minecraft minecraft) {
		attempted = false;
		minecraft.disconnectFromWorld(net.minecraft.client.multiplayer.ClientLevel.DEFAULT_QUIT_MESSAGE);
		minecraft.gui.setScreen(new TitleScreen());  // openWhenReady takes it from the title screen
	}

	/** Every client tick: a note for the player once they're in a world again. */
	public static void tick(Minecraft minecraft) {
		tickReset(minecraft);
		if (pendingNote != null && minecraft.player != null) {
			minecraft.gui.hud.getChat().addClientSystemMessage(net.minecraft.network.chat.Component.literal(pendingNote));
			pendingNote = null;
		}
	}

	/** Whether a world reset is under way (SkyClient tells Skyrim, which keeps the player on its own controls). */
	public static boolean resetting() {
		return resetStage != 0;
	}

	/** After each frame that went out to Skyrim: it has had a look at {@link #resetting()} by now. */
	public static void frameShown() {
		if (resetStage == 1) {
			resetFrames++;
		}
	}

	/**
	 * The reset key: throw this world's blocks away (everything built, everything dug, everywhere) and
	 * open it again fresh, with the player and their inventory as they were. Blocks the player built
	 * stay in the one Minecraft world however far Skyrim has taken them since, so they end up floating
	 * in the void of another place; this is how to get rid of them.
	 */
	public static void requestReset(Minecraft minecraft) {
		if (resetStage != 0) {
			return;
		}
		var server = minecraft.getSingleplayerServer();
		if (server == null || minecraft.level == null || minecraft.player == null) {
			SkyCraft.LOG.info("SkyCraft: world reset asked, but this isn't our own world (a friend's, or none is open); ignoring it");
			return;
		}
		if (server.getPlayerList().getPlayers().size() > 1) {
			SkyCraft.LOG.info("SkyCraft: world reset asked, but friends are in this world; ignoring it");
			return;
		}
		resetStage = 1;
		resetClosing = false;
		resetFrames = 0;
		resetSince = System.currentTimeMillis();
		SkyCraft.LOG.info("SkyCraft: world reset asked; giving Skyrim a moment to take the player");
	}

	private static void tickReset(Minecraft minecraft) {
		switch (resetStage) {
			case 1 -> {
				// A few frames with MC_RESETTING out to Skyrim first: closing the world stalls this
				// thread for a moment, and Skyrim should already have the player by then.
				if (!resetClosing && (resetFrames >= 3 || System.currentTimeMillis() - resetSince > 2000)) {
					resetClosing = true;
					// Like /join: from Minecraft's task queue, not from inside a tick.
					minecraft.execute(() -> closeWorldForReset(minecraft));
				}
			}
			case 2 -> deleteSavedChunks(minecraft);
			case 3 -> {
				long waited = System.currentTimeMillis() - resetSince;
				boolean back = minecraft.level != null && minecraft.player != null;
				if (back && (minecraft.gui.screen() == null || waited > 8000)) {
					resetStage = 0;
					pendingNote = resetNote != null ? resetNote : "World reset: everything built or dug is gone.";
					resetNote = null;
					SkyCraft.LOG.info("SkyCraft: world reset done");
				} else if (waited > 120_000) {
					resetStage = 0;
					SkyCraft.LOG.warn("SkyCraft: world reset: the world didn't open again in two minutes; giving up waiting");
				}
			}
			default -> {
			}
		}
	}

	private static void closeWorldForReset(Minecraft minecraft) {
		resetClosing = false;
		if (resetStage != 1) {
			return;
		}
		resetNote = null;
		resetWarned = false;
		resetTries = 0;
		attempted = true;  // openWhenReady leaves the title screen alone until the saved chunks are gone
		SkyDig.wallsChanged();  // walls worked out from the old world's dug cells mean nothing now
		SkyCraft.LOG.info("SkyCraft: world reset: closing the world");
		minecraft.disconnectFromWorld(net.minecraft.client.multiplayer.ClientLevel.DEFAULT_QUIT_MESSAGE);
		minecraft.gui.setScreen(new TitleScreen());
		resetStage = 2;
		resetSince = System.currentTimeMillis();
	}

	/** Stage 2, every tick until done: wait for the world's server to let go of its files, then clear the chunks. */
	private static void deleteSavedChunks(Minecraft minecraft) {
		if (resetTries++ % 10 != 0) {
			return;  // twice a second is plenty
		}
		long waited = System.currentTimeMillis() - resetSince;
		Path world = minecraft.gameDirectory.toPath().resolve("saves").resolve(SkyCraft.WORLD_NAME);
		if (!Files.isDirectory(world)) {
			SkyCraft.LOG.warn("SkyCraft: world reset: no world folder at {}; nothing to clear", world);
			resetNote = "Couldn't find the world's folder, so nothing was reset.";
			reopenAfterReset();
			return;
		}
		if (waited < 60_000 && !worldFree(world)) {
			return;  // the integrated server is still saving and shutting down
		}
		try {
			int removed = clearChunkFolders(world);
			SkyCraft.LOG.info("SkyCraft: world reset: removed {} saved chunk files", removed);
			reopenAfterReset();
		} catch (IOException e) {
			if (waited < 60_000) {
				if (!resetWarned) {
					resetWarned = true;
					SkyCraft.LOG.warn("SkyCraft: world reset: couldn't clear the world's chunks yet ({}); trying again", e.toString());
				}
				return;
			}
			SkyCraft.LOG.error("SkyCraft: world reset: couldn't clear the world's chunks", e);
			resetNote = "Couldn't reset the world (its files were in use). Close other programs using it and try again.";
			reopenAfterReset();
		}
	}

	private static void reopenAfterReset() {
		SkyDig.wallsChanged();
		attempted = false;  // openWhenReady opens the world from the title screen, as at startup
		resetStage = 3;
		resetSince = System.currentTimeMillis();
	}

	/** The world's server holds session.lock until it has finished shutting down. */
	private static boolean worldFree(Path world) {
		Path lock = world.resolve("session.lock");
		if (!Files.exists(lock)) {
			return true;
		}
		try (FileChannel channel = FileChannel.open(lock, StandardOpenOption.WRITE); FileLock held = channel.tryLock()) {
			return held != null;
		} catch (OverlappingFileLockException | IOException e) {
			return false;
		}
	}

	/** Empties the region, entities and poi folders anywhere in the world's folder; returns how many files went. */
	private static int clearChunkFolders(Path world) throws IOException {
		List<Path> folders;
		try (Stream<Path> walk = Files.walk(world)) {
			folders = walk.filter(p -> isChunkFolder(world, p)).toList();
		}
		int removed = 0;
		for (Path folder : folders) {
			List<Path> inside;
			try (Stream<Path> walk = Files.walk(folder)) {
				inside = walk.filter(p -> !p.equals(folder)).sorted(Comparator.reverseOrder()).toList();  // files before their folders
			}
			for (Path path : inside) {
				if (Files.deleteIfExists(path)) {
					removed++;
				}
			}
		}
		return removed;
	}

	private static boolean isChunkFolder(Path world, Path path) {
		if (path.equals(world) || !Files.isDirectory(path) || !CHUNK_FOLDERS.contains(path.getFileName().toString())) {
			return false;
		}
		for (Path part : world.relativize(path)) {
			if (KEEP_FOLDERS.contains(part.toString())) {
				return false;
			}
		}
		return true;
	}

	public static void openWhenReady(Minecraft minecraft) {
		// Couldn't reach a friend's world, or it closed under us: back to our own, and say why.
		if (minecraft.gui.screen() instanceof net.minecraft.client.gui.screens.DisconnectedScreen && minecraft.level == null) {
			pendingNote = sessionJoin != null
				? "Couldn't stay in " + sessionJoin + " (check the link, and that your friend's world is still open to LAN). You're back in your own world."
				: "Disconnected. You're back in your own world.";
			SkyCraft.LOG.info("SkyCraft: disconnected; back to the mirror world");
			sessionJoin = null;
			attempted = false;
			minecraft.gui.setScreen(new TitleScreen());
			return;
		}
		if (attempted && minecraft.level == null && minecraft.gui.screen() != null && System.currentTimeMillis() - lastLog > 5000) {
			lastLog = System.currentTimeMillis();
			SkyCraft.LOG.info("SkyCraft: still not in the mirror world; current screen {}", minecraft.gui.screen().getClass().getName());
		}
		if (attempted || minecraft.level != null || minecraft.gui.overlay() != null) {
			return;
		}
		// Wait for the menu to settle on the title screen; skip any first-launch prompts in front of it.
		if (!(minecraft.gui.screen() instanceof TitleScreen)) {
			if (minecraft.gui.screen() != null && System.currentTimeMillis() - lastLog > 5000) {
				lastLog = System.currentTimeMillis();
				SkyCraft.LOG.info("SkyCraft: waiting on screen {} before opening the mirror world", minecraft.gui.screen().getClass().getName());
			}
			if (minecraft.gui.screen() == null || minecraft.gui.screen().getClass().getName().contains("Onboarding")) {
				minecraft.gui.setScreen(new TitleScreen());
			}
			return;
		}
		TitleScreen title = (TitleScreen) minecraft.gui.screen();
		attempted = true;
		// Multiplayer: join a friend's world (their e4mc link, or any server address) instead.
		String join = sessionJoin != null ? sessionJoin : joinAddress(minecraft);
		if (join != null) {
			SkyCraft.LOG.info("SkyCraft: joining {}", join);
			pendingNote = "Joined " + join + ". Type /leave to go back to your own world.";
			net.minecraft.client.gui.screens.ConnectScreen.startConnecting(title, minecraft, net.minecraft.client.multiplayer.resolver.ServerAddress.parseString(join),
				new net.minecraft.client.multiplayer.ServerData("SkyCraft", join, net.minecraft.client.multiplayer.ServerData.Type.OTHER), false, null);
			return;
		}
		if (minecraft.getLevelSource().levelExists(SkyCraft.WORLD_NAME)) {
			SkyCraft.LOG.info("SkyCraft: opening mirror world");
			minecraft.createWorldOpenFlows().openWorld(SkyCraft.WORLD_NAME, () -> minecraft.gui.setScreen(title));
			return;
		}
		SkyCraft.LOG.info("SkyCraft: creating mirror world");
		LevelSettings settings = new LevelSettings(
			SkyCraft.WORLD_NAME,
			GameType.SURVIVAL,
			new LevelSettings.DifficultySettings(Difficulty.NORMAL, false, false),
			true,
			WorldDataConfiguration.DEFAULT
		);
		minecraft.createWorldOpenFlows().createFreshLevel(
			SkyCraft.WORLD_NAME,
			settings,
			new WorldOptions(0L, false, false),
			registries -> registries.lookupOrThrow(Registries.WORLD_PRESET).getOrThrow(PRESET).value().createWorldDimensions(),
			title
		);
	}
}
