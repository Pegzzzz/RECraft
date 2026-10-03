package dev.recraft;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.ChatFormatting;
import net.minecraft.core.Holder;
import net.minecraft.core.component.DataComponents;
import net.minecraft.core.registries.Registries;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.sounds.SoundEvents;
import net.minecraft.sounds.SoundSource;
import net.minecraft.util.Unit;
import net.minecraft.world.entity.EquipmentSlot;
import net.minecraft.world.entity.player.Inventory;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.item.enchantment.Enchantment;
import net.minecraft.world.item.enchantment.EnchantmentHelper;
import net.minecraft.world.item.enchantment.Enchantments;

/**
 * RE4's merchant decides the Minecraft gear. RECraft's DLL reads Leon's attache case and publishes what it
 * maps to (RecraftLink.O_TIERS...): handguns and magnums -> the sword (Handgun wooden, Punisher stone, Red9
 * iron, Blacktail diamond, magnums netherite), shotguns -> the axe, rifles -> the bow, the case size -> armour,
 * the TMP -> Protection, and each weapon's tune-ups -> enchantments. This keeps the inventory matching it.
 * Gear from the merchant doesn't wear out (RE4's weapons don't either); arrows and blocks do get used up.
 */
final class Gear {
	enum Kind {
		SWORD(null), AXE(null), BOW(null), HEAD(EquipmentSlot.HEAD), CHEST(EquipmentSlot.CHEST), LEGS(EquipmentSlot.LEGS), FEET(EquipmentSlot.FEET);
		final EquipmentSlot slot;
		Kind(EquipmentSlot slot) {
			this.slot = slot;
		}
	}

	static final Item[] SWORDS = { null, Items.WOODEN_SWORD, Items.STONE_SWORD, Items.IRON_SWORD, Items.DIAMOND_SWORD, Items.NETHERITE_SWORD };
	static final Item[] AXES = { null, Items.WOODEN_AXE, Items.STONE_AXE, Items.IRON_AXE, Items.DIAMOND_AXE, Items.NETHERITE_AXE };
	static final Item[] BOWS = { null, Items.BOW };
	static final Item[] HEADS = { null, Items.LEATHER_HELMET, Items.CHAINMAIL_HELMET, Items.IRON_HELMET, Items.DIAMOND_HELMET, Items.NETHERITE_HELMET };
	static final Item[] CHESTS = { null, Items.LEATHER_CHESTPLATE, Items.CHAINMAIL_CHESTPLATE, Items.IRON_CHESTPLATE, Items.DIAMOND_CHESTPLATE, Items.NETHERITE_CHESTPLATE };
	static final Item[] LEGGINGS = { null, Items.LEATHER_LEGGINGS, Items.CHAINMAIL_LEGGINGS, Items.IRON_LEGGINGS, Items.DIAMOND_LEGGINGS, Items.NETHERITE_LEGGINGS };
	static final Item[] BOOTS = { null, Items.LEATHER_BOOTS, Items.CHAINMAIL_BOOTS, Items.IRON_BOOTS, Items.DIAMOND_BOOTS, Items.NETHERITE_BOOTS };
	static final Item[][] OTHERS = {
		{ Items.GOLDEN_SWORD, Items.COPPER_SWORD }, { Items.GOLDEN_AXE, Items.COPPER_AXE }, { Items.CROSSBOW },
		{ Items.GOLDEN_HELMET, Items.COPPER_HELMET, Items.TURTLE_HELMET }, { Items.GOLDEN_CHESTPLATE, Items.COPPER_CHESTPLATE },
		{ Items.GOLDEN_LEGGINGS, Items.COPPER_LEGGINGS }, { Items.GOLDEN_BOOTS, Items.COPPER_BOOTS } };

	/** What RE4 published. */
	record Spec(int sword, int axe, int bow, int armour, byte[] swordE, byte[] axeE, byte[] bowE, int prot) {
		int tier(Kind k) {
			return switch (k) {
				case SWORD -> this.sword;
				case AXE -> this.axe;
				case BOW -> this.bow;
				default -> this.armour;
			};
		}

		boolean same(Spec o) {
			return o != null && this.sword == o.sword && this.axe == o.axe && this.bow == o.bow && this.armour == o.armour && this.prot == o.prot
				&& java.util.Arrays.equals(this.swordE, o.swordE) && java.util.Arrays.equals(this.axeE, o.axeE) && java.util.Arrays.equals(this.bowE, o.bowE);
		}
	}

	/** Where the last gear change came from (see announce). */
	static int source(MemorySegment s, long base) {
		return s.get(ValueLayout.JAVA_BYTE, base + RecraftLink.O_ARMOUR_E + 2);
	}

	static Spec read(MemorySegment s, long base) {
		byte[] t = bytes(s, base + RecraftLink.O_TIERS);
		byte[] a = bytes(s, base + RecraftLink.O_ARMOUR_E);
		if (a[1] != 1) {
			return null;   // RE4 hasn't read Leon's case yet
		}
		return new Spec(t[0], t[1], t[2], t[3], bytes(s, base + RecraftLink.O_SWORD_E), bytes(s, base + RecraftLink.O_AXE_E), bytes(s, base + RecraftLink.O_BOW_E), a[0]);
	}

	static byte[] bytes(MemorySegment s, long o) {
		byte[] b = new byte[4];
		for (int i = 0; i < 4; i++) {
			b[i] = s.get(ValueLayout.JAVA_BYTE, o + i);
		}
		return b;
	}

	static Item[] tiers(Kind k) {
		return switch (k) {
			case SWORD -> SWORDS;
			case AXE -> AXES;
			case BOW -> BOWS;
			case HEAD -> HEADS;
			case CHEST -> CHESTS;
			case LEGS -> LEGGINGS;
			case FEET -> BOOTS;
		};
	}

	static boolean isKind(ItemStack st, Kind k) {
		if (st.isEmpty()) {
			return false;
		}
		Item it = st.getItem();
		for (Item i : tiers(k)) {
			if (i == it) {
				return true;
			}
		}
		for (Item i : OTHERS[k.ordinal()]) {
			if (i == it) {
				return true;
			}
		}
		return false;
	}

	static Holder<Enchantment> ench(Player p, ResourceKey<Enchantment> key) {
		return p.registryAccess().lookupOrThrow(Registries.ENCHANTMENT).getOrThrow(key);
	}

	static void add(Player p, ItemStack st, ResourceKey<Enchantment> key, int level) {
		if (level > 0) {
			st.enchant(ench(p, key), level);
		}
	}

	static ItemStack build(Player p, Spec sp, Kind k) {
		Item[] t = tiers(k);
		int tier = Math.max(0, Math.min(sp.tier(k), t.length - 1));
		if (tier == 0) {
			return ItemStack.EMPTY;
		}
		ItemStack st = new ItemStack(t[tier]);
		switch (k) {
			case SWORD -> {
				add(p, st, Enchantments.SHARPNESS, sp.swordE()[0]);
				add(p, st, Enchantments.SWEEPING_EDGE, sp.swordE()[1]);
				add(p, st, Enchantments.FIRE_ASPECT, sp.swordE()[2]);
				add(p, st, Enchantments.KNOCKBACK, sp.swordE()[3]);
			}
			case AXE -> {
				add(p, st, Enchantments.SHARPNESS, sp.axeE()[0]);
				add(p, st, Enchantments.FIRE_ASPECT, sp.axeE()[2]);
				add(p, st, Enchantments.KNOCKBACK, sp.axeE()[3]);
			}
			case BOW -> {
				add(p, st, Enchantments.POWER, sp.bowE()[0]);
				add(p, st, Enchantments.PUNCH, sp.bowE()[1]);
				add(p, st, Enchantments.FLAME, sp.bowE()[2]);
				add(p, st, Enchantments.INFINITY, sp.bowE()[3]);
			}
			default -> add(p, st, Enchantments.PROTECTION, sp.prot());
		}
		st.set(DataComponents.UNBREAKABLE, Unit.INSTANCE);
		return st;
	}

	/** Makes the inventory hold exactly the gear RE4's case maps to. Returns what changed (for the message). */
	static List<String> reconcile(ServerPlayer p, Spec sp) {
		List<String> changed = new ArrayList<>();
		if (!p.containerMenu.getCarried().isEmpty()) {
			return changed;   // something is on the cursor: wait
		}
		Inventory inv = p.getInventory();
		for (Kind k : Kind.values()) {
			ItemStack want = build(p, sp, k);
			List<Integer> slots = new ArrayList<>();
			for (int i = 0; i < inv.getContainerSize(); i++) {
				if (isKind(inv.getItem(i), k)) {
					slots.add(i);
				}
			}
			if (want.isEmpty()) {
				for (int i : slots) {
					inv.setItem(i, ItemStack.EMPTY);
				}
				continue;
			}
			if (slots.size() == 1 && ItemStack.isSameItemSameComponents(inv.getItem(slots.get(0)), want)) {
				continue;
			}
			if (!slots.isEmpty()) {
				inv.setItem(slots.get(0), want);
				for (int j = 1; j < slots.size(); j++) {
					inv.setItem(slots.get(j), ItemStack.EMPTY);
				}
			} else if (k.slot != null && p.getItemBySlot(k.slot).isEmpty()) {
				p.setItemSlot(k.slot, want);
			} else if (!inv.add(want.copy())) {
				p.drop(want, false, net.minecraft.util.Prediction.SERVER_ONLY);
			}
			changed.add(describe(p, want));
		}
		if (sp.bowE()[3] > 0) {   // Infinity still needs one arrow to shoot
			boolean arrow = false;
			for (int i = 0; i < inv.getContainerSize(); i++) {
				arrow |= inv.getItem(i).getItem() == Items.ARROW;
			}
			if (!arrow) {
				give(p, new ItemStack(Items.ARROW, 1));
			}
		}
		return changed;
	}

	static String describe(Player p, ItemStack st) {
		StringBuilder b = new StringBuilder(st.getHoverName().getString());
		List<String> e = new ArrayList<>();
		for (var entry : st.getEnchantments().entrySet()) {
			e.add(Enchantment.getFullname(entry.getKey(), entry.getIntValue()).getString());
		}
		if (!e.isEmpty()) {
			b.append(" (").append(String.join(", ", e)).append(")");
		}
		return b.toString();
	}

	static void give(ServerPlayer p, ItemStack st) {
		if (!p.getInventory().add(st)) {
			p.drop(st, false, net.minecraft.util.Prediction.SERVER_ONLY);
		}
	}

	static void give(ServerPlayer p, Item item, int count) {
		while (count > 0) {
			int n = Math.min(count, item.getDefaultMaxStackSize());
			give(p, new ItemStack(item, n));
			count -= n;
		}
	}

	/** source (published by the DLL with the gear): 1 bought or tuned at the merchant, 2 picked up, 0 other. */
	static void announce(ServerPlayer p, List<String> changed, int source) {
		if (changed.isEmpty()) {
			return;
		}
		String from = source == 1 ? "From the merchant: " : source == 2 ? "Found: " : "New gear: ";
		String msg = from + String.join(", ", changed);
		System.out.println("[RECraft] gear: " + msg);
		p.sendSystemMessage(Component.literal(msg).withStyle(ChatFormatting.GOLD), true);
		p.level().playSound(null, p.getX(), p.getY(), p.getZ(), SoundEvents.ARMOR_EQUIP_IRON.value(), SoundSource.PLAYERS, 0.8F, 1.0F);
	}

	/** A new start: empty inventory with a few arrows and planks; the gear comes from Leon's case. */
	static void reset(ServerPlayer p) {
		Inventory inv = p.getInventory();
		for (int i = 0; i < inv.getContainerSize(); i++) {
			inv.setItem(i, ItemStack.EMPTY);
		}
		for (String t : new ArrayList<>(p.entityTags())) {
			if (t.startsWith("rc.")) {
				p.removeTag(t);
			}
		}
		p.addTag(RecraftLink.KIT_TAG);
		give(p, new ItemStack(Items.ARROW, 16));
		give(p, new ItemStack(Items.OAK_PLANKS, 16));
	}

	/** Damage multiplier for RE4's hits on Leon from the worn armour (RE4 applies it). */
	static float armourMultiplier(ServerPlayer p) {
		int armor = p.getArmorValue();
		int prot = 0;
		Holder<Enchantment> h = ench(p, Enchantments.PROTECTION);
		for (EquipmentSlot s : new EquipmentSlot[] { EquipmentSlot.HEAD, EquipmentSlot.CHEST, EquipmentSlot.LEGS, EquipmentSlot.FEET }) {
			prot += EnchantmentHelper.getItemEnchantmentLevel(h, p.getItemBySlot(s));
		}
		float a = 1.0F - 0.6F * Math.min(armor, 20) / 20.0F;
		float b = 1.0F - 0.025F * Math.min(prot, 16);
		return Math.max(0.2F, a * b);
	}
}
