package dev.recraft;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.reflect.Field;
import java.nio.charset.StandardCharsets;
import net.fabricmc.fabric.api.client.rendering.v1.hud.HudElementRegistry;
import net.fabricmc.fabric.api.client.rendering.v1.hud.VanillaHudElements;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.resources.Identifier;

/**
 * Hit feedback on Minecraft's HUD: RE4's enemies don't flinch from every hit any more, so each hit shows a
 * small hit marker around the crosshair (RECraft's DLL publishes the last hit
 * at RecraftLink.BASE + O_HIT: seq, hp, max hp, flags 1 flinched / 2 killed, name).
 */
final class HitHud {
	static final long O_HIT = RecraftLink.BASE + 0xDE0;
	private static Field shmField;
	private static int lastSeq;
	private static boolean init;
	private static long shownAt;
	private static int hp, max, flags;
	private static String name = "";

	static void register() {
		try {
			shmField = Class.forName("dev.skycraft.link.SkyLink").getDeclaredField("shm");
			shmField.setAccessible(true);
		} catch (Throwable t) {
			return;
		}
		HudElementRegistry.attachElementAfter(VanillaHudElements.CROSSHAIR, Identifier.fromNamespaceAndPath("recraft", "hit_feedback"), HitHud::draw);
	}

	private static void poll() {
		try {
			MemorySegment s = (MemorySegment) shmField.get(null);
			if (s == null || s.byteSize() < O_HIT + 32) {
				return;
			}
			int seq = s.get(ValueLayout.JAVA_INT, O_HIT);
			if (!init) {
				init = true;
				lastSeq = seq;
				return;
			}
			if (seq == lastSeq) {
				return;
			}
			lastSeq = seq;
			hp = s.get(ValueLayout.JAVA_INT, O_HIT + 4);
			max = s.get(ValueLayout.JAVA_INT, O_HIT + 8);
			flags = s.get(ValueLayout.JAVA_INT, O_HIT + 12);
			byte[] b = new byte[16];
			for (int i = 0; i < 16; i++) {
				b[i] = s.get(ValueLayout.JAVA_BYTE, O_HIT + 16 + i);
			}
			int n = 0;
			while (n < 16 && b[n] != 0) {
				n++;
			}
			name = new String(b, 0, n, StandardCharsets.US_ASCII);
			shownAt = System.currentTimeMillis();
		} catch (Throwable ignored) {
		}
	}

	private static void draw(GuiGraphicsExtractor g, DeltaTracker delta) {
		poll();
		long age = System.currentTimeMillis() - shownAt;
		boolean killed = (flags & 2) != 0, staggered = (flags & 1) != 0;
		long life = killed ? 300 : 160;
		if (shownAt == 0 || age > life) {
			return;
		}
		// a small, quick hit marker: four short ticks just off the crosshair (red = kill, a touch wider = stagger)
		int cx = g.guiWidth() / 2, cy = g.guiHeight() / 2;
		int alpha = (int) (230 * (1.0 - (double) age / life)) & 0xFF;
		int col = (alpha << 24) | (killed ? 0xFF4040 : 0xFFFFFF);
		int a0 = staggered || killed ? 4 : 3, a1 = a0 + 2;
		for (int i = a0; i <= a1; i++) {
			g.fill(cx - i - 1, cy - i - 1, cx - i, cy - i, col);
			g.fill(cx + i, cy - i - 1, cx + i + 1, cy - i, col);
			g.fill(cx - i - 1, cy + i, cx - i, cy + i + 1, col);
			g.fill(cx + i, cy + i, cx + i + 1, cy + i + 1, col);
		}
	}
}
