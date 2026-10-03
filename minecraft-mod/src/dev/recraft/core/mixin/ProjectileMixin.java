package dev.recraft.core.mixin;

import dev.recraft.core.world.CoreClip;
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.entity.projectile.ProjectileDeflection;
import net.minecraft.world.phys.HitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Minecraft ignores projectile hits on air, and to Minecraft a RE4 wall is air. Treat a hit on
 * RE4 geometry as a real hit: arrows stick in it, snowballs and eggs break on it.
 */
@Mixin(Projectile.class)
public abstract class ProjectileMixin {
	@Shadow
	protected abstract void onHit(HitResult hitResult);

	@Inject(method = "hitTargetOrDeflectSelf", at = @At("HEAD"), cancellable = true)
	private void recraft$hitRe4(HitResult hitResult, CallbackInfoReturnable<ProjectileDeflection> cir) {
		if (hitResult instanceof CoreClip.Re4HitResult) {
			this.onHit(hitResult);
			cir.setReturnValue(ProjectileDeflection.NONE);
		}
	}
}
