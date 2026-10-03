package dev.recraft.core.client;

import dev.recraft.core.RecraftCore;
import dev.recraft.core.world.CoreDig;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;
import net.fabricmc.fabric.api.client.screen.v1.ScreenEvents;
import net.fabricmc.fabric.api.client.screen.v1.Screens;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.components.Button;
import net.minecraft.client.gui.components.Tooltip;
import net.minecraft.client.gui.screens.PauseScreen;
import net.minecraft.network.chat.Component;

/**
 * The pause menu's "RE4 destruction" button (its top left corner, clear of the menu at any GUI
 * scale) and its setting, {@code destruction=} in config/recraft.properties (see CoreDig.destruction).
 */
public final class DestructionToggle {
	private static final String KEY = "destruction";

	private DestructionToggle() {
	}

	private static Path file() {
		return FabricLoader.getInstance().getConfigDir().resolve("recraft.properties");
	}

	public static void register() {
		load();
		ScreenEvents.AFTER_INIT.register((minecraft, screen, width, height) -> {
			if (screen instanceof PauseScreen pause && pause.showsPauseMenu() && minecraft.player != null) {
				Screens.getWidgets(screen).add(button(minecraft));
			}
		});
	}

	private static Button button(Minecraft minecraft) {
		// The host's server does all the digging, so in a friend's world it's their setting.
		boolean host = minecraft.hasSingleplayerServer();
		Button button = Button.builder(label(), b -> {
			CoreDig.destruction = !CoreDig.destruction;
			b.setMessage(label());
			save();
			RecraftCore.LOG.info("RECraft: RE4 destruction {}", CoreDig.destruction ? "on" : "off");
		}).bounds(4, 4, 150, 20).tooltip(Tooltip.create(Component.literal(host
			? "Mining RE4's ground and rocks, and explosions, dig into RE4's world. Holes already dug stay either way."
			: "In a friend's world, their setting decides."))).build();
		button.active = host;
		return button;
	}

	private static Component label() {
		return Component.literal("RE4 destruction: " + (CoreDig.destruction ? "On" : "Off"));
	}

	private static void load() {
		Properties props = new Properties();
		try (var in = Files.newBufferedReader(file())) {
			props.load(in);
		} catch (NoSuchFileException e) {
			return;
		} catch (IOException e) {
			RecraftCore.LOG.warn("RECraft: couldn't read {}", file(), e);
			return;
		}
		CoreDig.destruction = !"false".equalsIgnoreCase(props.getProperty(KEY, "true").trim());
	}

	/** Rewrites only its own line, keeping the file's comments and other settings (join=). */
	private static void save() {
		Path file = file();
		String line = KEY + "=" + CoreDig.destruction;
		try {
			List<String> lines = Files.exists(file) ? new ArrayList<>(Files.readAllLines(file)) : new ArrayList<>(List.of("# RECraft"));
			boolean found = false;
			for (int i = 0; i < lines.size(); i++) {
				if (lines.get(i).trim().startsWith(KEY + "=")) {
					lines.set(i, line);
					found = true;
				}
			}
			if (!found) {
				lines.add("# Mining and explosions dig into RE4's world (the pause menu's \"RE4 destruction\" button).");
				lines.add(line);
			}
			Files.createDirectories(file.getParent());
			Files.write(file, lines);
		} catch (IOException e) {
			RecraftCore.LOG.warn("RECraft: couldn't save {}", file, e);
		}
	}
}
