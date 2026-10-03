package dev.recraft;

import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import net.minecraft.ChatFormatting;
import net.minecraft.core.BlockPos;
import net.minecraft.core.particles.ParticleTypes;
import net.minecraft.network.chat.Component;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.sounds.SoundEvents;
import net.minecraft.sounds.SoundSource;
import net.minecraft.tags.ItemTags;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.InteractionResult;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.Vec3;

/**
 * RE4's grenades as Minecraft items, plus the axe as the heavy weapon:
 *  - TNT (hand grenade): placing it lights it; it goes off 2.5 s later. It breaks no blocks (RE4's rooms and
 *    your builds stay), but hurts everything around it - RE4's enemies and you.
 *  - Ender pearl (flash grenade): throws you up to 24 blocks where you look, stopping at RE4's walls.
 *  - Fire charge (incendiary grenade): set the first RE4 enemy in your sights on fire.
 *  - Axe: one swing every AxeCooldownSec seconds (RECraft.ini); RE4 makes that swing hit much harder.
 * RE4's rooms collide through the core's additions to block collisions, so every ray here is marched with
 * collision boxes rather than vanilla's raycast (which doesn't see them).
 */
final class Throwables {
	static final int TNT_FUSE = 50;
	private static final List<long[]> FUSES = new ArrayList<>();   // {packed pos, ticks left}
	static volatile int axeCooldownSec = 20;

	// ------------------------------------------------------------------ the axe
	static InteractionResult onAttack(Player player, Entity target) {
		ItemStack hand = player.getMainHandItem();
		if (!hand.is(ItemTags.AXES) || !Sweep.isStandIn(target) || isBreakable(target)) {
			return InteractionResult.PASS;   // windows, crates, barrels: chop away, no cooldown
		}
		if (player.getCooldowns().isOnCooldown(hand)) {
			return InteractionResult.FAIL;   // still recharging: no swing
		}
		if (!player.level().isClientSide()) {
			player.getCooldowns().addCooldown(hand, Math.max(1, axeCooldownSec) * 20);
		}
		return InteractionResult.PASS;
	}

	/** RE4's breakable scenery (the DLL names their stand-ins). */
	static boolean isBreakable(Entity e) {
		var n = e.getCustomName();
		if (n == null) {
			return false;
		}
		String s = n.getString();
		return switch (s) {
			case "Window", "Box", "Barrel", "Boarded window", "Lock", "Item", "Lamp", "Target", "Boards", "Trap" -> true;
			default -> false;
		};
	}

	// ------------------------------------------------------------------ using items
	static InteractionResult onUseItem(Player player, Level level, InteractionHand hand) {
		ItemStack st = player.getItemInHand(hand);
		if (st.is(Items.ENDER_PEARL)) {
			if (!level.isClientSide() && player instanceof ServerPlayer sp && !sp.getCooldowns().isOnCooldown(st)) {
				pearl(sp, st);
			}
			return InteractionResult.SUCCESS;
		}
		if (st.is(Items.FIRE_CHARGE)) {
			if (!level.isClientSide() && player instanceof ServerPlayer sp && !sp.getCooldowns().isOnCooldown(st)) {
				fireCharge(sp, st);
			}
			return InteractionResult.SUCCESS;
		}
		return InteractionResult.PASS;
	}

	static InteractionResult onUseBlock(Player player, Level level, InteractionHand hand, BlockHitResult hit) {
		ItemStack st = player.getItemInHand(hand);
		if (st.is(Items.FIRE_CHARGE) || st.is(Items.ENDER_PEARL)) {
			return onUseItem(player, level, hand);   // thrown, never placed as fire
		}
		if (!st.is(Items.TNT)) {
			return InteractionResult.PASS;
		}
		if (!level.isClientSide() && level instanceof ServerLevel sl) {
			BlockPos pos = hit.getBlockPos().relative(hit.getDirection());
			if (!sl.getBlockState(pos).canBeReplaced()) {
				return InteractionResult.FAIL;
			}
			sl.setBlock(pos, Blocks.TNT.defaultBlockState(), 3);
			synchronized (FUSES) {
				FUSES.add(new long[] { pos.asLong(), TNT_FUSE });
			}
			if (!player.hasInfiniteMaterials()) {
				st.shrink(1);
			}
			sl.playSound(null, pos, SoundEvents.TNT_PRIMED, SoundSource.BLOCKS, 1.0F, 1.0F);
			System.out.println("[RECraft] TNT lit at " + pos.toShortString());
		}
		return InteractionResult.SUCCESS;
	}

	/** Every server tick: lit TNT goes off when its fuse runs out. */
	static void tick(ServerLevel level) {
		synchronized (FUSES) {
			for (Iterator<long[]> it = FUSES.iterator(); it.hasNext(); ) {
				long[] f = it.next();
				if (--f[1] > 0) {
					BlockPos pos = BlockPos.of(f[0]);
					if (f[1] % 10 == 0) {
						level.sendParticles(ParticleTypes.SMOKE, pos.getX() + 0.5, pos.getY() + 1.0, pos.getZ() + 0.5, 3, 0.1, 0.1, 0.1, 0.01);
					}
					continue;
				}
				it.remove();
				BlockPos pos = BlockPos.of(f[0]);
				if (level.getBlockState(pos).is(Blocks.TNT)) {
					level.setBlock(pos, Blocks.AIR.defaultBlockState(), 3);
				}
				level.explode(null, pos.getX() + 0.5, pos.getY() + 0.5, pos.getZ() + 0.5, 3.5F, Level.ExplosionInteraction.NONE);
				System.out.println("[RECraft] TNT exploded at " + pos.toShortString());
			}
		}
	}

	// ------------------------------------------------------------------ rays that see RE4's rooms
	/** Distance along the ray to the first solid thing (blocks or RE4's room), at most max. */
	static double march(ServerLevel level, Vec3 from, Vec3 dir, double max, double r) {
		for (double d = 0.4; d <= max; d += 0.2) {
			Vec3 p = from.add(dir.scale(d));
			if (!level.noCollision(new AABB(p.x - r, p.y - r, p.z - r, p.x + r, p.y + r, p.z + r))) {
				return d;
			}
		}
		return max;
	}

	static void pearl(ServerPlayer p, ItemStack st) {
		ServerLevel level = p.level();
		Vec3 eye = p.getEyePosition(), dir = p.getLookAngle();
		double hit = march(level, eye, dir, 24.0, 0.15);
		Vec3 dest = null;
		for (double d = hit - 0.5; d >= 1.0; d -= 0.25) {
			Vec3 c = eye.add(dir.scale(d));
			Vec3 feet = new Vec3(c.x, c.y - p.getBbHeight() * 0.5, c.z);
			AABB box = p.getDimensions(p.getPose()).makeBoundingBox(feet);
			if (level.noCollision(box)) {
				dest = feet;
				break;
			}
		}
		if (dest == null) {
			p.sendSystemMessage(Component.literal("No room to land there").withStyle(ChatFormatting.GRAY), true);
			return;
		}
		Vec3 from = p.position();
		level.sendParticles(ParticleTypes.PORTAL, from.x, from.y + 1, from.z, 24, 0.3, 0.6, 0.3, 0.2);
		p.teleportTo(dest.x, dest.y, dest.z);
		p.resetFallDistance();
		level.sendParticles(ParticleTypes.PORTAL, dest.x, dest.y + 1, dest.z, 24, 0.3, 0.6, 0.3, 0.2);
		level.playSound(null, dest.x, dest.y, dest.z, SoundEvents.PLAYER_TELEPORT, SoundSource.PLAYERS, 1.0F, 1.0F);
		p.hurtServer(level, p.damageSources().fall(), 2.0F);   // like vanilla's pearl, it costs a little health
		if (!p.hasInfiniteMaterials()) {
			st.shrink(1);
		}
		p.getCooldowns().addCooldown(st, 20);
		System.out.println("[RECraft] ender pearl: " + String.format("%.1f", from.distanceTo(dest)) + " blocks");
	}

	static void fireCharge(ServerPlayer p, ItemStack st) {
		ServerLevel level = p.level();
		Vec3 eye = p.getEyePosition(), dir = p.getLookAngle();
		double wall = march(level, eye, dir, 24.0, 0.1);
		Vec3 end = eye.add(dir.scale(wall));
		Entity best = null;
		double bestD = Double.MAX_VALUE;
		for (Entity e : level.getEntities(p, new AABB(eye, end).inflate(1.0), e -> Sweep.isStandIn(e) && e.isAlive())) {
			var hitAt = e.getBoundingBox().inflate(0.3).clip(eye, end);
			if (hitAt.isPresent()) {
				double d = eye.distanceToSqr(hitAt.get());
				if (d < bestD) {
					bestD = d;
					best = e;
				}
			}
		}
		double reach = best != null ? Math.sqrt(bestD) : wall;
		for (double d = 1.0; d < reach; d += 0.6) {
			Vec3 q = eye.add(dir.scale(d));
			level.sendParticles(ParticleTypes.FLAME, q.x, q.y, q.z, 2, 0.05, 0.05, 0.05, 0.01);
		}
		level.playSound(null, p.getX(), p.getY(), p.getZ(), SoundEvents.FIRECHARGE_USE, SoundSource.PLAYERS, 1.0F, 1.0F);
		if (best instanceof LivingEntity le) {
			le.igniteForSeconds(8.0F);
			le.hurtServer(level, p.damageSources().onFire(), 4.0F);
			System.out.println("[RECraft] fire charge: set " + le.getName().getString() + " on fire");
		}
		if (!p.hasInfiniteMaterials()) {
			st.shrink(1);
		}
		p.getCooldowns().addCooldown(st, 10);
	}
}
