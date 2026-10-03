package dev.recraft.core.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.recraft.core.world.CoreClip;
import net.minecraft.world.item.Item;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.BlockHitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/** Buckets, boats and other "use on what I'm looking at" items see RE4 surfaces. */
@Mixin(Item.class)
public abstract class ItemMixin {
	@WrapOperation(
		method = "getPlayerPOVHitResult",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;clip(Lnet/minecraft/world/level/ClipContext;)Lnet/minecraft/world/phys/BlockHitResult;")
	)
	private static BlockHitResult recraft$povRe4(Level level, ClipContext context, Operation<BlockHitResult> original) {
		// These items act on hitPos.relative(face), so report the cell the surface is in.
		return CoreClip.refine(context.getFrom(), context.getTo(), original.call(level, context), CoreClip.Use.PROJECTILE);
	}
}
