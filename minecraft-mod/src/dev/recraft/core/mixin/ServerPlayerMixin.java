package dev.recraft.core.mixin;

import dev.recraft.core.RecraftCore;
import dev.recraft.core.combat.CoreCombat;
import dev.recraft.core.combat.Re4ActorEntity;
import dev.recraft.core.link.Proto;
import dev.recraft.core.link.CoreLink;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.server.level.ServerPlayer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(ServerPlayer.class)
public abstract class ServerPlayerMixin {
	/** Critical hits on a RE4 actor are flagged so RE4 can play them up. */
	@Inject(method = "crit", at = @At("HEAD"))
	private void recraft$critRe4(Entity entity, CallbackInfo ci) {
		if (entity instanceof Re4ActorEntity proxy) {
			proxy.markCritical();
		}
	}

	/** Dying in Minecraft is dying in RE4: the host's through the link, a guest's through theirs. */
	@Inject(method = "die", at = @At("HEAD"))
	private void recraft$diesInRe4(DamageSource source, CallbackInfo ci) {
		ServerPlayer self = (ServerPlayer) (Object) this;
		int attacker = CoreCombat.attackerFormId(source);
		if (!dev.recraft.core.net.CoreNet.isHost(self)) {
			if (net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking.canSend(self, dev.recraft.core.net.CoreNet.Died.TYPE)) {
				net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking.send(self, new dev.recraft.core.net.CoreNet.Died(attacker));
			}
			RecraftCore.LOG.info("RECraft: guest {} died ({}); telling their RE4", self.getPlainTextName(), source.getMsgId());
			return;
		}
		if (CoreLink.active()) {
			CoreLink.pushEvent(Proto.EV_PLAYER_DIED, attacker, 0, 0, 0, 0, 0);
			RecraftCore.LOG.info("RECraft: player died ({}); telling RE4", source.getMsgId());
		}
	}
}
