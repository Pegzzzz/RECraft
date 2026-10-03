package dev.recraft.core.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.recraft.core.world.CoreCollision;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockCollisions;
import net.minecraft.world.level.CollisionGetter;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.shapes.CollisionContext;
import net.minecraft.world.phys.shapes.EntityCollisionContext;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Adds RE4's geometry to every block-collision query. Vanilla movement, step-up, onGround
 * and fall-damage logic then run unchanged against it.
 */
@Mixin(BlockCollisions.class)
public abstract class BlockCollisionsMixin {
	@WrapOperation(
		method = "computeNext",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/world/phys/shapes/CollisionContext;getCollisionShape(Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/CollisionGetter;Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/phys/shapes/VoxelShape;"
		)
	)
	private VoxelShape recraft$addRe4Shape(
		CollisionContext context, BlockState state, CollisionGetter level, BlockPos pos, Operation<VoxelShape> original
	) {
		VoxelShape blockShape = original.call(context, state, level, pos);
		// The walls of holes dug into RE4's ground: solid for everyone.
		if (state.isAir()) {
			VoxelShape wall = dev.recraft.core.world.CoreDig.wallShape(level, pos);
			if (wall != null) {
				blockShape = blockShape.isEmpty() ? wall : Shapes.or(blockShape, wall);
			}
		}
		if (context instanceof EntityCollisionContext entityContext && CoreCollision.usesSmoothCollider(entityContext.getEntity())) {
			return blockShape; // this entity collides with RE4's exact triangles instead (CoreCollider)
		}
		VoxelShape sky = CoreCollision.shapeAt(pos);
		if (sky == null) {
			return blockShape;
		}
		return blockShape.isEmpty() ? sky : Shapes.or(blockShape, sky);
	}
}
