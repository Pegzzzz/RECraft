package dev.recraft;

import java.util.List;
import net.minecraft.core.particles.ParticleTypes;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.sounds.SoundEvents;
import net.minecraft.sounds.SoundSource;
import net.minecraft.tags.ItemTags;
import net.minecraft.world.InteractionResult;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.ai.attributes.Attributes;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.phys.AABB;

/**
 * A Minecraft-style sweep that reaches RE4's crowds: a fully charged sword swing also cuts every RE4
 * enemy (and crate) in a wide arc in front of you, not only the ones touching your target. The sweep
 * hits go through the core's stand-ins like any other hit, so RE4 reacts to each one.
 * Damage: 40% of the sword's attack, up to 85% with Sweeping Edge III.
 */
final class Sweep {
	static final double RANGE = 3.5, HALF_ARC_COS = Math.cos(Math.toRadians(70));

	static InteractionResult onAttack(ServerPlayer player, Entity target) {
		ItemStack hand = player.getMainHandItem();
		if (!hand.is(ItemTags.SWORDS) || player.getAttackStrengthScale(0.5F) < 0.9F || !isStandIn(target)) {
			return InteractionResult.PASS;
		}
		ServerLevel level = player.level();
		double ratio = player.getAttributeValue(Attributes.SWEEPING_DAMAGE_RATIO);
		float dmg = (float) (player.getAttributeValue(Attributes.ATTACK_DAMAGE) * (0.4 + 0.6 * Math.min(1.0, ratio)));
		double yaw = Math.toRadians(player.getYRot());
		double fx = -Math.sin(yaw), fz = Math.cos(yaw);
		AABB box = player.getBoundingBox().inflate(RANGE, 1.5, RANGE);
		List<Entity> near = level.getEntities(player, box, e -> e != target && isStandIn(e) && e.isAlive());
		DamageSource src = player.damageSources().playerAttack(player);
		int hits = 0;
		for (Entity e : near) {
			double dx = e.getX() - player.getX(), dz = e.getZ() - player.getZ();
			double d = Math.sqrt(dx * dx + dz * dz);
			if (d > RANGE + e.getBbWidth() * 0.5 || (d > 0.3 && (dx * fx + dz * fz) / d < HALF_ARC_COS)) {
				continue;
			}
			LivingEntity le = (LivingEntity) e;
			if (le.hurtServer(level, src, dmg)) {
				le.knockback(0.4, -fx, -fz, src, dmg);
				hits++;
			}
		}
		level.sendParticles(ParticleTypes.SWEEP_ATTACK, player.getX() + fx, player.getY(0.5), player.getZ() + fz, 0, fx, 0.0, fz, 0.0);
		if (hits > 0) {
			level.playSound(null, player.getX(), player.getY(), player.getZ(), SoundEvents.PLAYER_ATTACK_SWEEP, SoundSource.PLAYERS, 1.0F, 1.0F);
			System.out.println("[RECraft] sweep: " + hits + " more enemies hit for " + dmg);
		}
		return InteractionResult.PASS;   // the normal hit on the target still happens
	}

	static boolean isStandIn(Entity e) {
		return e instanceof LivingEntity && e.getClass().getName().equals("dev.recraft.core.combat.Re4ActorEntity");
	}
}
