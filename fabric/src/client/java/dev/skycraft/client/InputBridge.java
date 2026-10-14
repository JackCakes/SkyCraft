package dev.skycraft.client;

import dev.skycraft.combat.SkyCombat;
import dev.skycraft.combat.SkyrimSwings;
import dev.skycraft.link.Proto;
import dev.skycraft.progression.ResourceBridge;
import dev.skycraft.link.SkyLink;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.screens.PauseScreen;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.client.input.KeyEvent;
import net.minecraft.client.input.MouseButtonInfo;
import org.lwjgl.sdl.SDLKeyboard;

/**
 * Replays Skyrim-captured input into Minecraft's own input handlers, as if the (hidden) MC
 * window had focus. Keeps a virtual keyboard so InputConstants.isKeyDown() still works.
 */
public final class InputBridge {
	private static final boolean[] KEYS = new boolean[512];
	private static final boolean[] BUTTONS = new boolean[8];
	private static double cursorX, cursorY;
	private static int modifiers;
	private static int clickLogs;

	private InputBridge() {
	}

	public static boolean isKeyDown(int scancode) {
		return scancode >= 0 && scancode < KEYS.length && KEYS[scancode];
	}

	public static void drain(Minecraft minecraft) {
		SkyLink.drainInput((type, code, a, b, c) -> dispatch(minecraft, type, code, a, b, c));
	}

	private static void dispatch(Minecraft minecraft, int type, int code, int a, int b, int c) {
		long handle = minecraft.getWindow().handle();
		switch (type) {
			case Proto.IN_KEY -> key(minecraft, handle, code, a != 0);
			case Proto.IN_MOUSE_BUTTON -> {
				if (code > 0 && code < BUTTONS.length) {
					BUTTONS[code] = a != 0;
				}
				if (a != 0 && clickLogs++ < 20) {
					var hit = minecraft.hitResult;
					dev.skycraft.SkyCraft.LOG.info("SkyCraft: click {} -> {} {} (grabbed {}, screen {})", code, hit == null ? "null" : hit.getType(),
						hit instanceof net.minecraft.world.phys.EntityHitResult eh ? eh.getEntity().getName().getString() : hit == null ? "" : hit.getLocation(),
						minecraft.mouseHandler.isMouseGrabbed(), minecraft.gui.screen());
				}
				minecraft.mouseHandler.onButton(handle, new MouseButtonInfo(code, modifiers), a != 0 ? 1 : 0);
			}
			case Proto.IN_SCROLL -> minecraft.mouseHandler.onScroll(handle, 0.0, a / 120.0);
			case Proto.IN_CURSOR -> {
				double dx = a - cursorX;
				double dy = b - cursorY;
				cursorX = a;
				cursorY = b;
				minecraft.mouseHandler.onMove(handle, a, b, dx, dy);
			}
			case Proto.IN_TEXT -> {
				if (minecraft.gui.screen() != null) {
					minecraft.keyboardHandler.textInput(handle, new String(Character.toChars(a)));
				}
			}
			case Proto.IN_RELEASE_ALL -> releaseAll();
			case Proto.IN_HURT -> hurt(minecraft, code, a / 100.0F, b, c);
			case Proto.IN_RESET_WORLD -> MirrorWorld.requestReset(minecraft);
			case Proto.IN_SWING -> swing(minecraft, code, a, b, c);
			case Proto.IN_PROXY_HURT -> proxyHurt(minecraft, a, b, (code & Proto.PROXY_HURT_BY_PLAYER) != 0);
			case Proto.IN_RESOURCE_TRANSFER -> transferResource(minecraft, a, b, c);
			case Proto.IN_OPEN_MENU -> {
				if (minecraft.gui.screen() == null && minecraft.player != null) {
					releaseAll();
					minecraft.gui.setScreen(new PauseScreen(true));
				}
			}
			default -> {
			}
		}
	}

	/** Skyrim hit the player: apply it as Minecraft damage on the integrated server (or the host's). */
	private static void hurt(Minecraft minecraft, int kind, float skyrimDamage, int attacker, int flags) {
		var server = minecraft.getSingleplayerServer();
		if (minecraft.player == null) {
			return;
		}
		if (server == null) {
			// A guest in a friend's world: the host's server applies it.
			if (net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking.canSend(dev.skycraft.net.SkyNet.Hurt.TYPE)) {
				net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking.send(new dev.skycraft.net.SkyNet.Hurt(kind, skyrimDamage, attacker, flags));
			}
			return;
		}
		var uuid = minecraft.player.getUUID();
		server.execute(() -> {
			ServerPlayer player = server.getPlayerList().getPlayer(uuid);
			if (player != null) {
				SkyCombat.hurtPlayer(player, kind, skyrimDamage, attacker, flags);
			}
		});
	}

	/**
	 * Skyrim's player swung or shot in Skyrim's own controls: the mobs in front of them take the hit on
	 * the integrated server (see SkyrimSwings). A guest's Skyrim isn't in the host's world, so it has no say.
	 */
	private static void swing(Minecraft minecraft, int code, int damage100, int yaw100, int pitch100) {
		var server = minecraft.getSingleplayerServer();
		if (server == null || minecraft.player == null) {
			return;
		}
		int kind = code & 0xFF;
		float charge = ((code >> 8) & 0xFF) / 100.0F;
		var uuid = minecraft.player.getUUID();
		server.execute(() -> {
			ServerPlayer player = server.getPlayerList().getPlayer(uuid);
			if (player != null) {
				SkyrimSwings.swing(player, kind, damage100 / 100.0F, yaw100 / 100.0F, pitch100 / 100.0F, charge);
			}
		});
	}

	/** Skyrim's NPCs hurt a mob's stand-in: the mob itself takes it (see MobProxies). */
	private static void proxyHurt(Minecraft minecraft, int damage100, int mobId, boolean byPlayer) {
		var server = minecraft.getSingleplayerServer();
		if (server == null || minecraft.player == null) {
			return;
		}
		var uuid = minecraft.player.getUUID();
		server.execute(() -> {
			ServerPlayer player = server.getPlayerList().getPlayer(uuid);
			if (player != null) {
				SkyrimSwings.proxyHurt(player, mobId, damage100 / 100.0F, byPlayer);
			}
		});
	}

	/** Skyrim acquired a mapped item. Grant it on the authoritative Minecraft server. */
	private static void transferResource(Minecraft minecraft, int requestId, int count, int itemHash) {
		if (minecraft.player == null || count <= 0) {
			resourceAck(requestId, itemHash, 0);
			return;
		}
		var server = minecraft.getSingleplayerServer();
		if (server == null) {
			if (net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking.canSend(dev.skycraft.net.SkyNet.ResourceTransfer.TYPE)) {
				net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking.send(
					new dev.skycraft.net.SkyNet.ResourceTransfer(requestId, itemHash, count));
			} else {
				resourceAck(requestId, itemHash, 0);
			}
			return;
		}

		var uuid = minecraft.player.getUUID();
		server.execute(() -> {
			ServerPlayer player = server.getPlayerList().getPlayer(uuid);
			int accepted = player != null ? ResourceBridge.grant(player, itemHash, count) : 0;
			resourceAck(requestId, itemHash, accepted);
		});
	}

	public static void resourceAck(int requestId, int itemHash, int accepted) {
		if (SkyLink.active()) {
			SkyLink.pushEvent(Proto.EV_RESOURCE_TRANSFER_ACK, 0, accepted, 0, 0, 0, requestId, itemHash);
		}
	}

	private static void key(Minecraft minecraft, long handle, int scancode, boolean down) {
		if (scancode <= 0 || scancode >= KEYS.length) {
			return;
		}
		boolean wasDown = KEYS[scancode];
		KEYS[scancode] = down;
		updateModifiers();
		int action = down ? (wasDown ? -1 : 1) : 0; // -1 = repeat
		int keycode = SDLKeyboard.SDL_GetKeyFromScancode(scancode, (short) modifiers, true);
		minecraft.keyboardHandler.keyPress(handle, action, new KeyEvent(scancode, keycode, modifiers));
	}

	private static void updateModifiers() {
		int m = 0;
		if (KEYS[225]) m |= 0x0001; // SDL_KMOD_LSHIFT
		if (KEYS[229]) m |= 0x0002; // SDL_KMOD_RSHIFT
		if (KEYS[224]) m |= 0x0040; // SDL_KMOD_LCTRL
		if (KEYS[228]) m |= 0x0080; // SDL_KMOD_RCTRL
		if (KEYS[226]) m |= 0x0100; // SDL_KMOD_LALT
		if (KEYS[230]) m |= 0x0200; // SDL_KMOD_RALT
		modifiers = m;
	}

	/** Lift every key and button we think is held (focus moved to Skyrim, link dropped, ...). */
	public static void releaseAll() {
		Minecraft minecraft = Minecraft.getInstance();
		long handle = minecraft.getWindow().handle();
		for (int sc = 0; sc < KEYS.length; sc++) {
			if (KEYS[sc]) {
				KEYS[sc] = false;
				updateModifiers();
				minecraft.keyboardHandler.keyPress(handle, 0, new KeyEvent(sc, SDLKeyboard.SDL_GetKeyFromScancode(sc, (short) 0, true), modifiers));
			}
		}
		for (int button = 1; button < BUTTONS.length; button++) {
			if (BUTTONS[button]) {
				BUTTONS[button] = false;
				minecraft.mouseHandler.onButton(handle, new MouseButtonInfo(button, 0), 0);
			}
		}
	}
}
