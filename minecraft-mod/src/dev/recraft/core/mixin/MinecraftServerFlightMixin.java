package dev.recraft.core.mixin;

import dev.recraft.core.link.CoreLink;
import net.minecraft.server.MinecraftServer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Guests stand on their own RE4's ground, which this server only knows around the host; to it
 * they'd seem to hover and be kicked for flying. Their own clients keep them on the ground.
 */
@Mixin(MinecraftServer.class)
public abstract class MinecraftServerFlightMixin {
	@Inject(method = "allowFlight", at = @At("HEAD"), cancellable = true)
	private void recraft$guestsStandOnTheirRe4(CallbackInfoReturnable<Boolean> cir) {
		if (CoreLink.active()) {
			cir.setReturnValue(true);
		}
	}
}
