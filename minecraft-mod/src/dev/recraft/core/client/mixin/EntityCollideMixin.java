package dev.recraft.core.client.mixin;

import dev.recraft.core.client.CoreClient;
import dev.recraft.core.client.CoreCollider;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * After vanilla has collided the local player's movement with Minecraft blocks, collide it with
 * RE4's exact triangles (smooth slopes instead of voxel stair-steps).
 */
@Mixin(Entity.class)
public abstract class EntityCollideMixin {
	@Inject(method = "collide", at = @At("RETURN"), cancellable = true)
	private void recraft$smoothRe4Collision(Vec3 movement, CallbackInfoReturnable<Vec3> cir) {
		if ((Object) this instanceof LocalPlayer player && CoreClient.linked() && !player.noPhysics) {
			cir.setReturnValue(CoreCollider.collide(player, cir.getReturnValue()));
		}
	}
}
