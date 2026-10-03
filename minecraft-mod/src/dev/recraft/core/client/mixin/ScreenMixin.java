package dev.recraft.core.client.mixin;

import dev.recraft.core.client.CoreClient;
import net.minecraft.client.gui.screens.Screen;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * RE4 keeps running while a Minecraft screen (pause menu, options, inventory) is open, so the
 * Minecraft world must too: arrows keep flying and enemy hits still land.
 */
@Mixin(Screen.class)
public abstract class ScreenMixin {
	@Inject(method = "isPauseScreen", at = @At("HEAD"), cancellable = true)
	private void recraft$neverPauseWhileLinked(CallbackInfoReturnable<Boolean> cir) {
		if (CoreClient.linked()) {
			cir.setReturnValue(false);
		}
	}
}
