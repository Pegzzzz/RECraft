package dev.recraft;

import java.lang.reflect.Field;
import net.minecraft.client.KeyMapping;
import net.minecraft.client.Minecraft;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.EntityHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;

/**
 * RE4's floors are SkyCraft collision that Minecraft's crosshair also picks, so aiming at an enemy's legs (or
 * a padlock low on a door) used to land on the ground just in front of them. When the attack key is pressed
 * and the crosshair isn't on an entity, an RE4 stand-in that the aim passes through (or within a few
 * centimetres of) becomes the target, as long as it's no further than the ground the crosshair hit.
 */
public final class LowAim {
	private static Field clickCount;
	private static boolean said;

	public static void adjust(Minecraft mc) {
		try {
			var p = mc.player;
			if (p == null || mc.level == null || mc.gui.screen() != null) {
				return;
			}
			HitResult hr = mc.hitResult;
			if (hr != null && hr.getType() == HitResult.Type.ENTITY) {
				return;
			}
			KeyMapping key = mc.options.keyAttack;
			if (!key.isDown() && clicks(key) == 0) {
				return;
			}
			Vec3 eye = p.getEyePosition(), look = p.getViewVector(1.0F);
			double reach = p.entityInteractionRange();
			Vec3 end = eye.add(look.scale(reach));
			double ground = hr != null && hr.getType() == HitResult.Type.BLOCK ? hr.getLocation().distanceTo(eye) : reach;
			Entity best = null;
			Vec3 bestAt = null;
			double bestD = Double.MAX_VALUE;
			for (Entity e : mc.level.getEntities(p, new AABB(eye, end).inflate(1.0), e -> Sweep.isStandIn(e) && e.isAlive())) {
				AABB bb = e.getBoundingBox().inflate(0.2);
				var at = bb.clip(eye, end);
				if (at.isEmpty() && bb.contains(eye)) {
					at = java.util.Optional.of(eye);
				}
				if (at.isEmpty()) {
					continue;
				}
				double d = at.get().distanceTo(eye);
				if (d < bestD && d <= ground + 0.75) {
					bestD = d;
					best = e;
					bestAt = at.get();
				}
			}
			if (best == null) {
				return;
			}
			mc.hitResult = new EntityHitResult(best, bestAt);
			mc.crosshairPickEntity = best;
			if (!said) {
				said = true;
				System.out.println("[RECraft] low swing reached " + best.getName().getString() + " past the floor");
			}
		} catch (Throwable t) {
			// never break the attack key
		}
	}

	private static int clicks(KeyMapping k) {
		try {
			if (clickCount == null) {
				clickCount = KeyMapping.class.getDeclaredField("clickCount");
				clickCount.setAccessible(true);
			}
			return clickCount.getInt(k);
		} catch (Throwable t) {
			return 0;
		}
	}
}
