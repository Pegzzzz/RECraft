package dev.recraft.core.client.mixin;

import com.mojang.blaze3d.platform.Window;
import dev.recraft.core.client.CoreClient;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** The MC window is hidden while linked; RE4 has the real focus, so pretend we do too. */
@Mixin(Window.class)
public abstract class WindowMixin {
	@Inject(method = "isFocused", at = @At("HEAD"), cancellable = true)
	private void recraft$focused(CallbackInfoReturnable<Boolean> cir) {
		if (CoreClient.tookOver()) {
			// Focused while RE4 is connected; if RE4 goes away, act unfocused so MC
			// never tries to grab the (hidden) mouse.
			cir.setReturnValue(CoreClient.linked());
		}
	}

	@Inject(method = "isIconified", at = @At("HEAD"), cancellable = true)
	private void recraft$notIconified(CallbackInfoReturnable<Boolean> cir) {
		if (CoreClient.linked()) {
			cir.setReturnValue(false);
		}
	}
}
