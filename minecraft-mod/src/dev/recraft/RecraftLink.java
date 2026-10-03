package dev.recraft;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Set;
import java.util.List;
import net.fabricmc.api.ModInitializer;
import net.fabricmc.fabric.api.command.v2.CommandRegistrationCallback;
import net.fabricmc.fabric.api.event.player.AttackEntityCallback;
import net.fabricmc.fabric.api.event.player.UseBlockCallback;
import net.fabricmc.fabric.api.event.player.UseItemCallback;
import net.minecraft.ChatFormatting;
import net.minecraft.commands.Commands;
import net.minecraft.network.chat.Component;
import net.minecraft.world.entity.EquipmentSlot;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.core.BlockPos;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;

/**
 * RECraft's Minecraft side (Resident Evil 4 x Minecraft), next to SkyCraft. It shares SkyCraft's link memory (a free block at
 * 0x1B200, between SkyCraft's event ring and its entity table):
 *  - one shared health: reports the player's health every tick and applies the health RE4 asks for,
 *  - RE4's merchant decides the gear: Leon's attache case (weapons, tune-ups, case size) maps to Minecraft
 *    swords, axes, bow, armour and enchantments (see Gear); ammo Leon picks up becomes arrows, crates you
 *    break give planks, herbs become steak (one per herb),
 *  - worn armour softens RE4's hits (RE4 applies the multiplier published here),
 *  - the solid blocks around the player, so RE4's enemies can't walk through what you build.
 */
public final class RecraftLink implements ModInitializer {
	static final long BASE = 0x1B200;
	static final int MAGIC = 0x58433452;   // "R4CX"
	static final long O_MAGIC = 0x00, O_HEARTBEAT = 0x04, O_HEALTH = 0x08, O_MAX = 0x0C, O_FLAGS = 0x10, O_DEATHS = 0x14;
	static final long O_REQ_SEQ = 0x20, O_REQ_HEALTH = 0x24;
	static final long O_GEAR_SEQ = 0x28, O_TIERS = 0x2C, O_SWORD_E = 0x30, O_BOW_E = 0x34, O_ARMOR_MUL = 0x38, O_ARMOUR_E = 0x3C;
	static final long O_ARROWS = 0x54, O_PLANKS = 0x58, O_AXE_E = 0x5C, O_STEAK = 0x18;
	static final String KIT_TAG = "rc.v15";
	static final long O_BLK_SEQ = 0x40, O_BLK_X = 0x44, O_BLK_Y = 0x48, O_BLK_Z = 0x4C, O_BLK_COUNT = 0x50, O_BLOCKS = 0x60;
	static final long END = 0x1C000;
	static final long O_HIT = 0xDE0;   // the DLL's last-hit record (HitHud) sits after the block list
	static final long O_TNT = 0xDC0, O_PEARL = 0xDC4, O_FIRE_CHARGE = 0xDC8, O_AXE_COOLDOWN = 0xDCC;   // grenades -> throwables (totals)
	static final int MAX_BLOCKS = (int) ((O_TNT - O_BLOCKS) / 6);
	static final ValueLayout.OfInt I = ValueLayout.JAVA_INT;
	static final ValueLayout.OfFloat F = ValueLayout.JAVA_FLOAT;
	static final ValueLayout.OfShort S = ValueLayout.JAVA_SHORT;

	private Field shmField;
	private int lastReq;
	private boolean reqInit;
	private int ticks, heartbeat, deaths, blockSeq;
	private boolean wasAlive = true, warned;
	// Blocks from earlier sessions are cleared (the mirror world is otherwise empty).
	private final Set<Long> cleaned = new HashSet<>();
	private int removedTotal;
	// pesetas (RE4's) for the merchant
	private int lastArrows, lastPlanks, lastSteak;
	private final int[] lastThrow = new int[3];
	private Gear.Spec lastSpec;
	private Object resetFor;   // the server (world session) the inventory was last reset for

	@Override
	public void onInitialize() {
		try {
			Class<?> link = Class.forName("dev.skycraft.link.SkyLink");
			this.shmField = link.getDeclaredField("shm");
			this.shmField.setAccessible(true);
		} catch (Throwable t) {
			System.out.println("[RECraft] SkyCraft's link not found - RECraft's Minecraft side is off: " + t);
			return;
		}
		ServerTickEvents.END_SERVER_TICK.register(this::tick);
		// a fully charged sword swing sweeps every RE4 enemy in front of you, Minecraft style but wider
		AttackEntityCallback.EVENT.register((player, world, hand, entity, hit) -> {
			// the axe swings once per cooldown; a fully charged sword sweeps every RE4 enemy in front of you
			var r = Throwables.onAttack(player, entity);
			if (r != net.minecraft.world.InteractionResult.PASS) {
				return r;
			}
			return !world.isClientSide() && player instanceof ServerPlayer sp ? Sweep.onAttack(sp, entity) : net.minecraft.world.InteractionResult.PASS;
		});
		// RE4's grenades: TNT lights when placed, ender pearls teleport, fire charges set enemies alight
		UseItemCallback.EVENT.register(Throwables::onUseItem);
		UseBlockCallback.EVENT.register(Throwables::onUseBlock);
		CommandRegistrationCallback.EVENT.register((dispatcher, ctx, selection) -> dispatcher.register(Commands.literal("recraft")
			.then(Commands.literal("reset").executes(c -> {
				ServerPlayer p = c.getSource().getPlayerOrException();
				Gear.reset(p);
				this.lastSpec = null;
				c.getSource().sendSuccess(() -> Component.literal("RECraft: inventory reset - your gear comes back from Leon's attache case"), false);
				return 1;
			}))));
		System.out.println("[RECraft] Minecraft side ready (shared health, RE4's merchant, blocks stop enemies)");
	}

	private void tick(MinecraftServer server) {
		try {
			MemorySegment s = (MemorySegment) this.shmField.get(null);
			if (s == null || s.byteSize() < END) {
				this.reqInit = false;
				return;
			}
			List<ServerPlayer> players = server.getPlayerList().getPlayers();
			if (players.isEmpty()) {
				return;
			}
			ServerPlayer player = players.getFirst();
			ServerLevel level = player.level();
			this.ticks++;

			// ---- health requested by RE4 (Leon's health changed: hits, herbs, death, continue)
			int reqSeq = s.get(I, BASE + O_REQ_SEQ);
			java.lang.invoke.VarHandle.acquireFence();   // the health that goes with this sequence number, not an older one
			if (!this.reqInit) {
				this.reqInit = true;
				this.lastReq = reqSeq;
				this.lastArrows = s.get(I, BASE + O_ARROWS);
				this.lastPlanks = s.get(I, BASE + O_PLANKS);
				this.lastSteak = s.get(I, BASE + O_STEAK);
				this.lastThrow[0] = s.get(I, BASE + O_TNT);
				this.lastThrow[1] = s.get(I, BASE + O_PEARL);
				this.lastThrow[2] = s.get(I, BASE + O_FIRE_CHARGE);
			} else if (reqSeq != this.lastReq) {
				this.lastReq = reqSeq;
				float want = s.get(F, BASE + O_REQ_HEALTH);
				if (player.isAlive()) {
					if (want <= 0.0F) {
						player.kill(level);
					} else {
						float max = player.getMaxHealth();
						float target = Math.min(want, max);
						float cur = player.getHealth();
						if (target < cur - 0.05F) {
							// worn armour takes wear from RE4's hits
							int wear = Math.max(1, (int) ((cur - target) / 4.0F));
							for (EquipmentSlot es : ARMOUR_SLOTS) {
								ItemStack a = player.getItemBySlot(es);
								if (!a.isEmpty() && a.isDamageableItem()) {
									a.hurtAndBreak(wear, player, es);
								}
							}
							player.hurtServer(level, level.damageSources().magic(), cur - target);   // red flash + sound
						}
						if (player.isAlive()) {
							player.setHealth(target);
						}
					}
				}
			}

			// ---- report health
			boolean alive = player.isAlive() && !player.isDeadOrDying();
			if (this.wasAlive && !alive) {
				this.deaths++;
			}
			this.wasAlive = alive;
			s.set(F, BASE + O_HEALTH, alive ? player.getHealth() : 0.0F);
			s.set(F, BASE + O_MAX, player.getMaxHealth());
			s.set(I, BASE + O_FLAGS, alive ? 1 : 0);
			s.set(I, BASE + O_DEATHS, this.deaths);
			s.set(I, BASE + O_MAGIC, MAGIC);
			s.set(I, BASE + O_HEARTBEAT, ++this.heartbeat);

			// ---- new session: clear blocks built in earlier sessions, area by area, before you get there
			if (this.ticks > 100 && player.getY() > -500 && this.ticks % 10 == 5) {
				this.cleanAround(level, player);
			}

			// ---- the hittable stand-ins for RE4's enemies and breakables don't stop you placing blocks
			if (this.ticks % 20 == 0) {
				for (net.minecraft.world.entity.Entity e : level.getAllEntities()) {
					if (e.getClass().getName().equals("dev.skycraft.combat.SkyrimActorEntity")) {
						e.blocksBuilding = false;
					}
				}
			}

			// ---- RE4's merchant: the gear follows Leon's attache case; ammo -> arrows, crates -> planks
			// every time the world is started: a fresh inventory (the gear comes back from Leon's case)
			if (this.resetFor != server && player.isAlive()) {
				this.resetFor = server;
				Gear.reset(player);
				this.lastSpec = null;
				player.sendSystemMessage(Component.literal("RECraft: fresh start - your sword, bow and armour come from Leon's attache case. Ammo Leon picks up turns into arrows, herbs into steak, crates you break into planks.").withStyle(ChatFormatting.GOLD));
				System.out.println("[RECraft] new session: inventory reset");
			}
			if (this.ticks % 10 == 0 && player.isAlive()) {
				Gear.Spec spec = Gear.read(s, BASE);
				if (spec != null) {
					List<String> changed = Gear.reconcile(player, spec);
					if (this.lastSpec != null && !spec.same(this.lastSpec)) {
						Gear.announce(player, changed, Gear.source(s, BASE));
					} else if (!changed.isEmpty()) {
						System.out.println("[RECraft] gear restored: " + String.join(", ", changed));
					}
					this.lastSpec = spec;
				}
				s.set(F, BASE + O_ARMOR_MUL, Gear.armourMultiplier(player));
			}
			Throwables.tick(level);
			int axeCd = s.get(I, BASE + O_AXE_COOLDOWN);
			if (axeCd > 0 && axeCd < 600) {
				Throwables.axeCooldownSec = axeCd;
			}
			{   // RE4's grenades Leon picks up: hand grenade -> TNT, flash -> ender pearl, incendiary -> fire charge
				long[] offs = { O_TNT, O_PEARL, O_FIRE_CHARGE };
				net.minecraft.world.item.Item[] items = { Items.TNT, Items.ENDER_PEARL, Items.FIRE_CHARGE };
				String[] what = { "TNT", "ender pearl", "fire charge" };
				for (int k = 0; k < 3; k++) {
					int v = s.get(I, BASE + offs[k]);
					int d = v - this.lastThrow[k];
					this.lastThrow[k] = v;
					if (d > 0 && d < 100 && player.isAlive()) {
						Gear.give(player, items[k], d);
						player.sendSystemMessage(Component.literal("+" + d + " " + what[k] + " (grenade)").withStyle(ChatFormatting.GOLD), true);
					}
				}
			}
			int steak = s.get(I, BASE + O_STEAK);
			if (steak != this.lastSteak) {   // RE4's herbs are food now: one steak per herb
				int d = steak - this.lastSteak;
				this.lastSteak = steak;
				if (d > 0 && d < 1000 && player.isAlive()) {
					Gear.give(player, Items.COOKED_BEEF, d);
					player.sendSystemMessage(Component.literal("+" + d + " steak (herb)").withStyle(ChatFormatting.GREEN), true);
				}
			}
			int arrows = s.get(I, BASE + O_ARROWS), planks = s.get(I, BASE + O_PLANKS);
			if (arrows != this.lastArrows) {
				int d = arrows - this.lastArrows;
				this.lastArrows = arrows;
				if (d > 0 && d < 10000 && player.isAlive()) {
					Gear.give(player, Items.ARROW, Math.min(d, 640));
					player.sendSystemMessage(Component.literal("+" + d + " arrows (ammo)").withStyle(ChatFormatting.GRAY), true);
				}
			}
			if (planks != this.lastPlanks) {
				int d = planks - this.lastPlanks;
				this.lastPlanks = planks;
				if (d > 0 && d < 10000 && player.isAlive()) {
					Gear.give(player, Items.OAK_PLANKS, Math.min(d, 640));
					player.sendSystemMessage(Component.literal("+" + d + " planks (crate)").withStyle(ChatFormatting.GRAY), true);
				}
			}

			// ---- solid blocks near the player (RE4 keeps its enemies out of them)
			if (this.ticks % 10 == 0) {
				BlockPos c = player.blockPosition();
				BlockPos.MutableBlockPos p = new BlockPos.MutableBlockPos();
				List<int[]> found = new ArrayList<>();
				for (int dy = -3; dy <= 6; dy++) {
					for (int dx = -16; dx <= 16; dx++) {
						for (int dz = -16; dz <= 16; dz++) {
							p.set(c.getX() + dx, c.getY() + dy, c.getZ() + dz);
							BlockState st = level.getBlockState(p);
							if (!st.isAir() && !st.getCollisionShape(level, p).isEmpty()) {
								found.add(new int[] { dx, dy, dz, dx * dx + dz * dz + dy * dy });
							}
						}
					}
				}
				found.sort((a, b) -> Integer.compare(a[3], b[3]));
				int n = Math.min(found.size(), MAX_BLOCKS);
				s.set(I, BASE + O_BLK_SEQ, ++this.blockSeq * 2 - 1);
				java.lang.invoke.VarHandle.fullFence();   // the RE4 side reads this as a seqlock: odd first, data, then even
				s.set(I, BASE + O_BLK_X, c.getX());
				s.set(I, BASE + O_BLK_Y, c.getY());
				s.set(I, BASE + O_BLK_Z, c.getZ());
				s.set(I, BASE + O_BLK_COUNT, n);
				for (int i = 0; i < n; i++) {
					int[] b = found.get(i);
					long o = BASE + O_BLOCKS + i * 6L;
					s.set(S, o, (short) b[0]);
					s.set(S, o + 2, (short) b[1]);
					s.set(S, o + 4, (short) b[2]);
				}
				java.lang.invoke.VarHandle.releaseFence();
				s.set(I, BASE + O_BLK_SEQ, this.blockSeq * 2);
			}
		} catch (Throwable t) {
			if (!this.warned) {
				this.warned = true;
				System.out.println("[RECraft] Minecraft side error (continuing): " + t);
				t.printStackTrace();
			}
		}
	}

	private static final EquipmentSlot[] ARMOUR_SLOTS = { EquipmentSlot.HEAD, EquipmentSlot.CHEST, EquipmentSlot.LEGS, EquipmentSlot.FEET };

	/** Clears each 16x16 area the first time it comes within ~40 blocks this session (blocks there are from before). */
	private void cleanAround(ServerLevel level, ServerPlayer player) {
		BlockPos c = player.blockPosition();
		int pcx = c.getX() >> 4, pcz = c.getZ() >> 4;
		BlockState air = Blocks.AIR.defaultBlockState();
		BlockPos.MutableBlockPos p = new BlockPos.MutableBlockPos();
		int removed = 0;
		int y0 = c.getY() - 24, y1 = c.getY() + 40;
		for (int cx = pcx - 2; cx <= pcx + 2; cx++) {
			for (int cz = pcz - 2; cz <= pcz + 2; cz++) {
				long key = ((long) cx << 32) ^ (cz & 0xFFFFFFFFL);
				if (!this.cleaned.add(key)) {
					continue;
				}
				// only the chunk's 16-block sections that hold anything: the mirror world is mostly empty
				// (0.10 read all 16 x 16 x 65 blocks of 25 chunks in one tick when a session started)
				net.minecraft.world.level.chunk.LevelChunk chunk = level.getChunk(cx, cz);
				net.minecraft.world.level.chunk.LevelChunkSection[] sections = chunk.getSections();
				for (int si = Math.max(0, level.getSectionIndex(y0)); si <= Math.min(sections.length - 1, level.getSectionIndex(y1)); si++) {
					if (sections[si] == null || sections[si].hasOnlyAir()) {
						continue;
					}
					int sy0 = level.getSectionYFromSectionIndex(si) << 4;
					for (int y = Math.max(y0, sy0); y <= Math.min(y1, sy0 + 15); y++) {
						for (int x = cx * 16; x < cx * 16 + 16; x++) {
							for (int z = cz * 16; z < cz * 16 + 16; z++) {
								p.set(x, y, z);
								if (!level.getBlockState(p).isAir()) {
									level.setBlock(p, air, 3, 512);
									removed++;
								}
							}
						}
					}
				}
			}
		}
		if (removed > 0) {
			this.removedTotal += removed;
			System.out.println("[RECraft] cleared " + removed + " blocks left from an earlier session (" + this.removedTotal + " this session)");
		}
	}
}
