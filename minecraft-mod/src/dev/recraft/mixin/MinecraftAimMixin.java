package dev.recraft.mixin;

import dev.recraft.LowAim;
import net.minecraft.client.Minecraft;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Right before Minecraft acts on the attack key: let a swing at RE4's floor reach the enemy standing on it. */
@Mixin(Minecraft.class)
public abstract class MinecraftAimMixin {
	@Inject(method = "handleKeybinds", at = @At("HEAD"))
	private void recraft$lowAim(CallbackInfo ci) {
		LowAim.adjust((Minecraft) (Object) this);
	}
}
