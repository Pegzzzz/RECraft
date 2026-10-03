package dev.recraft.core.client.mixin;

import dev.recraft.core.client.CoreDigClient;
import net.minecraft.client.Minecraft;
import net.minecraft.world.InteractionHand;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Attacking RE4's geometry digs into it (CoreDigClient). */
@Mixin(Minecraft.class)
public abstract class MinecraftDigMixin {
	@Inject(method = "startAttack", at = @At("HEAD"), cancellable = true)
	private void recraft$digStart(CallbackInfoReturnable<Boolean> cir) {
		Minecraft minecraft = (Minecraft) (Object) this;
		if (CoreDigClient.attack(minecraft)) {
			// A swing, not a miss: no miss cooldown before mining the block that appears.
			var held = minecraft.player.getItemInHand(InteractionHand.MAIN_HAND);
			minecraft.player.swing(InteractionHand.MAIN_HAND, held.getAttackAnimation(), false);
			cir.setReturnValue(true);
		}
	}

	@Inject(method = "continueAttack", at = @At("HEAD"))
	private void recraft$digHold(boolean down, CallbackInfo ci) {
		if (down) {
			CoreDigClient.attack((Minecraft) (Object) this);
		}
	}
}
