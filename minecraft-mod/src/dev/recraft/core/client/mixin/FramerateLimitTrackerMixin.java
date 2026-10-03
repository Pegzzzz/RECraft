package dev.recraft.core.client.mixin;

import com.mojang.blaze3d.platform.FramerateLimitTracker;
import dev.recraft.core.client.CoreClient;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** CoreClient.paceFrame() locks us to RE4's frame rate; don't let MC throttle on its own. */
@Mixin(FramerateLimitTracker.class)
public abstract class FramerateLimitTrackerMixin {
	@Inject(method = "getFramerateLimit", at = @At("HEAD"), cancellable = true)
	private void recraft$unlimited(CallbackInfoReturnable<Integer> cir) {
		if (CoreClient.linked()) {
			cir.setReturnValue(260);
		}
	}
}
