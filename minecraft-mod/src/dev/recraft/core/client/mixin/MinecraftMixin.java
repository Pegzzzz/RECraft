package dev.recraft.core.client.mixin;

import dev.recraft.core.client.CoreClient;
import net.minecraft.client.Minecraft;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Minecraft.class)
public abstract class MinecraftMixin {
	@Inject(method = "runTick", at = @At("HEAD"))
	private void recraft$beginFrame(boolean advanceGameTime, CallbackInfo ci) {
		CoreClient.beginFrame();
	}

	@Inject(
		method = "renderFrame",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/GameRenderer;render()V", shift = At.Shift.AFTER)
	)
	private void recraft$afterRender(boolean advanceGameTime, CallbackInfo ci) {
		CoreClient.afterRender();
	}

	@Inject(method = "renderFrame", at = @At("TAIL"))
	private void recraft$pace(boolean advanceGameTime, CallbackInfo ci) {
		CoreClient.paceFrame();
	}
}
