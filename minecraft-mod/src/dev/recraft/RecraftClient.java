package dev.recraft;

import java.lang.reflect.Method;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.Vec3;

/**
 * Sneaking stops you at edges, like in Minecraft - also at the edges of Resident Evil 4's floors,
 * which are SkyCraft's own collision rather than Minecraft blocks (vanilla only checks blocks).
 * After each client tick: if you're sneaking, were standing on something, and this tick's move
 * left nothing under your feet, the move is taken back (sliding along the edge when possible).
 */
public final class RecraftClient implements ClientModInitializer {
	private Method groundAt;
	private double lastX, lastZ;
	private boolean lastOnGround, have, warned;
	private int tick;

	@Override
	public void onInitializeClient() {
		try {
			this.groundAt = Class.forName("dev.skycraft.client.SkyCollider").getMethod("groundAt", double.class, double.class, double.class, double.class);
		} catch (Throwable t) {
			System.out.println("[RECraft] SkyCraft's floor check not found - sneaking only stops at Minecraft blocks: " + t);
		}
		ClientTickEvents.END_CLIENT_TICK.register(this::tick);
		HitHud.register();   // hit marker + enemy health bar
	}

	private void tick(Minecraft mc) {
		try {
			LocalPlayer p = mc.player;
			if (p == null || mc.level == null) {
				this.have = false;
				return;
			}
			if (this.tick % 100 == 50) {
				// SkyCraft asks for 8 chunks; RE4's stand-ins and your blocks only need a few, and the hidden
				// Minecraft then leaves more CPU to RE4 in big fights
				var o = mc.options;
				if (o.renderDistance().get() > 5) {
					o.renderDistance().set(5);
					System.out.println("[RECraft] render distance 5 (Minecraft runs hidden)");
				}
				if (o.simulationDistance().get() > 5) {
					o.simulationDistance().set(5);
				}
			}
			if (this.tick++ % 20 == 0) {   // stand-ins don't stop block placement (client side check)
				for (net.minecraft.world.entity.Entity e : mc.level.entitiesForRendering()) {
					if (e.getClass().getName().equals("dev.skycraft.combat.SkyrimActorEntity")) {
						e.blocksBuilding = false;
					}
				}
			}
			double x = p.getX(), y = p.getY(), z = p.getZ();
			if (this.have && p.isShiftKeyDown() && this.lastOnGround && !this.supported(mc.level, x, y, z)) {
				if (this.supported(mc.level, x, y, this.lastZ)) {
					z = this.lastZ;
				} else if (this.supported(mc.level, this.lastX, y, z)) {
					x = this.lastX;
				} else {
					x = this.lastX;
					z = this.lastZ;
				}
				p.setPos(x, y, z);
				Vec3 d = p.getDeltaMovement();
				p.setDeltaMovement(0.0, d.y, 0.0);
			}
			this.lastX = x;
			this.lastZ = z;
			this.lastOnGround = p.onGround();
			this.have = true;
		} catch (Throwable t) {
			if (!this.warned) {
				this.warned = true;
				System.out.println("[RECraft] sneak edge check error (continuing): " + t);
			}
		}
	}

	/** Something to stand on under any part of the player's feet (corners and centre). */
	private boolean supported(Level level, double x, double y, double z) {
		double[][] pts = { { 0, 0 }, { 0.25, 0.25 }, { -0.25, 0.25 }, { 0.25, -0.25 }, { -0.25, -0.25 } };
		for (double[] o : pts) {
			double px = x + o[0], pz = z + o[1];
			BlockPos bp = BlockPos.containing(px, y - 0.3, pz);
			if (!level.getBlockState(bp).getCollisionShape(level, bp).isEmpty()) {
				return true;
			}
			if (this.groundAt != null) {
				try {
					double g = (double) this.groundAt.invoke(null, px, y - 0.6, pz, 0.65);
					if (!Double.isNaN(g) && g >= y - 0.6) {
						return true;
					}
				} catch (Throwable t) {
					return true;   // can't tell: don't hold the player back
				}
			}
		}
		return false;
	}
}
