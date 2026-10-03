// collision.cpp - RE4 room collision for Minecraft.
//
// RECraft_rooms.bin (built offline from every room's SAT file with JADERLINK's RE4-SAT-EAT-TOOL)
// holds each room's collision triangles in the tool's units (RE4 world units / 100).
// When Leon enters a room, a worker thread converts that room to link collision messages:
// exact triangles (Minecraft's player collides with these) plus a 1/8-block voxel shell (blocks,
// items and mobs collide with these), bucketed into 8-block regions.
//
// The axis convention is checked against the real game once per room: of the candidate mappings,
// the one that puts a floor right under Leon's feet wins (and is logged).

#include "link.h"
#include <vector>
#include <map>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace collision {

#pragma pack(push, 1)
struct RoomEntry { uint32_t room, count, offset; };
struct ColRegion { int32_t minX, minY, minZ, maxX, maxY, maxZ; uint32_t epoch, count; };
struct ColTri { float v[9]; uint32_t flags; };
struct ColBlock { int32_t x, y, z; uint32_t pad; uint64_t bits[8]; };
#pragma pack(pop)

static std::vector<uint8_t> g_pack;
static std::map<uint32_t, RoomEntry> g_rooms;

bool Load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { BridgeLog("collision: %s not found - using a flat floor", path); return false; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); BridgeLog("collision: %s is empty - using a flat floor", path); return false; }
    g_pack.resize(n);
    size_t got = fread(g_pack.data(), 1, n, f); fclose(f);
    if (got != (size_t)n) { BridgeLog("collision: %s could not be read in full - using a flat floor", path); g_pack.clear(); return false; }
    if (n < 12 || memcmp(g_pack.data(), "R4CR", 4)) { BridgeLog("collision: bad pack"); g_pack.clear(); return false; }
    uint32_t count; memcpy(&count, g_pack.data() + 8, 4);
    for (uint32_t i = 0; i < count; i++) {
        RoomEntry e; memcpy(&e, g_pack.data() + 12 + i * 12, 12);
        if ((uint64_t)e.offset + (uint64_t)e.count * 36 <= (uint64_t)n) g_rooms[e.room] = e;
    }
    BridgeLog("collision: %u rooms loaded (%.1f MB)", (unsigned)g_rooms.size(), n / 1048576.0);
    return true;
}

// ------------------------------------------------------------------ build (worker thread)
struct Job { uint16_t room; float lx, ly, lz; uint32_t epoch; };
static CRITICAL_SECTION g_lock;
static bool g_lockInit = false;
static HANDLE g_wake = nullptr;
static Job  g_job{}; static bool g_jobPending = false;
static std::vector<uint8_t> g_out;      // ready messages for g_outEpoch
static uint32_t g_outEpoch = 0; static bool g_outReady = false;

static void Put(std::vector<uint8_t>& out, uint32_t type, const void* a, size_t na, const void* b = nullptr, size_t nb = 0) {
    uint32_t hdr[2] = {type, (uint32_t)(na + nb)};
    size_t at = out.size();
    out.resize(at + ((8 + na + nb + 7) & ~(size_t)7));
    memcpy(&out[at], hdr, 8);
    memcpy(&out[at + 8], a, na);
    if (nb) memcpy(&out[at + 8 + na], b, nb);
}

static std::vector<float> g_occ; static uint16_t g_occRoom = 0xFFFF; static uint32_t g_occVersion = 0;
// Copy of the current room's collision triangles (RE4 units) if it changed since `version`.
bool Occluders(uint32_t& version, uint16_t& room, std::vector<float>& out) {
    if (!g_lockInit) return false;
    EnterCriticalSection(&g_lock);
    bool changed = version != g_occVersion;
    if (changed) { out = g_occ; version = g_occVersion; room = g_occRoom; }
    LeaveCriticalSection(&g_lock);
    return changed;
}

static int FloorDiv8(int v) { return v >= 0 ? v / 8 : -((-v + 7) / 8); }

// The room's own collision as last sent (per 8-block region), so doors can be added and removed later
// by resending only the regions they touch.
struct StaticRegion { std::vector<ColTri> tris; std::map<int64_t, ColBlock> blocks; };
static std::map<int64_t, StaticRegion> g_static;
static uint32_t g_staticEpoch = 0;
static std::vector<int64_t> g_doorRegions;                 // regions the doors were in last time
static std::vector<float> g_doorTris; static uint32_t g_doorEpoch = 0; static bool g_doorPending = false;
static std::vector<uint8_t> g_doorOut; static uint32_t g_doorOutEpoch = 0; static bool g_doorOutReady = false;
static std::vector<int64_t> g_doorOutKeys;                  // regions in the update not taken yet
static int64_t Key3(int x, int y, int z) { return ((int64_t)(x & 0x1FFFFF) << 42) | ((int64_t)(y & 0x1FFFFF) << 21) | (int64_t)(z & 0x1FFFFF); }
static void Unkey3(int64_t k, int& x, int& y, int& z) {
    x = (int)((k >> 42) & 0x1FFFFF); y = (int)((k >> 21) & 0x1FFFFF); z = (int)(k & 0x1FFFFF);
    if (x & 0x100000) x -= 0x200000; if (y & 0x100000) y -= 0x200000; if (z & 0x100000) z -= 0x200000;
}
// Adds a triangle (Minecraft coordinates) to `regions`: exact collision + its 1/8-block voxel shell.
template<class R> static void AddTri(std::map<int64_t, R>& regions, const ColTri& ct) {
    float mn[3], mx[3];
    for (int a = 0; a < 3; a++) { mn[a] = fminf(ct.v[a], fminf(ct.v[3 + a], ct.v[6 + a])); mx[a] = fmaxf(ct.v[a], fmaxf(ct.v[3 + a], ct.v[6 + a])); }
    int r0[3], r1[3];
    for (int a = 0; a < 3; a++) { r0[a] = FloorDiv8((int)floorf(mn[a])); r1[a] = FloorDiv8((int)floorf(mx[a])); }
    for (int x = r0[0]; x <= r1[0]; x++) for (int y = r0[1]; y <= r1[1]; y++) for (int z = r0[2]; z <= r1[2]; z++) regions[Key3(x, y, z)].tris.push_back(ct);
    float e1 = sqrtf(powf(ct.v[3] - ct.v[0], 2) + powf(ct.v[4] - ct.v[1], 2) + powf(ct.v[5] - ct.v[2], 2));
    float e2 = sqrtf(powf(ct.v[6] - ct.v[0], 2) + powf(ct.v[7] - ct.v[1], 2) + powf(ct.v[8] - ct.v[2], 2));
    float e3 = sqrtf(powf(ct.v[6] - ct.v[3], 2) + powf(ct.v[7] - ct.v[4], 2) + powf(ct.v[8] - ct.v[5], 2));
    int steps = (int)ceilf(fmaxf(e1, fmaxf(e2, e3)) * 16.f) + 1;
    if (steps > 4000) steps = 4000;
    for (int i = 0; i <= steps; i++)
        for (int j = 0; j <= steps - i; j++) {
            float u = (float)i / steps, w = (float)j / steps, p[3];
            for (int a = 0; a < 3; a++) p[a] = ct.v[a] + (ct.v[3 + a] - ct.v[a]) * u + (ct.v[6 + a] - ct.v[a]) * w;
            int vx = (int)floorf(p[0] * 8), vy = (int)floorf(p[1] * 8), vz = (int)floorf(p[2] * 8);
            int bx = FloorDiv8(vx), by = FloorDiv8(vy), bz = FloorDiv8(vz);
            auto& rg = regions[Key3(FloorDiv8(bx), FloorDiv8(by), FloorDiv8(bz))];
            ColBlock& cb = rg.blocks[Key3(bx, by, bz)];
            cb.x = bx; cb.y = by; cb.z = bz;
            cb.bits[vy - by * 8] |= 1ull << ((vz - bz * 8) * 8 + (vx - bx * 8));
        }
}

// Doors (worker thread): the regions the doors touch now or touched before, resent as room + doors.
static void BuildDoors(const std::vector<float>& tris, uint32_t epoch) {
    std::map<int64_t, StaticRegion> doors;
    for (size_t i = 0; i + 8 < tris.size(); i += 9) {
        ColTri ct{}; for (int k = 0; k < 9; k++) ct.v[k] = tris[i + k] / (float)bridge::kUnitsPerBlock;
        AddTri(doors, ct);
    }
    std::vector<uint8_t> out;
    EnterCriticalSection(&g_lock);
    if (epoch != g_staticEpoch) {   // that room's collision isn't built yet: try again after it is
        if (!g_doorPending) { g_doorPending = true; g_doorTris = tris; g_doorEpoch = epoch; }
        LeaveCriticalSection(&g_lock); return;
    }
    std::vector<int64_t> keys = g_doorRegions;
    if (g_doorOutReady && g_doorOutEpoch == epoch) keys.insert(keys.end(), g_doorOutKeys.begin(), g_doorOutKeys.end());
    for (auto& kv : doors) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end()); keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    for (int64_t k : keys) {
        StaticRegion r;
        auto st = g_static.find(k);
        if (st != g_static.end()) r = st->second;
        auto dr = doors.find(k);
        if (dr != doors.end()) {
            r.tris.insert(r.tris.end(), dr->second.tris.begin(), dr->second.tris.end());
            for (auto& b : dr->second.blocks) {
                ColBlock& cb = r.blocks[b.first];
                if (!cb.bits[0] && !cb.bits[1] && !cb.bits[2] && !cb.bits[3] && !cb.bits[4] && !cb.bits[5] && !cb.bits[6] && !cb.bits[7]) { cb = b.second; continue; }
                for (int q = 0; q < 8; q++) cb.bits[q] |= b.second.bits[q];
            }
        }
        int rx, ry, rz; Unkey3(k, rx, ry, rz);
        ColRegion hdr{rx * 8, ry * 8, rz * 8, rx * 8 + 7, ry * 8 + 7, rz * 8 + 7, epoch, (uint32_t)r.tris.size()};
        Put(out, 3, &hdr, sizeof hdr, r.tris.data(), r.tris.size() * sizeof(ColTri));   // even when empty: replaces the old
        std::vector<ColBlock> blocks; for (auto& b : r.blocks) blocks.push_back(b.second);
        hdr.count = (uint32_t)blocks.size();
        Put(out, 2, &hdr, sizeof hdr, blocks.data(), blocks.size() * sizeof(ColBlock));
    }
    g_doorRegions.clear(); for (auto& kv : doors) g_doorRegions.push_back(kv.first);
    g_doorOutKeys = keys;
    g_doorOut.swap(out); g_doorOutEpoch = epoch; g_doorOutReady = true;
    LeaveCriticalSection(&g_lock);
    static int logs = 0;
    if (logs++ < 20) BridgeLog("collision: doors updated (%u door triangles, %u regions resent)", (unsigned)(tris.size() / 9), (unsigned)keys.size());
}

static void Build(const Job& job) {
    auto it = g_rooms.find(job.room);
    std::vector<uint8_t> out;
    if (it == g_rooms.end() || it->second.count == 0) {
        BridgeLog("collision: room %04X has no collision data", job.room);
        EnterCriticalSection(&g_lock); g_out.clear(); g_outEpoch = job.epoch; g_outReady = true; LeaveCriticalSection(&g_lock);
        return;
    }
    const float* raw = (const float*)(g_pack.data() + it->second.offset);
    uint32_t n = it->second.count;

    // Pick the axis mapping that puts a floor under Leon (tool units * 100 = RE4 units).
    static const float kScale[] = {100.f};
    static const int   kFlip[4][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    float bestS = 100.f; int bestF = 0, bestScore = -1;
    for (float s : kScale)
        for (int fi = 0; fi < 4; fi++) {
            int score = 0;
            float fx = (float)kFlip[fi][0], fz = (float)kFlip[fi][1];
            for (uint32_t t = 0; t < n; t++) {
                const float* v = raw + t * 9;
                float ax = v[0] * s * fx, az = v[2] * s * fz, bx = v[3] * s * fx, bz = v[5] * s * fz, cx = v[6] * s * fx, cz = v[8] * s * fz;
                float d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
                if (fabsf(d) < 1e-6f) continue;
                float w0 = ((bz - cz) * (job.lx - cx) + (cx - bx) * (job.lz - cz)) / d;
                float w1 = ((cz - az) * (job.lx - cx) + (ax - cx) * (job.lz - cz)) / d;
                float w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float y = (w0 * v[1] + w1 * v[4] + w2 * v[7]) * s;
                float dy = job.ly - y;
                if (dy > -150.f && dy < 400.f) score += 10;   // a floor right at Leon's feet
                else score += 1;                               // something above/below him
            }
            if (score > bestScore) { bestScore = score; bestS = s; bestF = fi; }
        }
    {   // Every room so far uses x+1 z+1. Leon's position can still be the previous room's when a new room is
        // asked for (the first frame after a door), so only take another mapping on clear evidence.
        int s0 = 0; float fx = 1.f, fz = 1.f; (void)fx; (void)fz;
        for (uint32_t t = 0; t < n; t++) {
            const float* v = raw + t * 9;
            float ax = v[0] * 100.f, az = v[2] * 100.f, bx = v[3] * 100.f, bz = v[5] * 100.f, cx = v[6] * 100.f, cz = v[8] * 100.f;
            float d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
            if (fabsf(d) < 1e-6f) continue;
            float w0 = ((bz - cz) * (job.lx - cx) + (cx - bx) * (job.lz - cz)) / d;
            float w1 = ((cz - az) * (job.lx - cx) + (ax - cx) * (job.lz - cz)) / d;
            if (w0 < 0 || w1 < 0 || 1 - w0 - w1 < 0) continue;
            float y = (w0 * v[1] + w1 * v[4] + (1 - w0 - w1) * v[7]) * 100.f, dy = job.ly - y;
            s0 += (dy > -150.f && dy < 400.f) ? 10 : 1;
        }
        if (bestF != 0 && (bestScore < 10 || s0 >= 10)) { bestF = 0; bestScore = s0; }
    }
    float sx = kFlip[bestF][0] * bestS / (float)bridge::kUnitsPerBlock;
    float sy = bestS / (float)bridge::kUnitsPerBlock;
    float sz = kFlip[bestF][1] * bestS / (float)bridge::kUnitsPerBlock;
    BridgeLog("collision: room %04X, %u triangles, mapping x%+d z%+d (score %d)", job.room, n, kFlip[bestF][0], kFlip[bestF][1], bestScore);
    {   // the room's walls/floors in RE4 units, for hiding Minecraft blocks behind them (blocks.cpp)
        std::vector<float> occ(n * 9);
        for (uint32_t t = 0; t < n * 3; t++) {
            occ[t * 3] = raw[t * 3] * bestS * kFlip[bestF][0]; occ[t * 3 + 1] = raw[t * 3 + 1] * bestS; occ[t * 3 + 2] = raw[t * 3 + 2] * bestS * kFlip[bestF][1];
        }
        EnterCriticalSection(&g_lock); g_occ.swap(occ); g_occRoom = job.room; g_occVersion++; LeaveCriticalSection(&g_lock);
    }

    // Convert and bucket into 8-block regions; voxelize a 1/8-block shell.
    struct Region { std::vector<ColTri> tris; std::map<int64_t, ColBlock> blocks; };
    std::map<int64_t, Region> regions;
    auto key3 = [](int x, int y, int z) { return ((int64_t)(x & 0x1FFFFF) << 42) | ((int64_t)(y & 0x1FFFFF) << 21) | (int64_t)(z & 0x1FFFFF); };
    auto unkey = [](int64_t k, int& x, int& y, int& z) {
        x = (int)((k >> 42) & 0x1FFFFF); y = (int)((k >> 21) & 0x1FFFFF); z = (int)(k & 0x1FFFFF);
        if (x & 0x100000) x -= 0x200000; if (y & 0x100000) y -= 0x200000; if (z & 0x100000) z -= 0x200000;
    };
    uint32_t nCeil = 0;
    for (uint32_t t = 0; t < n; t++) {
        const float* r = raw + t * 9;
        ColTri ct{};
        for (int k = 0; k < 3; k++) { ct.v[k * 3] = r[k * 3] * sx; ct.v[k * 3 + 1] = r[k * 3 + 1] * sy; ct.v[k * 3 + 2] = r[k * 3 + 2] * sz; }
        // mirroring an axis flips the winding; keep triangles facing the same way
        if (kFlip[bestF][0] * kFlip[bestF][1] < 0) for (int k = 0; k < 3; k++) std::swap(ct.v[3 + k], ct.v[6 + k]);
        float mn[3], mx[3];
        for (int a = 0; a < 3; a++) { mn[a] = fminf(ct.v[a], fminf(ct.v[3 + a], ct.v[6 + a])); mx[a] = fmaxf(ct.v[a], fmaxf(ct.v[3 + a], ct.v[6 + a])); }
        int r0[3], r1[3];
        for (int a = 0; a < 3; a++) { r0[a] = FloorDiv8((int)floorf(mn[a])); r1[a] = FloorDiv8((int)floorf(mx[a])); }
        // Downward-facing triangles (ceilings) stay out of Minecraft's exact triangle collision:
        // the Minecraft side takes any surface up to 2.5 blocks above the feet as ground when it places the
        // player (and catches jumps on "walkable" ones), which put the player on top of the house.
        // The voxel shell below still stops the head at the ceiling.
        float ux = ct.v[3] - ct.v[0], uy = ct.v[4] - ct.v[1], uz = ct.v[5] - ct.v[2];
        float wx = ct.v[6] - ct.v[0], wy = ct.v[7] - ct.v[1], wz = ct.v[8] - ct.v[2];
        float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
        float nl = sqrtf(nx * nx + ny * ny + nz * nz);
        bool ceiling = nl > 1e-9f && ny / nl < -0.3f;
        if (ceiling) nCeil++;
        else
            for (int x = r0[0]; x <= r1[0]; x++)
                for (int y = r0[1]; y <= r1[1]; y++)
                    for (int z = r0[2]; z <= r1[2]; z++) regions[key3(x, y, z)].tris.push_back(ct);
        // shell voxels
        float e1 = sqrtf(powf(ct.v[3] - ct.v[0], 2) + powf(ct.v[4] - ct.v[1], 2) + powf(ct.v[5] - ct.v[2], 2));
        float e2 = sqrtf(powf(ct.v[6] - ct.v[0], 2) + powf(ct.v[7] - ct.v[1], 2) + powf(ct.v[8] - ct.v[2], 2));
        float e3 = sqrtf(powf(ct.v[6] - ct.v[3], 2) + powf(ct.v[7] - ct.v[4], 2) + powf(ct.v[8] - ct.v[5], 2));
        int steps = (int)ceilf(fmaxf(e1, fmaxf(e2, e3)) * 16.f) + 1;
        if (steps > 4000) steps = 4000;
        for (int i = 0; i <= steps; i++)
            for (int j = 0; j <= steps - i; j++) {
                float u = (float)i / steps, w = (float)j / steps;
                float p[3];
                for (int a = 0; a < 3; a++) p[a] = ct.v[a] + (ct.v[3 + a] - ct.v[a]) * u + (ct.v[6 + a] - ct.v[a]) * w;
                int vx = (int)floorf(p[0] * 8), vy = (int)floorf(p[1] * 8), vz = (int)floorf(p[2] * 8);
                int bx = FloorDiv8(vx), by = FloorDiv8(vy), bz = FloorDiv8(vz);
                Region& rg = regions[key3(FloorDiv8(bx), FloorDiv8(by), FloorDiv8(bz))];
                ColBlock& cb = rg.blocks[key3(bx, by, bz)];
                cb.x = bx; cb.y = by; cb.z = bz;
                cb.bits[vy - by * 8] |= 1ull << ((vz - bz * 8) * 8 + (vx - bx * 8));
            }
    }

    // Minecraft holds its player until every region around them is "known", so also send the empty
    // regions: the room's whole box (one region of margin, two below) and the area around Leon.
    {
        int lo[3] = {INT32_MAX, INT32_MAX, INT32_MAX}, hi[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
        for (auto& kv : regions) {
            int rx, ry, rz; unkey(kv.first, rx, ry, rz);
            int r[3] = {rx, ry, rz};
            for (int a = 0; a < 3; a++) { if (r[a] < lo[a]) lo[a] = r[a]; if (r[a] > hi[a]) hi[a] = r[a]; }
        }
        int lr[3] = {FloorDiv8((int)floorf(job.lx / 1000.f)), FloorDiv8((int)floorf(job.ly / 1000.f)), FloorDiv8((int)floorf(job.lz / 1000.f))};
        for (int a = 0; a < 3; a++) { if (lr[a] - 2 < lo[a]) lo[a] = lr[a] - 2; if (lr[a] + 2 > hi[a]) hi[a] = lr[a] + 2; }
        lo[0]--; lo[1] -= 2; lo[2]--; hi[0]++; hi[1]++; hi[2]++;
        long volume = (long)(hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1);
        if (volume < 200000)
            for (int x = lo[0]; x <= hi[0]; x++)
                for (int y = lo[1]; y <= hi[1]; y++)
                    for (int z = lo[2]; z <= hi[2]; z++) regions[key3(x, y, z)];
        else
            BridgeLog("collision: room box too large (%ld regions); only sending regions with geometry", volume);
    }

    EnterCriticalSection(&g_lock);
    g_static.clear();
    for (auto& kv : regions) { StaticRegion& sr = g_static[kv.first]; sr.tris = kv.second.tris; for (auto& b : kv.second.blocks) sr.blocks[b.first] = b.second; }
    g_staticEpoch = job.epoch; g_doorRegions.clear();
    LeaveCriticalSection(&g_lock);
    size_t nt = 0, nb = 0;
    for (auto& kv : regions) {
        int rx, ry, rz; unkey(kv.first, rx, ry, rz);
        ColRegion hdr{rx * 8, ry * 8, rz * 8, rx * 8 + 7, ry * 8 + 7, rz * 8 + 7, job.epoch, (uint32_t)kv.second.tris.size()};
        if (!kv.second.tris.empty()) {
            Put(out, 3, &hdr, sizeof hdr, kv.second.tris.data(), kv.second.tris.size() * sizeof(ColTri));
            nt += kv.second.tris.size();
        }
        std::vector<ColBlock> blocks;
        for (auto& b : kv.second.blocks) blocks.push_back(b.second);
        hdr.count = (uint32_t)blocks.size();
        Put(out, 2, &hdr, sizeof hdr, blocks.data(), blocks.size() * sizeof(ColBlock));
        nb += blocks.size();
    }
    BridgeLog("collision: room %04X: %u ceiling triangles kept to the voxel shell only", job.room, nCeil);
    BridgeLog("collision: room %04X ready: %u regions, %u triangle refs, %u voxel blocks, %.1f MB",
              job.room, (unsigned)regions.size(), (unsigned)nt, (unsigned)nb, out.size() / 1048576.0);
    EnterCriticalSection(&g_lock);
    g_out.swap(out); g_outEpoch = job.epoch; g_outReady = true;
    LeaveCriticalSection(&g_lock);
}

static DWORD WINAPI Worker(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        Job job; bool have;
        EnterCriticalSection(&g_lock); job = g_job; have = g_jobPending; g_jobPending = false; LeaveCriticalSection(&g_lock);
        if (have) Build(job);
        std::vector<float> doors; uint32_t de = 0; bool dh;
        EnterCriticalSection(&g_lock); dh = g_doorPending; g_doorPending = false; if (dh) { doors = g_doorTris; de = g_doorEpoch; } LeaveCriticalSection(&g_lock);
        if (dh) BuildDoors(doors, de);
    }
}

bool Available() { return !g_rooms.empty(); }

void Request(uint16_t room, float lx, float ly, float lz, uint32_t epoch) {
    if (!g_lockInit) {
        InitializeCriticalSection(&g_lock); g_lockInit = true;
        g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }
    EnterCriticalSection(&g_lock);
    g_job = {room, lx, ly, lz, epoch}; g_jobPending = true; g_outReady = false;
    LeaveCriticalSection(&g_lock);
    SetEvent(g_wake);
}

// The doors' collision right now (RE4 units, vertical faces only). Only changes trigger an update.
void SetDoors(const std::vector<float>& tris, uint32_t epoch) {
    if (!g_lockInit) return;
    EnterCriticalSection(&g_lock);
    bool same = epoch == g_doorEpoch && tris.size() == g_doorTris.size();
    if (same) for (size_t i = 0; i < tris.size(); i++) if (fabsf(tris[i] - g_doorTris[i]) > 40.f) { same = false; break; }
    if (!same) { g_doorTris = tris; g_doorEpoch = epoch; g_doorPending = true; }
    LeaveCriticalSection(&g_lock);
    if (!same) SetEvent(g_wake);
}
bool TakeDoors(uint32_t epoch, std::vector<uint8_t>& msgs) {
    if (!g_lockInit) return false;
    EnterCriticalSection(&g_lock);
    bool ok = g_doorOutReady && g_doorOutEpoch == epoch;
    if (ok) { msgs.swap(g_doorOut); g_doorOut.clear(); g_doorOutReady = false; g_doorOutKeys.clear(); }
    LeaveCriticalSection(&g_lock);
    return ok;
}

// Hands over the finished messages for `epoch` (once). Returns false while still building.
bool Take(uint32_t epoch, std::vector<uint8_t>& msgs) {
    if (!g_lockInit) return false;
    EnterCriticalSection(&g_lock);
    bool ok = g_outReady && g_outEpoch == epoch;
    if (ok) { msgs.swap(g_out); g_out.clear(); g_outReady = false; }
    LeaveCriticalSection(&g_lock);
    return ok;
}

} // namespace collision
