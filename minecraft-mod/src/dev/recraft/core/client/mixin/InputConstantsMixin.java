package dev.recraft.core.client.mixin;

import com.mojang.blaze3d.platform.InputConstants;
import com.mojang.blaze3d.platform.Window;
import dev.recraft.core.client.InputBridge;
import dev.recraft.core.client.CoreClient;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Keyboard state and mouse capture come from RE4 while linked, not from SDL. */
@Mixin(InputConstants.class)
public abstract class InputConstantsMixin {
	@Inject(method = "isKeyDown", at = @At("HEAD"), cancellable = true)
	private static void recraft$isKeyDown(int key, CallbackInfoReturnable<Boolean> cir) {
		if (CoreClient.tookOver()) {
			cir.setReturnValue(InputBridge.isKeyDown(key));
		}
	}

	@Inject(method = "grabMouse", at = @At("HEAD"), cancellable = true)
	private static void recraft$grabMouse(Window window, double xpos, double ypos, CallbackInfo ci) {
		if (CoreClient.tookOver()) {
			ci.cancel();
		}
	}

	@Inject(method = "releaseMouse", at = @At("HEAD"), cancellable = true)
	private static void recraft$releaseMouse(Window window, double xpos, double ypos, CallbackInfo ci) {
		if (CoreClient.tookOver()) {
			ci.cancel();
		}
	}
}
