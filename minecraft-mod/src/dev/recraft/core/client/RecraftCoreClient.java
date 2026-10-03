package dev.recraft.core.client;

import dev.recraft.core.combat.CoreCombat;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.fabricmc.fabric.api.client.rendering.v1.EntityRendererRegistry;
import net.minecraft.client.renderer.entity.NoopRenderer;

public final class RecraftCoreClient implements ClientModInitializer {
	@Override
	public void onInitializeClient() {
		dev.recraft.core.link.CoreLink.announceRunning();
		DestructionToggle.register();
		// Multiplayer without editing files: the host opens their world to LAN (O, Open to LAN) and
		// e4mc gives them a link; friends type /join <link> in chat, and /leave to come back.
		net.fabricmc.fabric.api.client.command.v2.ClientCommandRegistrationCallback.EVENT.register((dispatcher, context) -> {
			dispatcher.register(net.fabricmc.fabric.api.client.command.v2.ClientCommands.literal("join")
				.then(net.fabricmc.fabric.api.client.command.v2.ClientCommands.argument("link", com.mojang.brigadier.arguments.StringArgumentType.greedyString())
					.executes(c -> {
						String link = com.mojang.brigadier.arguments.StringArgumentType.getString(c, "link");
						c.getSource().sendFeedback(net.minecraft.network.chat.Component.literal("Joining " + link.trim() + "..."));
						// After the chat screen has closed: this leaves the current world.
						net.minecraft.client.Minecraft.getInstance().execute(() -> MirrorWorld.joinFriend(net.minecraft.client.Minecraft.getInstance(), link));
						return 1;
					})));
			dispatcher.register(net.fabricmc.fabric.api.client.command.v2.ClientCommands.literal("leave").executes(c -> {
				net.minecraft.client.Minecraft.getInstance().execute(() -> MirrorWorld.leaveFriend(net.minecraft.client.Minecraft.getInstance()));
				return 1;
			}));
		});
		ClientTickEvents.END_CLIENT_TICK.register(CoreClient::clientTick);
		// Multiplayer testing on one PC: RECRAFT_LAN_PORT opens the world to LAN on that port as soon
		// as it's loaded, and RECRAFT_LAN_OFFLINE lets offline (dev) clients join it.
		net.fabricmc.fabric.api.client.networking.v1.ClientPlayConnectionEvents.JOIN.register((handler, sender, minecraft) -> {
			String port = System.getenv("RECRAFT_LAN_PORT");
			var server = minecraft.getSingleplayerServer();
			if (port == null || port.isBlank() || server == null || server.isPublished()) {
				return;
			}
			minecraft.execute(() -> {
				if (System.getenv("RECRAFT_LAN_OFFLINE") != null) {
					server.setUsesAuthentication(false);
				}
				boolean ok = server.publishServer(net.minecraft.server.MinecraftServer.MultiplayerScope.LAN, false, Integer.parseInt(port.trim()));
				dev.recraft.core.RecraftCore.LOG.info("RECraft: world opened to LAN on port {} ({}{})", port.trim(), ok ? "ok" : "FAILED",
					System.getenv("RECRAFT_LAN_OFFLINE") != null ? ", offline logins allowed" : "");
			});
		});
		// A guest in a friend's world: dying there kills this player's own RE4 character.
		net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking.registerGlobalReceiver(dev.recraft.core.net.CoreNet.Died.TYPE, (payload, context) -> {
			if (dev.recraft.core.link.CoreLink.active()) {
				dev.recraft.core.link.CoreLink.pushEvent(dev.recraft.core.link.Proto.EV_PLAYER_DIED, payload.attackerFormId(), 0, 0, 0, 0, 0);
			}
		});
		// RE4 draws the real NPC; its Minecraft stand-in is only a hitbox.
		EntityRendererRegistry.register(CoreCombat.RE4_ACTOR, NoopRenderer::new);
		// Players (client-side movement AND the integrated server's re-check of it) use the smooth
		// triangle collider, never RE4's voxels; otherwise the server sees the smooth position
		// dip into a voxel and teleports the player back every few ticks.
		dev.recraft.core.world.CoreCollision.setSmoothCollider(e -> e instanceof net.minecraft.world.entity.player.Player && CoreClient.linked());
	}
}
