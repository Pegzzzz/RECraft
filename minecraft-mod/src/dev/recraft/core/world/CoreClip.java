package dev.recraft.core.world;

import java.util.ArrayList;
import java.util.List;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;

/**
 * Makes Minecraft ray casts (arrows and other projectiles, the crosshair pick) hit RE4's exact
 * collision triangles. Vanilla still clips against real Minecraft blocks; whichever is nearer wins.
 */
public final class CoreClip {
	private CoreClip() {
	}

	public enum Use {
		/** A projectile: the hit cell is the one the surface is in (it sticks there). */
		PROJECTILE,
		/** The player's crosshair: the hit cell is where a block placed against the surface goes. */
		PICK
	}

	private static final ThreadLocal<List<CoreTri>> SCRATCH = ThreadLocal.withInitial(ArrayList::new);

	public static BlockHitResult refine(Vec3 from, Vec3 to, BlockHitResult vanilla, Use use) {
		CoreRay.Hit hit = cast(from, to);
		if (use == Use.PICK) {
			// The walls of a dug hole (RE4 ground the crosshair meets from inside the hole).
			double limit = hit != null ? hit.t() : 1.0;
			if (vanilla.getType() != HitResult.Type.MISS) {
				limit = Math.min(limit, Math.sqrt(from.distanceToSqr(vanilla.getLocation()) / Math.max(from.distanceToSqr(to), 1e-9)));
			}
			CoreRay.Hit wall = digWall(from, to, limit);
			if (wall != null) {
				hit = wall;
				vanilla = BlockHitResult.miss(to, Direction.UP, BlockPos.containing(to));
			}
		}
		if (hit == null) {
			return vanilla;
		}
		Vec3 location = new Vec3(hit.x(), hit.y(), hit.z());
		if (vanilla.getType() != HitResult.Type.MISS && from.distanceToSqr(vanilla.getLocation()) <= from.distanceToSqr(location)) {
			return vanilla;
		}
		Direction face = Direction.values()[CoreRay.dominantFace(hit.nx(), hit.ny(), hit.nz())];
		int[] cell = use == Use.PICK ? CoreRay.placementCell(hit) : CoreRay.surfaceCell(hit);
		return new Re4HitResult(location, face, new BlockPos(cell[0], cell[1], cell[2]), hit);
	}

	private static final CoreTri STONE_WALL = new CoreTri(new float[9], 0, dev.recraft.core.link.Proto.TRI_DIGGABLE | (dev.recraft.core.link.Proto.DIG_STONE << dev.recraft.core.link.Proto.TRI_MATERIAL_SHIFT));

	/**
	 * Where the segment, having gone through dug cells, first enters an undug cell inside RE4's
	 * geometry (the wall of a hole, drawn by the client's DigWalls), before segment parameter
	 * {@code limit}; or null.
	 */
	static CoreRay.Hit digWall(Vec3 from, Vec3 to, double limit) {
		CoreDig.DugLookup dug = CoreDig.clientDug;
		if (dug == null) {
			return null;
		}
		double dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
		int x = (int) Math.floor(from.x), y = (int) Math.floor(from.y), z = (int) Math.floor(from.z);
		int stepX = dx > 0 ? 1 : -1, stepY = dy > 0 ? 1 : -1, stepZ = dz > 0 ? 1 : -1;
		double tDeltaX = dx == 0 ? Double.POSITIVE_INFINITY : Math.abs(1.0 / dx);
		double tDeltaY = dy == 0 ? Double.POSITIVE_INFINITY : Math.abs(1.0 / dy);
		double tDeltaZ = dz == 0 ? Double.POSITIVE_INFINITY : Math.abs(1.0 / dz);
		double tMaxX = dx == 0 ? Double.POSITIVE_INFINITY : ((dx > 0 ? x + 1 - from.x : from.x - x) * tDeltaX);
		double tMaxY = dy == 0 ? Double.POSITIVE_INFINITY : ((dy > 0 ? y + 1 - from.y : from.y - y) * tDeltaY);
		double tMaxZ = dz == 0 ? Double.POSITIVE_INFINITY : ((dz > 0 ? z + 1 - from.z : from.z - z) * tDeltaZ);
		boolean wasDug = dug.isDug(x, y, z);
		CoreDig.Probe probe = null;
		for (int i = 0; i < 64; i++) {
			double t;
			double nx = 0, ny = 0, nz = 0;
			if (tMaxX <= tMaxY && tMaxX <= tMaxZ) {
				t = tMaxX;
				tMaxX += tDeltaX;
				x += stepX;
				nx = -stepX;
			} else if (tMaxY <= tMaxZ) {
				t = tMaxY;
				tMaxY += tDeltaY;
				y += stepY;
				ny = -stepY;
			} else {
				t = tMaxZ;
				tMaxZ += tDeltaZ;
				z += stepZ;
				nz = -stepZ;
			}
			if (t > limit) {
				return null;
			}
			boolean isDug = dug.isDug(x, y, z);
			if (wasDug && !isDug) {
				double px = from.x + dx * t, py = from.y + dy * t, pz = from.z + dz * t;
				if (probe == null) {
					probe = new CoreDig.Probe().around(Math.min(from.x, to.x), Math.min(from.y, to.y), Math.min(from.z, to.z), Math.max(from.x, to.x), Math.max(from.y, to.y),
						Math.max(from.z, to.z));
				}
				if (probe.test(px - nx * 0.02, py - ny * 0.02, pz - nz * 0.02) > CoreDig.AIR) {
					return new CoreRay.Hit(t, px, py, pz, nx, ny, nz, probe.surface != null ? probe.surface : STONE_WALL);
				}
			}
			wasDug = isDug;
		}
		return null;
	}

	/** Nearest RE4 triangle hit on the segment, or null. */
	public static CoreRay.Hit cast(Vec3 from, Vec3 to) {
		List<CoreTri> tris = SCRATCH.get();
		tris.clear();
		CoreCollision.trianglesNear(new AABB(from, to).inflate(0.01), tris);
		if (tris.isEmpty()) {
			return null;
		}
		CoreRay.Hit hit = CoreRay.cast(tris, from.x, from.y, from.z, to.x, to.y, to.z);
		tris.clear();
		return hit;
	}

	/** A hit on RE4 geometry (not a Minecraft block). Keeps the exact surface normal and triangle. */
	public static final class Re4HitResult extends BlockHitResult {
		public final double nx, ny, nz;
		public final CoreRay.Hit hit;

		public Re4HitResult(Vec3 location, Direction direction, BlockPos pos, CoreRay.Hit hit) {
			super(location, direction, pos, false);
			this.nx = hit.nx();
			this.ny = hit.ny();
			this.nz = hit.nz();
			this.hit = hit;
		}
	}
}
