package dev.recraft.core.mixin;

import dev.recraft.core.world.CoreCollision;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.tags.FluidTags;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FlowingFluid;
import net.minecraft.world.level.material.FluidState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Water and lava settle on RE4's ground and run over it, never into it. RE4's terrain
 * arrives as a thin surface, so the rules follow that surface:
 * <ul>
 * <li>a fluid lies on whatever RE4 geometry is in its cell and never flows down through it;</li>
 * <li>it moves into a cell only where its surface there would be above the ground (the top of the
 * geometry in that cell), so it runs downhill and over bumps but not uphill;</li>
 * <li>an empty cell right under RE4 geometry is under the ground (or an overhang), so no fluid
 * flows sideways into it;</li>
 * <li>it never enters cells RE4 hasn't described yet (far from the player).</li>
 * </ul>
 */
@Mixin(FlowingFluid.class)
public abstract class FlowingFluidMixin {
	// A falling fluid's surface in its cell (8/9) and the margin it must clear the ground by.
	private static final float FALLING_SURFACE = 8.0F / 9.0F;
	private static final float MARGIN = 0.05F;

	@Inject(method = "canPassThroughWall", at = @At("HEAD"), cancellable = true)
	private static void recraft$re4Wall(
		Direction direction, BlockGetter level, BlockPos sourcePos, BlockState sourceState, BlockPos targetPos, BlockState targetState,
		CallbackInfoReturnable<Boolean> cir
	) {
		if (!targetState.isAir() || !CoreCollision.active() || direction == Direction.UP) {
			return;
		}
		if (!CoreCollision.isKnown(targetPos.getX(), targetPos.getY(), targetPos.getZ())) {
			refused("unknown region", direction, sourcePos, sourceState, targetPos, 0.0F);
			cir.setReturnValue(false);
			return;
		}
		if (direction == Direction.DOWN) {
			if (CoreCollision.hasGeometry(sourcePos)) {
				// Lying on RE4 ground: it doesn't sink through.
				refused("resting on ground", direction, sourcePos, sourceState, targetPos, CoreCollision.groundTop(sourcePos));
				cir.setReturnValue(false);
				return;
			}
			float top = CoreCollision.groundTop(targetPos);
			if (top >= FALLING_SURFACE - MARGIN) {
				refused("ground below", direction, sourcePos, sourceState, targetPos, top);
				cir.setReturnValue(false);
			}
			return;
		}
		// Sideways.
		if (!CoreCollision.hasGeometry(targetPos)) {
			if (CoreCollision.hasGeometry(targetPos.above())) {
				// Under the ground (terrain rises a block or more there) or under an overhang.
				refused("under ground", direction, sourcePos, sourceState, targetPos, 1.0F);
				cir.setReturnValue(false);
			}
			return;
		}
		FluidState fluid = sourceState.getFluidState();
		float surface;
		if (fluid.isEmpty()) {
			surface = 0.8F; // Minecraft looking ahead for a slope: any cell a flow could reach
		} else if (fluid.getValue(FlowingFluid.FALLING)) {
			surface = 7.0F / 9.0F; // a falling fluid spreads at level 7 where it lands
		} else {
			// One level less there (lava drops two outside the Nether).
			int drop = fluid.is(FluidTags.LAVA) ? 2 : 1;
			surface = Math.max(0, fluid.getAmount() - drop) / 9.0F;
		}
		float top = CoreCollision.groundTop(targetPos);
		// Level or downhill ground (within a voxel of the ground it leaves) always takes it, as a
		// flat Minecraft floor would: its drawn surface sits on that ground (see WorldExporter).
		boolean flatOrDownhill = top <= CoreCollision.groundTop(sourcePos) + 0.13F;
		if (!flatOrDownhill && top >= surface - MARGIN) {
			refused("ground beside", direction, sourcePos, sourceState, targetPos, top);
			cir.setReturnValue(false);
		}
	}

	private static long recraft$lastLog;
	private static int recraft$logged;

	private static void refused(String why, Direction direction, BlockPos sourcePos, BlockState sourceState, BlockPos targetPos, float ground) {
		long now = System.currentTimeMillis();
		if (now - recraft$lastLog > 1000) {
			recraft$lastLog = now;
			recraft$logged = 0;
		}
		if (recraft$logged++ < 4) {
			FluidState fluid = sourceState.getFluidState();
			dev.recraft.core.RecraftCore.LOG.info("RECraft: fluid flow refused ({}): {} -> {} going {}, fluid {} amount {}, RE4 ground height there {}", why,
				sourcePos.toShortString(), targetPos.toShortString(), direction, fluid.getType(), fluid.getAmount(), String.format("%.2f", ground));
		}
	}
}
