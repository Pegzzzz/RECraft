// combat.cpp - RE4 enemies inside Minecraft (SkyCraft's actor proxies) and hits in both directions.
//
// * Every RE4 enemy near Leon is published in the actor table; SkyCraft's Minecraft mod mirrors each
//   as an invisible, hittable proxy. Minecraft's own combat (attack cooldown, crits, sweeping,
//   enchantments, bows, tridents...) then works on them.
// * Minecraft's hits come back as events: the damage is applied to the RE4 enemy, which is put into
//   its damage reaction (or its death routine) and pushed back by Minecraft's knockback.
// * RE4's hits on Leon are cancelled in RE4 and sent to Minecraft instead: Minecraft's health,
//   armour and shields decide.
// Runs on the game thread (called from the camera hook).

#include "link.h"
#include "memcheck.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <vector>

namespace render { extern uint8_t* g_subScreen; }
namespace collision { void SetDoors(const std::vector<float>& tris, uint32_t epoch); }
namespace bridge { uint32_t CollisionEpoch(); }
namespace combat {

uint8_t* g_cand[8]; int g_candN = 0;
uint8_t*** g_ppGlobal = nullptr;
uint8_t*** g_ppPlayer = nullptr;
static uint8_t* g_mgr = nullptr;
static bool g_mcDied = false;

namespace off {
    const int be_flag = 0x04, pos = 0x94, ang = 0xA0, guid = 0xF8, r_no = 0xFC, id = 0x100,
              hp = 0x324, hp_max = 0x326, dmg_flag = 0x328 + 4, dmg_from = 0x328 + 8;
    const int mgr_array = 0x4, mgr_count = 0x8, mgr_block = 0xC;
    const int pl_hp = 0x4FB4, pl_hp_max = 0x4FB6, gold = 0x4FA8;   // GLOBAL_WK::goldAmount_4FA8 (pesetas)
}

static inline bool Readable(const void* p, size_t n) { return mem::Readable(p, n); }   // whole range, cached (memcheck.h)
template<class T> static T Rd(const uint8_t* p, int o) { T v; memcpy(&v, p + o, sizeof v); return v; }
template<class T> static void Wr(uint8_t* p, int o, T v) { memcpy(p + o, &v, sizeof v); }

static bool IsEnemy(int id) {   // re4_tweaks' list
    if (id == 0x2A || id == 0x3B || id == 0x3D || id == 0x4E) return false;
    return id > 0x10 && id < 0x4F;
}

static bool MgrInfo(uint8_t* mgr, uint8_t*& arr, uint32_t& cnt, uint32_t& blk) {
    if (!Readable(mgr, 0x20)) return false;
    arr = Rd<uint8_t*>(mgr, off::mgr_array); cnt = Rd<uint32_t>(mgr, off::mgr_count); blk = Rd<uint32_t>(mgr, off::mgr_block);
    if (cnt == 0 || cnt > 2048 || blk < off::hp_max + 2 || blk > 0x10000) return false;
    return Readable(arr, (size_t)cnt * blk);
}

// Which candidate is cManager<cEm>? re4_tweaks uses the first match of the pattern, so that one is the
// default. A candidate holding live enemies (and Leon, who is in the same list) confirms it.
static void PickManager() {
    static ULONGLONG lastTry = 0, lastLog = 0;
    ULONGLONG now = GetTickCount64();
    if (now - lastTry < 500) return;
    lastTry = now;
    uint8_t* pl = g_ppPlayer ? (uint8_t*)*g_ppPlayer : nullptr;
    int best = -1, bestEnemies = 0; bool log = now - lastLog > 10000;
    for (int k = 0; k < g_candN; k++) {
        uint8_t* arr; uint32_t cnt, blk; int valid = 0, enemies = 0; bool hasLeon = false;
        bool ok = MgrInfo(g_cand[k], arr, cnt, blk);
        if (ok)
            for (uint32_t i = 0; i < cnt; i++) {
                uint8_t* e = arr + i * blk;
                if (e == pl) hasLeon = true;
                if (!(Rd<uint32_t>(e, off::be_flag) & 0x601)) continue;
                valid++;
                int16_t hp = Rd<int16_t>(e, off::hp), mx = Rd<int16_t>(e, off::hp_max);
                if (IsEnemy(Rd<uint8_t>(e, off::id)) && mx > 0 && mx <= 30000 && hp <= mx) enemies++;
            }
        if (log) BridgeLog("combat: candidate %d %p: %s count %u block 0x%X, %d live, %d enemies%s", k, g_cand[k],
                           ok ? "ok" : "unreadable", ok ? cnt : 0, ok ? blk : 0, valid, enemies, hasLeon ? ", holds Leon" : "");
        if (ok && (hasLeon || enemies > 0) && (best < 0 || (hasLeon ? 1000 : 0) + enemies > bestEnemies)) {
            best = k; bestEnemies = (hasLeon ? 1000 : 0) + enemies;
        }
    }
    if (log) lastLog = now;
    if (best < 0 && g_candN > 0) {
        uint8_t* arr; uint32_t cnt, blk;
        if (MgrInfo(g_cand[0], arr, cnt, blk)) best = 0;   // re4_tweaks' choice
    }
    if (best >= 0) {
        g_mgr = g_cand[best];
        BridgeLog("combat: enemy manager %p (candidate %d%s)", g_mgr, best, bestEnemies >= 1000 ? ", holds Leon" : "");
    }
}

static uint32_t FormId(uint32_t index, uint8_t* e) { return 0x10000000u | (index << 16) | (Rd<uint32_t>(e, off::guid) & 0xFFFF); }

static const char* EnemyName(int id) {   // re4_tweaks' names
    switch (id) {
    case 0x11: case 0x14: case 0x19: case 0x1A: return "Zealot";
    case 0x1B: case 0x1C: return "Garrador";
    case 0x1D: case 0x1E: case 0x1F: case 0x20: case 0x43: return "Soldier";
    case 0x21: return "Dog";       case 0x22: return "Colmillo"; case 0x23: return "Crow";
    case 0x24: return "Snake";     case 0x25: return "Parasite"; case 0x26: return "Cow";
    case 0x27: return "Bass";      case 0x28: return "Chicken";  case 0x29: return "Bat";
    case 0x2B: return "El Gigante"; case 0x2C: return "Verdugo"; case 0x2D: return "Novistador";
    case 0x2E: return "Spider";    case 0x2F: return "Del Lago"; case 0x31: case 0x3F: return "Saddler";
    case 0x32: return "U-3";       case 0x35: return "Mendez";   case 0x36: return "Regenerator";
    case 0x38: return "Salazar";   case 0x39: return "Krauser";  case 0x3A: return "Robot";
    case 0x3C: return "Knight";
    case 0x18: return "Merchant";
    default: return "Ganado";
    }
}
// Breakable scenery RE4 keeps in the same list (decompilation: embox / emwindow / embarrel / emBarred):
// they break from Leon's weapon hits, so Minecraft's hits go through the same hit registration.
// Shoot-down scenery (decompilation id + 0x10): emitem 0x5C (spinels and other items hanging from the ceiling or a
// wall, blue medallions), emtorch 0x57 (lamps, candles, braziers), emhit 0x5D (shootable targets: bells, switches,
// beetles), emBar 0x61 (wooden boards across a passage). Each takes a gun hit like Leon's.
// The traps (em2a, 0x2A - left out of the enemy list): a bear trap snaps shut from any weapon hit but the
// hand/flash/mine kinds (decompilation em2aDmCkTrap1: hp 0, Trap1Break; on the dog it frees it), and a
// tripwire bomb goes off (em2aDmCkTrap2).
static bool IsShootable(int id) { return id == 0x5C || id == 0x57 || id == 0x5D || id == 0x61 || id == 0x2A; }
static bool IsBreakable(int id) { return id == 0x53 || id == 0x56 || id == 0x58 || id == 0x5E || IsShootable(id); }
static const char* BreakableName(int id) {
    switch (id) {
    case 0x53: return "Box"; case 0x56: return "Window"; case 0x58: return "Barrel"; case 0x5E: return "Boarded window";
    case 0x5C: return "Item"; case 0x57: return "Lamp"; case 0x5D: return "Target"; case 0x61: return "Boards";
    case 0x2A: return "Trap";
    default: return "Object";
    }
}
// The stand-in must stick out of whatever solid shell Minecraft has around the object, or the
// crosshair stops on the shell ("BLOCK") instead: size from the object's own collision cylinder
// (cModel::atari_2B4 radius 0xC / height 0x14) plus a margin, never smaller than a generous default.
static void BreakableSize(const uint8_t* e, int id, float& w, float& h) {
    switch (id) {
    case 0x5C: w = 0.6f; h = 0.6f; return;   // a small item: a small target around it (it hangs, so its centre is the point)
    case 0x57: w = 0.7f; h = 0.8f; return;
    case 0x53: w = 1.4f; h = 1.3f; break; case 0x58: w = 1.3f; h = 1.4f; break;
    case 0x5D: case 0x61: w = 1.0f; h = 1.0f; break;
    case 0x2A: w = 0.9f; h = 0.5f; break;   // flat on the floor: low, so Minecraft hits it by looking down
    default: w = 1.4f; h = 1.8f; break;
    }
    float r = Rd<float>(e, 0x2B4 + 0xC), ht = Rd<float>(e, 0x2B4 + 0x14);
    if (r > 50.f && r < 3000.f && 2.f * r / 1000.f + 0.4f > w) w = 2.f * r / 1000.f + 0.4f;
    if (ht > 50.f && ht < 5000.f && ht / 1000.f + 0.3f > h) h = ht / 1000.f + 0.3f;
}
static bool Passive(int id) { return id == 0x21 || id == 0x26 || id == 0x27 || id == 0x28 || id == 0x18; }   // the dog (chapter 1's friend), cow, bass, chicken, the merchant

// ------------------------------------------------------------------ actor table (to Minecraft)
#pragma pack(push, 1)
struct ActorRecord { uint32_t formId, flags; float x, y, z, yaw, width, height, healthFrac; uint16_t level, pad; char name[24]; };
struct McEvent { uint32_t type, formId; float a, b, c, d; uint32_t flags, weapon; };
#pragma pack(pop)

// A Minecraft hit goes through RE4's own damage system, exactly like Leon's weapons do
// (decompilation: PlWepHitCheck2 -> cDmgInfo::set(0, 10, type, from, rad, part)): the enemy's own code
// then plays the flinch / knock-back / death motion, drops its item and dissolves the body. RE4 picks
// its own damage number, so we steer the result: before the hit the enemy gets plenty of spare health
// (non-lethal) or 1 hp (lethal); once RE4 has processed the hit, health is set to what Minecraft's
// damage leaves.
struct Pending { uint8_t* em; int16_t target; bool lethal; int frames; };
static Pending g_pend[512];
enum : uint8_t { kWepHandgun = 0x01, kWepKnife = 0x10, kWepKick = 0x22 };

static bool IsGanadoFamily(int id) { return (id >= 0x10 && id <= 0x20) || (id >= 0x42 && id <= 0x44); }

// Which of the enemy's hit boxes the Minecraft player's aim goes through - the way RE4 itself finds where
// a shot lands (decompilation em_sub.cpp emLineAtCk): each YARARE_INFO (list from cEm+0x344, next +0x30)
// is a capsule (or a cube, flags bit3) around ofs (+0x00) in the space of a parts (partsNo +0x26, 1-based,
// 0 = the model; world matrix cCoord::mat +0x0C), height +0x1C along y (x / z with flags 2 / 4), width +0x18.
// Only a box the aim line really passes through counts; otherwise it's a body hit (never the head).
static uint8_t* PartsPtr(uint8_t* e, int no) {
    uint8_t* b = Rd<uint8_t*>(e, 0xF4);   // cModel::pParts chain
    for (int i = 0; b && i < no; i++) { if (!Readable(b, 0xF8)) return nullptr; b = Rd<uint8_t*>(b, 0xF4); }
    return b && Readable(b, 0x40) ? b : nullptr;
}
static void MulPt(const uint8_t* m, const float v[3], float out[3]) {
    float M[3][4]; memcpy(M, m + 0x0C, sizeof M);
    for (int r = 0; r < 3; r++) out[r] = M[r][0] * v[0] + M[r][1] * v[1] + M[r][2] * v[2] + M[r][3];
}
// Where the body really is: the bounding box of its bones (cModel parts chain, translation of each world
// matrix), in RE4 units. Used to lay the Minecraft stand-in flat when an enemy is down on the floor.
static bool BodyBounds(uint8_t* e, float lo[3], float hi[3]) {
    int n = 0;
    uint8_t* b = Rd<uint8_t*>(e, 0xF4);
    for (int i = 0; b && i < 80 && Readable(b, 0xF8); i++, b = Rd<uint8_t*>(b, 0xF4)) {
        float M[3][4]; memcpy(M, b + 0x0C, sizeof M);
        float t[3] = {M[0][3], M[1][3], M[2][3]};
        if (!(t[0] == t[0]) || fabsf(t[0]) > 1e7f || fabsf(t[1]) > 1e7f || fabsf(t[2]) > 1e7f) continue;
        for (int k = 0; k < 3; k++) { if (!n || t[k] < lo[k]) lo[k] = t[k]; if (!n || t[k] > hi[k]) hi[k] = t[k]; }
        n++;
    }
    return n >= 4;
}
// RE4's doors are enemies too (cEmDoor, id 0x41; decompilation emdoor.cpp). Minecraft's player would walk
// straight through them (they aren't in the room's collision), so each door's panel goes into Minecraft's
// collision where the door is right now - shut, it blocks; swung open, it lies along the wall. Padlocks on
// a door (pLockL / pLockR, hit boxes 12 / 11) get a small stand-in so the sword can break them.
// The PC build numbers the scenery "enemies" 0x10 higher than the GameCube decompilation (box 0x43 -> 0x53,
// window 0x46 -> 0x56, barrel 0x48 -> 0x58, boarded window 0x4E -> 0x5E all confirmed in game), so cEmDoor is
// 0x51 here, not the decompilation's 0x41 (0.26-0.32 looked for 0x41: no door, and so no padlock, was ever found).
static const int kDoorId = 0x51;
// The door's own work (EmDoorWork) starts right after cEm. The decompilation is the GameCube build (cEm is
// 0x3E0 bytes there); the PC build's cEm is 0x408 (re4_tweaks' SDK: assert_size(cEm, 0x408)), so every
// door field sits 0x28 further on: Height +0x20, Width +0x24, hit[16] +0x28, pLockL +0x428, pLockR +0x42C.
// (0.26-0.30 read the GameCube offsets: no door was ever recognised.) Checked on the first door seen.
static int g_doorWk = 0x408; static bool g_doorWkSure = false;   // once a door confirms a layout, it stays
static bool DoorDims(uint8_t* e, float& H, float& W) {
    auto ok = [](float h, float w) { return h > 1000.f && h < 6000.f && w > 150.f && w < 2500.f; };
    static int said = 0;
    H = Rd<float>(e, g_doorWk + 0x20); W = Rd<float>(e, g_doorWk + 0x24);
    if (ok(H, W)) {
        if (!g_doorWkSure) { g_doorWkSure = true; BridgeLog("doors: door work at +0x%X (%.0f x %.0f mm)", g_doorWk, 2 * W, H); }
        return true;
    }
    if (!g_doorWkSure)   // nothing confirmed yet: try the other layouts
        for (int base : {0x408, 0x3E0, 0x40C, 0x404}) {
            float h = Rd<float>(e, base + 0x20), w = Rd<float>(e, base + 0x24);
            if (ok(h, w)) {
                g_doorWk = base; g_doorWkSure = true; H = h; W = w;
                BridgeLog("doors: door work at +0x%X (%.0f x %.0f mm)", base, 2 * w, h);
                return true;
            }
        }
    if (said++ < 4) BridgeLog("doors: a door (id 0x51) with no recognisable size (+0x428 %.0f/%.0f, +0x400 %.0f/%.0f) - left out",
                              Rd<float>(e, 0x428), Rd<float>(e, 0x42C), Rd<float>(e, 0x400), Rd<float>(e, 0x404));
    return false;
}
static void DoorTick(uint8_t* e, uint32_t i, const float plp[3], uint8_t* view, uint32_t& n, std::vector<float>& tris) {
    float dp[3]; memcpy(dp, e + off::pos, 12);
    float dx = dp[0] - plp[0], dz = dp[2] - plp[2];
    if (dx * dx + dz * dz > 40000.f * 40000.f) return;
    float H = 0, W = 0;
    bool dims = DoorDims(e, H, W);
    if (cfg::doorCollision && dims && Rd<int16_t>(e, off::hp) > 0) {
        float c[8][3];
        for (int k = 0; k < 8; k++) {
            float l[3] = {(k & 1) ? 0.f : -2.f * W, (k & 2) ? H : 0.f, (k & 4) ? 50.f : -50.f};
            MulPt(e, l, c[k]);
        }
        static const int f[8][3] = {{0,1,3},{0,3,2},{4,6,7},{4,7,5},{0,2,6},{0,6,4},{1,5,7},{1,7,3}};   // the four upright faces
        for (auto& t : f) for (int k = 0; k < 3; k++) tris.insert(tris.end(), c[t[k]], c[t[k]] + 3);
        static int said = 0;
        if (said++ < 4) BridgeLog("doors: door #%u %.0fx%.0f mm, hinge (%.0f %.0f %.0f) -> far edge (%.0f %.0f %.0f)", i, 2 * W, H, c[1][0], c[1][1], c[1][2], c[0][0], c[0][1], c[0][2]);
    }
    if (!dims) return;
    for (int side = 0; side < 2 && n < 256; side++) {   // 0: left lock (pLockL, hit[12]), 1: right lock (pLockR, hit[11])
        uint8_t* lk = Rd<uint8_t*>(e, g_doorWk + (side ? 0x42C : 0x428));
        if (!lk || !Readable(lk, 0xA0)) continue;
        float lp[3]; memcpy(lp, lk + 0x94, 12);
        float ex = lp[0] - dp[0], ez = lp[2] - dp[2];
        if (!(ex * ex + ez * ez < 4000.f * 4000.f)) continue;
        ActorRecord r{};
        r.formId = 0x20000000u | (i << 16) | (uint32_t)side;
        r.flags = 0;
        r.x = lp[0] / 1000.f; r.y = (lp[1] - 250.f) / 1000.f; r.z = lp[2] / 1000.f;
        r.width = 0.4f; r.height = 0.5f; r.healthFrac = 1.f; r.level = 1;
        snprintf(r.name, sizeof r.name, "Lock");
        memcpy(view + 0x12000 + 0x40 + n * 64, &r, 64);
        n++;
    }
}

static uint8_t* AimedPart(uint8_t* e, float* outRad) {
    uint8_t* first = e + 0x344;
    auto& S = bridge::S();
    if (!S.haveEye) return first;
    float yaw, pitch; bridge::GetLook(yaw, pitch);
    float yr = yaw * 0.0174532925f, pr = pitch * 0.0174532925f;
    float dir[3] = {-sinf(yr) * cosf(pr), -sinf(pr), cosf(yr) * cosf(pr)};
    float eye[3] = {(float)(S.eyeX * bridge::kUnitsPerBlock), (float)(S.eyeY * bridge::kUnitsPerBlock), (float)(S.eyeZ * bridge::kUnitsPerBlock)};
    uint8_t* best = nullptr; float bestMiss = 1e30f, bestT = 0;
    uint8_t* body = nullptr; float bodyY = 1e30f;   // fallback: the box nearest the enemy's chest
    float ep[3]; memcpy(ep, e + off::pos, 12);
    int n = 0;
    for (uint8_t* p = first; p && n < 40 && Readable(p, 0x34); p = Rd<uint8_t*>(p, 0x30), n++) {
        uint16_t flags = Rd<uint16_t>(p, 0x24);
        if (!(flags & 1)) continue;
        int16_t pno = Rd<int16_t>(p, 0x26);
        uint8_t* m = pno ? PartsPtr(e, pno - 1) : e;
        if (!m) continue;
        float ofs[3]; memcpy(ofs, p, 12);
        float w = Rd<float>(p, 0x18), h = Rd<float>(p, 0x1C);
        float a[3], b[3], top[3] = {ofs[0], ofs[1], ofs[2]};
        if (flags & 8) { float d = Rd<float>(p, 0x20); w = (w > h ? (w > d ? w : d) : (h > d ? h : d)) * 0.5f; }
        else if (flags & 2) top[0] += h; else if (flags & 4) top[2] += h; else top[1] += h;
        MulPt(m, ofs, a); MulPt(m, (flags & 8) ? ofs : top, b);
        float r = w;
        // closest approach between the aim ray and the segment a-b
        float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w0[3] = {eye[0] - a[0], eye[1] - a[1], eye[2] - a[2]};
        float A = 1.f, B = dir[0] * u[0] + dir[1] * u[1] + dir[2] * u[2], C = u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
        float D = dir[0] * w0[0] + dir[1] * w0[1] + dir[2] * w0[2], E = u[0] * w0[0] + u[1] * w0[1] + u[2] * w0[2];
        float den = A * C - B * B, sc, tc;
        if (den < 1e-6f || C < 1e-6f) { sc = -D; tc = C > 1e-6f ? E / C : 0.f; }
        else { sc = (B * E - C * D) / den; tc = (A * E - B * D) / den; }
        if (tc < 0) tc = 0; if (tc > 1) tc = 1;
        if (sc < 0) sc = 0;
        float q[3], c[3];
        for (int k = 0; k < 3; k++) { q[k] = eye[k] + dir[k] * sc; c[k] = a[k] + u[k] * tc; }
        float dx = q[0] - c[0], dy = q[1] - c[1], dz = q[2] - c[2];
        float miss = sqrtf(dx * dx + dy * dy + dz * dz) - r;
        if (miss < bestMiss) { bestMiss = miss; best = p; bestT = sc; }
        float cy = (a[1] + b[1]) * 0.5f - (ep[1] + 1100.f);   // chest height
        if (fabsf(cy) < bodyY) { bodyY = fabsf(cy); body = p; }
    }
    uint8_t* pick = (best && bestMiss < 60.f) ? best : (body ? body : first);   // within 6 cm of a box: that box
    if (outRad) *outRad = pick == best ? bestT * bestT : -1.f;
    static int logs = 0;
    if (logs < 12) { logs++; BridgeLog("combat: aimed hit box %p (%s, %d boxes, miss %.0f mm)", pick, pick == best ? "under the crosshair" : "body - nothing aimed", n, bestMiss); }
    return pick;
}

static void RegisterHit(uint8_t* e, int type, const float from[3], float distSq, uint8_t* aimed = nullptr) {
    uint8_t* part = aimed ? aimed : e + 0x344;        // the hit box (cEm::yarare_344 = the model's first one)
    float rad = distSq; memcpy(part + 0x28, &rad, 4); // YARARE_INFO::len_28 (near/far for the damage table)
    uint8_t* d = e + 0x328;                           // cEm::m_DmgInfo_328
    float t0 = 10.0f; memcpy(d + 0x0, &t0, 4);         // UHD's extra float (60 fps timer or distance)
    d[0x4] = 1;                                       // m_Flag: hit registered
    d[0x5] = 10;                                      // m_Timer: expires after 10 frames if unhandled
    d[0x6] = (uint8_t)type;                           // m_Wep: hit kind
    memcpy(d + 0x8, from, 12);                        // m_PosFrom
    memcpy(d + 0x14, &rad, 4);                        // m_Dist
    memcpy(d + 0x18, &part, 4);                       // m_pDamageYarare
}

// ------------------------------------------------------------------ RECraft's Minecraft side (RECraft-link mod)
// Lives in a free block of SkyCraft's link memory (0x1B200): the player's health (every tick), the
// health RE4 asks for, and the solid blocks around the player.
namespace mcx {
    const uint32_t kBase = 0x1B200, kMagic = 0x58433452;
    const uint32_t oMagic = 0x00, oBeat = 0x04, oHealth = 0x08, oMax = 0x0C, oFlags = 0x10, oDeaths = 0x14;
    const uint32_t oReqSeq = 0x20, oReqHealth = 0x24, oBlkSeq = 0x40, oBlkX = 0x44, oBlkCount = 0x50, oBlocks = 0x60;
    // RE4's merchant decides the Minecraft gear: what's in Leon's attache case (weapons, their tune-ups,
    // the case size) is published here and RECraft's Minecraft mod hands out the matching items.
    const uint32_t oGearSeq = 0x28, oTiers = 0x2C, oSwordE = 0x30, oBowE = 0x34, oArmorMul = 0x38, oArmourE = 0x3C,
                   oArrows = 0x54, oPlanks = 0x58, oAxeE = 0x5C, oSteak = 0x18;
    // the last enemy Minecraft hit, for the hit marker / health bar on Minecraft's HUD (end of the block)
    const uint32_t oHit = 0xDE0;   // seq, hp, max hp, flags (1 flinched, 2 killed), name[16]
    // grenades Leon picks up become Minecraft throwables (running totals): TNT, ender pearls, fire charges
    const uint32_t oTnt = 0xDC0, oPearl = 0xDC4, oFireCharge = 0xDC8, oAxeCooldown = 0xDCC;
    const uint32_t kMaxBlocks = (oTnt - oBlocks) / 6;
}
static volatile bool g_companionOk = false;
static uint8_t* volatile g_view = nullptr;
template<class T> static T Mx(uint32_t o) { return *(volatile T*)(g_view + mcx::kBase + o); }
template<class T> static void MxW(uint32_t o, T v) { *(volatile T*)(g_view + mcx::kBase + o) = v; }


static bool Companion(uint8_t* view) {
    static uint32_t lastBeat = 0; static ULONGLONG lastChange = 0;
    uint32_t magic = *(volatile uint32_t*)(view + mcx::kBase + mcx::oMagic), beat = *(volatile uint32_t*)(view + mcx::kBase + mcx::oBeat);
    if (magic != mcx::kMagic) return false;
    if (beat != lastBeat) { lastBeat = beat; lastChange = GetTickCount64(); }
    static bool said = false;
    bool alive = GetTickCount64() - lastChange < 2000;
    if (alive && !said) { said = true; BridgeLog("combat: RECraft's Minecraft mod is running - shared health both ways, blocks stop enemies"); }
    return alive;
}
static void RequestMcHealth(uint8_t* view, float h) {
    *(volatile float*)(view + mcx::kBase + mcx::oReqHealth) = h;
    MemoryBarrier();
    *(volatile uint32_t*)(view + mcx::kBase + mcx::oReqSeq) += 1;
}

// Minecraft blocks keep RE4's enemies out: an enemy whose body would overlap a solid block is put
// back where it stood last frame (so it stops at your walls like at RE4's own).
static std::vector<int64_t> g_blocks;   // sorted keys
static int64_t BKey(int x, int y, int z) { return ((int64_t)(x & 0x1FFFFF) << 42) | ((int64_t)(y & 0x1FFFFF) << 21) | (int64_t)(z & 0x1FFFFF); }
static void ReadBlocks(uint8_t* view) {
    static uint32_t lastSeq = 0;
    uint8_t* b = view + mcx::kBase;
    uint32_t seq = *(volatile uint32_t*)(b + mcx::oBlkSeq);
    if (seq == lastSeq || (seq & 1)) return;
    MemoryBarrier();   // the list after the sequence number (seqlock)
    int32_t c[3]; memcpy(c, b + mcx::oBlkX, 12);
    uint32_t n = *(volatile uint32_t*)(b + mcx::oBlkCount);
    if (n > mcx::kMaxBlocks) n = mcx::kMaxBlocks;
    std::vector<int64_t> k; k.reserve(n);
    for (uint32_t i = 0; i < n; i++) { int16_t d[3]; memcpy(d, b + mcx::oBlocks + i * 6, 6); k.push_back(BKey(c[0] + d[0], c[1] + d[1], c[2] + d[2])); }
    MemoryBarrier();
    if (*(volatile uint32_t*)(b + mcx::oBlkSeq) != seq) return;
    lastSeq = seq;
    std::sort(k.begin(), k.end());
    static size_t lastN = (size_t)-1;
    if (k.size() != lastN) { lastN = k.size(); BridgeLog("combat: %u Minecraft blocks near the player now stop enemies", (unsigned)k.size()); }
    g_blocks.swap(k);
}
static bool InBlocks(const float p[3]) {   // enemy body: 0.35-block radius, 1.7 blocks tall
    if (g_blocks.empty()) return false;
    float x = p[0] / 1000.f, y = p[1] / 1000.f, z = p[2] / 1000.f;
    for (int bx = (int)floorf(x - 0.35f); bx <= (int)floorf(x + 0.35f); bx++)
        for (int by = (int)floorf(y + 0.05f); by <= (int)floorf(y + 1.7f); by++)
            for (int bz = (int)floorf(z - 0.35f); bz <= (int)floorf(z + 0.35f); bz++)
                if (std::binary_search(g_blocks.begin(), g_blocks.end(), BKey(bx, by, bz))) return true;
    return false;
}
static float g_lastOk[512][3]; static uint8_t* g_lastOkEm[512];

// Enemies as rough boxes for blocks.cpp's depth pass, so Minecraft blocks behind an enemy are hidden by it.
static std::vector<float> g_enemyBoxes;   // x, y, z of each live enemy's feet (RE4 units)
void EnemyPositions(std::vector<float>& out) { out = g_enemyBoxes; }


// ------------------------------------------------------------------ RE4's merchant -> Minecraft gear
// Leon's attache case (cItemMgr, decompilation item.cpp: 0x180 ItemWork slots of 0xE bytes: id, count,
// flags (bit0 in use), owner (0 Leon, 1 Ashley), tune-up levels in four nibbles) decides the Minecraft gear:
//   handguns and magnums -> the sword tier, shotguns (and the Chicago Typewriter) -> the axe tier,
//   rifles -> the bow, the attache case size -> armour, the TMP -> Protection;
//   tune-ups -> enchantments. Ammo Leon picks up becomes arrows; crates and barrels you break give planks.
uint8_t* g_itemMgr = nullptr;
void (__fastcall* g_itemErase)(void* mgr, void* edx, void* item) = nullptr;

struct GearMap { uint16_t id; uint8_t fam, tier; };   // fam 0 sword, 1 axe, 2 bow, 3 armour enchantment
static const GearMap kGear[] = {
    {35, 0, 1} /*Handgun*/, {33, 0, 2}, {64, 0, 2} /*Punisher*/, {37, 0, 3}, {38, 0, 3} /*Red9*/, {39, 0, 4} /*Blacktail*/,
    {3, 0, 5} /*Matilda*/, {41, 0, 5} /*Broken Butterfly*/, {42, 0, 5} /*Killer7*/, {55, 0, 5} /*Handcannon*/,
    {44, 1, 2} /*Shotgun*/, {148, 1, 3} /*Riot Gun*/, {45, 1, 4} /*Striker*/, {52, 1, 5} /*Chicago Typewriter*/,
    {46, 2, 1}, {107, 2, 1} /*Rifle*/, {47, 2, 2}, {108, 2, 2}, {81, 2, 2} /*Rifle (semi-auto)*/,
    {48, 3, 1}, {50, 3, 1}, {62, 3, 1} /*TMP*/,
};
static float AmmoArrows(uint16_t id) {   // Minecraft arrows per round
    switch (id) {
    case 4: return 1.f;            // handgun ammo
    case 24: case 7: case 160: return 2.f;   // shotgun shells, rifle ammo
    case 0: case 26: return 3.f;   // magnum, handcannon ammo
    case 32: return 0.4f;          // TMP ammo
    case 106: return 0.3f;         // Chicago Typewriter ammo
    case 70: return 2.f;           // mine darts
    case 114: case 17: return 1.f; // arrows, bolts
    default: return 0.f;
    }
}

// Herbs become food: one Minecraft steak per herb (mixes count each herb in them).
static int HerbSteaks(uint16_t id) {
    switch (id) {
    case 6: case 25: case 28: return 1;                     // green, red, yellow
    case 18: case 20: case 22: case 168: return 2;          // G+G, G+R, G+Y, R+Y
    case 19: case 21: return 3;                             // G+G+G, G+R+Y
    default: return 0;
    }
}

static uint8_t* g_crate[8]; static ULONGLONG g_crateT[8];
static void PublishHit(const char* name, int hp, int mx, uint32_t flags) {
    if (!g_view) return;
    uint8_t* b = g_view + mcx::kBase + mcx::oHit;
    *(volatile int32_t*)(b + 4) = hp < 0 ? 0 : hp; *(volatile int32_t*)(b + 8) = mx; *(volatile uint32_t*)(b + 12) = flags;
    char n[16] = {}; strncpy(n, name, 15); memcpy(b + 16, n, 16);
    MemoryBarrier();
    *(volatile uint32_t*)b += 1;
}

static void NoteCrateHit(uint8_t* e) {
    int k = 0;
    for (int i = 0; i < 8; i++) { if (g_crate[i] == e) return; if (!g_crate[i] || g_crateT[i] < g_crateT[k]) k = i; }
    g_crate[k] = e; g_crateT[k] = GetTickCount64();
}

static void GearTick(bool puppet) {
    static int frame = 0;
    if (++frame % 15) return;   // ~4 times a second
    // crates and barrels Minecraft broke -> planks
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < 8; i++) {
        uint8_t* e = g_crate[i];
        if (!e) continue;
        if (now - g_crateT[i] > 5000 || !Readable(e, 0x330)) { g_crate[i] = nullptr; continue; }
        uint32_t bf = Rd<uint32_t>(e, off::be_flag);
        if (Rd<int16_t>(e, off::hp) <= 0 || Rd<uint8_t>(e, off::r_no) == 3 || (bf & 0x200) || !(bf & 0x601)) {
            g_crate[i] = nullptr;
            MxW<uint32_t>(mcx::oPlanks, Mx<uint32_t>(mcx::oPlanks) + 4);
            BridgeLog("gear: a crate broke -> 4 planks in Minecraft");
        }
    }
    MxW<uint32_t>(mcx::oAxeCooldown, (uint32_t)cfg::axeCooldownSec);   // the Minecraft side enforces it
    uint8_t* m = g_itemMgr;
    if (!Readable(m, 0x30)) return;
    uint8_t* items = Rd<uint8_t*>(m, 0x14);
    int n = Rd<int32_t>(m, 0x1C);
    if (n <= 0 || n > 0x400 || !Readable(items, (size_t)n * 0xE)) return;
    int best[4] = {0, 0, 0, 0}, caseTier = -1; uint8_t lv[4][4] = {};
    bool any = false;
    uint8_t* ammo[24]; int na = 0;
    uint8_t* herb[24]; int nherb = 0;
    for (int i = 0; i < n; i++) {
        uint8_t* it = items + i * 0xE;
        if (!(it[4] & 1) || it[5] != 0) continue;   // in use, Leon's
        any = true;
        uint16_t id = Rd<uint16_t>(it, 0), w = Rd<uint16_t>(it, 6);
        if (id >= 124 && id <= 127) { caseTier = (std::max)(caseTier, id - 124); continue; }
        if (AmmoArrows(id) > 0.f) { if (na < 24) ammo[na++] = it; continue; }
        if (HerbSteaks(id) > 0) { if (nherb < 24) herb[nherb++] = it; continue; }
        if (id == 1 || id == 2 || id == 14) { if (nherb < 24) herb[nherb++] = it; continue; }   // grenades: same conversion pass
        for (const GearMap& g : kGear) {
            if (g.id != id) continue;
            uint8_t l[4] = {(uint8_t)(w & 0xF), (uint8_t)((w >> 4) & 0xF), (uint8_t)((w >> 8) & 0xF), (uint8_t)((w >> 12) & 0xF)};
            if (g.tier > best[g.fam]) { best[g.fam] = g.tier; memcpy(lv[g.fam], l, 4); }
            else if (g.tier == best[g.fam]) for (int k = 0; k < 4; k++) lv[g.fam][k] = (std::max)(lv[g.fam][k], l[k]);
        }
    }
    if (!any) return;
    auto mn = [](int a, int b) { return (uint8_t)(a < b ? a : b); };
    static const uint8_t kCaseArmour[4] = {0, 1, 3, 4};   // case S none, M leather, L iron, XL diamond
    uint8_t spec[20] = {
        (uint8_t)(best[0] ? best[0] : 1), (uint8_t)(best[1] ? best[1] : 1), 1, caseTier >= 0 ? kCaseArmour[caseTier] : (uint8_t)0,   // a wooden axe at least (the heavy weapon)
        // sword: firepower -> Sharpness, firing speed -> Sweeping Edge, reload -> Fire Aspect, capacity -> Knockback
        mn(lv[0][0], 5), mn(lv[0][1], 3), mn(lv[0][2], 2), mn(lv[0][3], 2),
        // bow: the handgun's tune-ups enchant it too (firepower -> Power, firing speed -> Punch, reload -> Flame,
        // capacity -> Infinity); a rifle adds +1 Power (semi-auto +2) and its own tune-ups, whichever is higher
        mn((std::max)((int)lv[0][0], best[2] ? lv[2][0] + best[2] : 0), 5), mn((std::max)(lv[0][1], lv[2][1]), 2),
        (uint8_t)((std::max)(lv[0][2], lv[2][2]) >= 2 ? 1 : 0), (uint8_t)((std::max)(lv[0][3], lv[2][3]) >= 3 ? 1 : 0),
        // armour: TMP -> Protection (I, firepower adds), valid
        best[3] ? mn(lv[3][0] + 1, 4) : (uint8_t)0, 1, 0, 0,
        // axe: same as the sword
        mn(lv[1][0], 5), mn(lv[1][1], 3), mn(lv[1][2], 2), mn(lv[1][3], 2),
    };
    // where a change came from, for Minecraft's message: the merchant (his shop screen, SS_OPEN_SHOP, was
    // open just now) or a pick-up (STA_ITEM_GET, or the full-case pick-up screen SS_OPEN_ITEM)
    static ULONGLONG shopAt = 0, pickupAt = 0;
    {
        uint8_t* w = render::g_subScreen; uint32_t open = 0;
        if (w && Readable(w + 0x2C, 4)) open = Rd<uint32_t>(w, 0x2C);
        uint8_t* gl = g_ppGlobal ? (uint8_t*)*g_ppGlobal : nullptr;
        bool itemGet = gl && Readable(gl + 0x5020, 4) && (Rd<uint32_t>(gl, 0x5020) & 0x2);
        if (open & 0x10) shopAt = now;
        if (itemGet || (open & 0x80)) pickupAt = now;
    }
    static uint8_t last[20]; static bool have = false;
    if (!have || memcmp(last, spec, 20) != 0) {
        memcpy(last, spec, 20);   // (compared without the source byte)
        spec[14] = (have && now - shopAt < 4000) ? 1 : (have && now - pickupAt < 6000) ? 2 : 0;   // 1 merchant, 2 found
        memcpy(g_view + mcx::kBase + mcx::oTiers, spec, 4);
        memcpy(g_view + mcx::kBase + mcx::oSwordE, spec + 4, 4);
        memcpy(g_view + mcx::kBase + mcx::oBowE, spec + 8, 4);
        memcpy(g_view + mcx::kBase + mcx::oArmourE, spec + 12, 4);
        memcpy(g_view + mcx::kBase + mcx::oAxeE, spec + 16, 4);
        MemoryBarrier();
        MxW<uint32_t>(mcx::oGearSeq, Mx<uint32_t>(mcx::oGearSeq) + 1);
        BridgeLog("gear: case -> sword %d (sharp %d sweep %d fire %d kb %d), axe %d (sharp %d), bow (power %d punch %d flame %d inf %d), armour %d (prot %d)%s",
                  spec[0], spec[4], spec[5], spec[6], spec[7], spec[1], spec[16], spec[8], spec[9], spec[10], spec[11], spec[3], spec[12],
                  spec[14] == 1 ? " - from the merchant" : spec[14] == 2 ? " - found" : "");
        have = true;
    }
    // herbs Leon picked up -> steak (RE4's herbs don't heal any more: food does, through Minecraft)
    if (nherb && puppet && g_itemErase && m[0x13] == 0) {
        int steaks = 0, tnt = 0, pearls = 0, fire = 0;
        for (int i = 0; i < nherb; i++) {
            uint16_t id = Rd<uint16_t>(herb[i], 0), num = Rd<uint16_t>(herb[i], 2);
            int k = num ? num : 1;
            if (id == 1) tnt += k; else if (id == 14) pearls += k; else if (id == 2) fire += k;
            else steaks += HerbSteaks(id) * k;
            g_itemErase(m, nullptr, herb[i]);
        }
        if (steaks) MxW<uint32_t>(mcx::oSteak, Mx<uint32_t>(mcx::oSteak) + steaks);
        if (tnt) MxW<uint32_t>(mcx::oTnt, Mx<uint32_t>(mcx::oTnt) + tnt);
        if (pearls) MxW<uint32_t>(mcx::oPearl, Mx<uint32_t>(mcx::oPearl) + pearls);
        if (fire) MxW<uint32_t>(mcx::oFireCharge, Mx<uint32_t>(mcx::oFireCharge) + fire);
        BridgeLog("gear: pickups -> %d steak, %d TNT, %d ender pearls, %d fire charges in Minecraft", steaks, tnt, pearls, fire);
    }
    // ammo Leon picked up (or bought) -> arrows; RE4 can't fire it anyway while Minecraft has Leon
    if (na && puppet && g_itemErase && m[0x13] == 0) {
        float arrows = 0;
        for (int i = 0; i < na; i++) {
            uint16_t id = Rd<uint16_t>(ammo[i], 0), num = Rd<uint16_t>(ammo[i], 2);
            arrows += num * AmmoArrows(id) * cfg::arrowsPerAmmo;
            g_itemErase(m, nullptr, ammo[i]);
        }
        int a = (int)ceilf(arrows);
        if (a > 0) {
            MxW<uint32_t>(mcx::oArrows, Mx<uint32_t>(mcx::oArrows) + a);
            BridgeLog("gear: %d ammo pickup(s) -> %d arrows in Minecraft", na, a);
        }
    }
}

void Tick(uint8_t* view, bool puppet) {
    if (!view) return;
    mem::Reset();
    if (!g_mgr) PickManager();
    else { uint8_t* a; uint32_t c, b; if (!MgrInfo(g_mgr, a, c, b)) { static ULONGLONG t = 0; if (GetTickCount64() - t > 5000) { t = GetTickCount64(); PickManager(); } } }
    uint8_t* arr = nullptr; uint32_t cnt = 0, blk = 0;
    bool haveEnemies = g_mgr && MgrInfo(g_mgr, arr, cnt, blk);
    uint8_t* pl = g_ppPlayer ? (uint8_t*)*g_ppPlayer : nullptr;
    uint8_t* gl = g_ppGlobal ? (uint8_t*)*g_ppGlobal : nullptr;
    if (!Readable(pl, 0x110) || !Readable(gl, 0x4FC0)) return;
    float plp[3]; memcpy(plp, pl + off::pos, 12);
    // Difficulty: RE4's adaptive rank (GLOBAL_WK::dynamicDifficultyLevel_4F98, 1..10) never drops below
    // MinEnemyRank - above 6 Ganados rush (dash) twice as often and wait less between attacks.
    if (cfg::minEnemyRank > 0 && Readable(gl, 0x4FA0)) {
        uint8_t lv = gl[0x4F98];
        if (lv >= 1 && lv <= 10 && lv < cfg::minEnemyRank) {
            int32_t pts = cfg::minEnemyRank * 1000 + 500;
            Wr<int32_t>(gl, 0x4F94, pts); gl[0x4F98] = (uint8_t)cfg::minEnemyRank;
            static int said = 0; if (said++ < 5) BridgeLog("difficulty: RE4's enemy rank %d raised to %d", lv, cfg::minEnemyRank);
        }
    }

    // 1) publish enemies (every 3rd frame, ~20 Hz)
    static int frame = 0;
    if (haveEnemies && (++frame % 3) == 0) {
        volatile uint32_t* seq = (volatile uint32_t*)(view + 0x12000);
        uint32_t s = *seq;
        *seq = s | 1; MemoryBarrier();
        uint32_t n = 0;
        static std::vector<float> doorTris; doorTris.clear();
        for (uint32_t i = 0; i < cnt && n < 256; i++) {
            uint8_t* e = arr + i * blk;
            if (!(Rd<uint32_t>(e, off::be_flag) & 0x601)) continue;
            int id = Rd<uint8_t>(e, off::id);
            if (id == kDoorId) { if (blk >= (uint32_t)g_doorWk + 0x430) DoorTick(e, i, plp, view, n, doorTris); continue; }
            bool breakable = IsBreakable(id);
            if (!IsEnemy(id) && !breakable) continue;
            float p[3], a[3]; memcpy(p, e + off::pos, 12); memcpy(a, e + off::ang, 12);
            float dx = p[0] - plp[0], dz = p[2] - plp[2];
            if (dx * dx + dz * dz > 40000.f * 40000.f) continue;   // within ~40 blocks
            int16_t hp = Rd<int16_t>(e, off::hp), mx = Rd<int16_t>(e, off::hp_max);
            if (i < 512 && g_pend[i].frames > 0 && g_pend[i].em == e) hp = g_pend[i].target;
            ActorRecord r{};
            r.formId = FormId(i, e);
            bool dead = hp <= 0 || Rd<uint8_t>(e, off::r_no) == 3 || (Rd<uint32_t>(e, off::be_flag) & 0x200);
            r.flags = (Passive(id) ? 0u : 1u | 8u) | (dead ? 2u : 0u);    // hostile + in combat, dead
            r.x = p[0] / 1000.f; r.y = p[1] / 1000.f; r.z = p[2] / 1000.f;
            r.yaw = -a[1] * 57.2957795f;
            r.width = 0.7f; r.height = 1.8f;
            float blo[3], bhi[3];
            if (!breakable && BodyBounds(e, blo, bhi)) {
                float ex = bhi[0] - blo[0], ey = bhi[1] - blo[1], ez = bhi[2] - blo[2];
                float horiz = ex > ez ? ex : ez;
                // down on the floor (knocked down, crawling, dying): a low, wide box where the body lies,
                // so Minecraft can hit it by looking down at it
                if (ey < 900.f && horiz > 800.f && fabsf(blo[1] - p[1]) < 1200.f) {
                    r.x = (blo[0] + bhi[0]) * 0.5f / 1000.f; r.z = (blo[2] + bhi[2]) * 0.5f / 1000.f;
                    r.y = (blo[1] < p[1] ? blo[1] : p[1]) / 1000.f;
                    r.width = fminf(2.4f, horiz / 1000.f + 0.4f);
                    r.height = fmaxf(0.5f, fminf(1.2f, ey / 1000.f + 0.35f));
                    static int said = 0;
                    if (said++ < 6) BridgeLog("combat: %s #%u is down - stand-in laid flat (%.1f x %.1f blocks)", EnemyName(id), i, r.width, r.height);
                }
            }
            if (breakable) {
                BreakableSize(e, id, r.width, r.height); r.flags = dead ? 2u : 0u;
                if (id == 0x5C || id == 0x57) r.y -= r.height * 0.5f;   // hanging things: the box is centred on them
                else r.y -= 0.15f;
                static uint32_t seenShoot[8]; static int nSeen = 0; uint32_t key = (uint32_t)id << 16 | i;
                if (IsShootable(id)) { bool k = false; for (int q = 0; q < nSeen; q++) k |= seenShoot[q] == key;
                    if (!k && nSeen < 8) { seenShoot[nSeen++] = key; BridgeLog("combat: %s #%u (id 0x%02X) at (%.1f %.1f %.1f) can be hit", BreakableName(id), i, id, p[0] / 1000.f, p[1] / 1000.f, p[2] / 1000.f); } }
            }
            r.healthFrac = mx > 0 ? (hp > 0 ? (float)hp / mx : 0.f) : 1.f;
            r.level = 1;
            snprintf(r.name, sizeof r.name, "%s", breakable ? BreakableName(id) : EnemyName(id));
            memcpy(view + 0x12000 + 0x40 + n * 64, &r, 64);
            n++;
        }
        {   // once per room: which object ids the room has (for mapping more scenery later)
            static uint32_t loggedEpoch = 0;
            if (loggedEpoch != bridge::CollisionEpoch()) {
                loggedEpoch = bridge::CollisionEpoch();
                int cntId[256] = {}; for (uint32_t k = 0; k < cnt; k++) { uint8_t* q = arr + k * blk; if (Rd<uint32_t>(q, off::be_flag) & 0x601) cntId[Rd<uint8_t>(q, off::id)]++; }
                char line[400]; int len = 0;
                for (int k = 0; k < 256 && len < 360; k++) if (cntId[k]) len += snprintf(line + len, sizeof line - len, " %02X:%d", k, cntId[k]);
                BridgeLog("combat: room objects (id:count)%s", len ? line : " none");
            }
        }
        collision::SetDoors(doorTris, bridge::CollisionEpoch());
        *(volatile uint32_t*)(view + 0x12004) = n;
        MemoryBarrier(); *seq = (s | 1) + 1;
        static uint32_t lastN = 0xFFFFFFFF;
        if (n != lastN) { BridgeLog("combat: %u enemies near Leon sent to Minecraft", n); lastN = n; }
    }

    if (haveEnemies) {
        g_enemyBoxes.clear();
        for (uint32_t i = 0; i < cnt && i < 512; i++) {
            uint8_t* e = arr + i * blk;
            if (!(Rd<uint32_t>(e, off::be_flag) & 0x601) || !IsEnemy(Rd<uint8_t>(e, off::id))) continue;
            if (Rd<int16_t>(e, off::hp) <= 0 || Rd<uint8_t>(e, off::r_no) == 3) continue;
            float p[3]; memcpy(p, e + off::pos, 12);
            float dx = p[0] - plp[0], dz = p[2] - plp[2];
            if (dx * dx + dz * dz > 30000.f * 30000.f) continue;
            g_enemyBoxes.insert(g_enemyBoxes.end(), p, p + 3);
        }
    }

    // 2) Minecraft's hits on enemies
    volatile uint64_t* head = (volatile uint64_t*)(view + 0x17000);
    volatile uint64_t* tail = (volatile uint64_t*)(view + 0x17040);
    uint64_t t = *tail, h = *head;
    while (t < h) {
        McEvent ev; memcpy(&ev, view + 0x17080 + (t % 512) * 32, 32);
        t++;
        if (ev.type == 1 && haveEnemies && (ev.formId & 0xF0000000u) == 0x20000000u) {   // a padlock on a door
            uint32_t idx = (ev.formId >> 16) & 0xFFF, side = ev.formId & 1;
            if (idx >= cnt || idx >= 512) continue;
            uint8_t* e = arr + idx * blk;
            if (blk < (uint32_t)g_doorWk + 0x430 || Rd<uint8_t>(e, off::id) != kDoorId || !(Rd<uint32_t>(e, off::be_flag) & 0x601)) continue;
            if (!Rd<uint8_t*>(e, g_doorWk + (side ? 0x42C : 0x428)) || (e[0x328 + 4] & 1)) continue;
            uint8_t* box = e + g_doorWk + 0x28 + (side ? 11 : 12) * 0x34;   // EmDoorWork::hit[11] right lock, [12] left lock
            float dx = Rd<float>(e, off::pos) - plp[0], dy = Rd<float>(e, off::pos + 4) - plp[1], dz = Rd<float>(e, off::pos + 8) - plp[2];
            RegisterHit(e, kWepHandgun, plp, dx * dx + dy * dy + dz * dz, box);
            BridgeLog("combat: Minecraft hit a padlock (door #%u, %s)", idx, side ? "right" : "left");
            continue;
        }
        if (ev.type == 1 && haveEnemies) {             // kEvHitActor
            uint32_t idx = (ev.formId >> 16) & 0xFFF;
            if (idx >= cnt || idx >= 512) continue;
            uint8_t* e = arr + idx * blk;
            if (FormId(idx, e) != ev.formId || !(Rd<uint32_t>(e, off::be_flag) & 0x601)) continue;
            int oid = Rd<uint8_t>(e, off::id);
            if (IsBreakable(oid)) {   // the object's own code takes the hit (boxes/barrels: one hit, windows: 1-4)
                if (Rd<int16_t>(e, off::hp) > 0 && !(e[0x328 + 4] & 1)) {
                    float dx = Rd<float>(e, off::pos) - plp[0], dy = Rd<float>(e, off::pos + 4) - plp[1], dz = Rd<float>(e, off::pos + 8) - plp[2];
                    // the axe (and arrows) hit like a shot: glass breaks at once instead of after several knife hits
                    // hit like a shot: glass and crates break at any of Minecraft's reach (a knife hit only counts up close)
                    RegisterHit(e, kWepHandgun, plp, dx * dx + dy * dy + dz * dz);
                    if (oid == 0x53 || oid == 0x58) NoteCrateHit(e);
                    BridgeLog("combat: Minecraft hit %s #%u (%s)", BreakableName(oid), idx, (ev.flags & 2) ? "arrow" : "melee");
                }
                continue;
            }
            Pending& pd = g_pend[idx];
            bool busy = pd.em == e && pd.frames > 0;
            int16_t hp = busy ? pd.target : Rd<int16_t>(e, off::hp);
            if (hp <= 0 || Rd<uint8_t>(e, off::r_no) == 3) continue;
            bool axe = ev.weapon == 2 && !(ev.flags & 2);   // SkyCraft WEAPON_AXE: RECraft's heavy weapon (one swing per cooldown)
            int dmg = (int)lroundf(ev.a * cfg::damageScale * (axe ? cfg::axeDamage : 1.0f));
            if (dmg < 1) dmg = 1;
            int nhp = hp - dmg;
            int id = Rd<uint8_t>(e, off::id);
            bool arrow = (ev.flags & 2) != 0, crit = (ev.flags & 1) != 0;
            // Difficulty: only strong hits make an enemy flinch, and not more often than every
            // StaggerCooldownMs; other hits just take health (the enemy keeps coming). Killing blows
            // always go through RE4 so it plays the death.
            static uint8_t* lastEm[512]; static ULONGLONG lastStagger[512];
            ULONGLONG tnow = GetTickCount64();
            if (lastEm[idx] != e) { lastEm[idx] = e; lastStagger[idx] = 0; }
            bool strong = crit || ev.a >= cfg::staggerMinDamage;
            bool cooled = tnow - lastStagger[idx] >= (ULONGLONG)cfg::staggerCooldownMs;
            bool lucky = (rand() % 100) < cfg::staggerChance;    // now and then a weak hit staggers too
            int16_t mxhp = Rd<int16_t>(e, off::hp_max);
            bool blast = ev.weapon == 0 && !(ev.flags & 2) && ev.a >= 8.0f;   // TNT and other big blows with nothing in hand
            if (nhp > 0 && !axe && !blast && (!cooled || !(strong || lucky))) {
                if (busy) pd.target = (int16_t)nhp; else Wr<int16_t>(e, off::hp, (int16_t)nhp);
                PublishHit(EnemyName(id), nhp, mxhp, 0);
                BridgeLog("combat: Minecraft hit %s #%u for %.1f (%s%s, no flinch) -> hp %d -> %d", EnemyName(id), idx, ev.a,
                          crit ? "crit " : "", arrow ? "arrow" : "melee", hp, nhp);
                continue;
            }
            if (nhp > 0) lastStagger[idx] = tnow;
            PublishHit(EnemyName(id), nhp, mxhp, nhp > 0 ? 1u : 3u);
            // knock-downs only from a critical hit that also knocks back hard (sprint / Knockback)
            int type = arrow ? kWepHandgun : ((axe || blast || (crit && ev.d > 0.6f)) && IsGanadoFamily(id)) ? kWepKick : kWepKnife;   // the axe and TNT knock down
            pd.em = e; pd.target = (int16_t)(nhp > 0 ? nhp : 0); pd.lethal = nhp <= 0; pd.frames = 30;
            if (pd.lethal) Wr<int16_t>(e, off::hp, 1);                       // any RE4 damage kills
            else Wr<int16_t>(e, off::hp, (int16_t)(nhp + 20000 > 32000 ? 32000 : nhp + 20000));   // RE4 can't kill it with this hit
            if (!(e[0x328 + 4] & 1)) {
                float dx = Rd<float>(e, off::pos) - plp[0], dy = Rd<float>(e, off::pos + 4) - plp[1], dz = Rd<float>(e, off::pos + 8) - plp[2];
                RegisterHit(e, type, plp, dx * dx + dy * dy + dz * dz, AimedPart(e, nullptr));
            }
            BridgeLog("combat: Minecraft hit %s #%u for %.1f (%s%s, RE4 hit type 0x%02X) -> hp %d -> %d%s",
                      EnemyName(id), idx, ev.a, crit ? "crit " : "", arrow ? "arrow" : "melee", type, hp,
                      pd.target, pd.lethal ? " (killed)" : "");
        } else if (ev.type == 2) {                     // kEvPlayerDied
            BridgeLog("combat: the Minecraft player died");
            g_mcDied = true;
        }
    }
    *tail = t;

    // 3) settle hits RE4 has processed (or that timed out): health becomes what Minecraft decided
    if (haveEnemies)
        for (uint32_t i = 0; i < cnt && i < 512; i++) {
            Pending& pd = g_pend[i];
            if (pd.frames <= 0) continue;
            if (pd.em != arr + i * blk) { pd.frames = 0; continue; }
            bool handled = !(pd.em[0x328 + 4] & 1);
            if (!handled && --pd.frames > 0) continue;
            pd.frames = 0;
            int16_t cur = Rd<int16_t>(pd.em, off::hp);
            if (!pd.lethal) { if (cur > 0) Wr<int16_t>(pd.em, off::hp, pd.target); }
            else if (!handled && cur > 0) BridgeLog("combat: RE4 ignored the killing blow on #%u (busy animation); it stays at 1 hp", i);
        }

    // 3b) Minecraft blocks stop enemies
    bool companion = Companion(view);
    g_view = view; g_companionOk = companion;
    if (companion) ReadBlocks(view);
    if (companion) GearTick(puppet);


    if (haveEnemies && companion && !g_blocks.empty()) {
        static int pushes = 0;
        for (uint32_t i = 0; i < cnt && i < 512; i++) {
            uint8_t* e = arr + i * blk;
            if (!(Rd<uint32_t>(e, off::be_flag) & 0x601) || !IsEnemy(Rd<uint8_t>(e, off::id))) { g_lastOkEm[i] = nullptr; continue; }
            float p[3]; memcpy(p, e + off::pos, 12);
            if (!InBlocks(p)) { memcpy(g_lastOk[i], p, 12); g_lastOkEm[i] = e; continue; }
            if (g_lastOkEm[i] == e) {
                p[0] = g_lastOk[i][0]; p[2] = g_lastOk[i][2];
                if (InBlocks(p)) p[1] = g_lastOk[i][1];
                memcpy(e + off::pos, p, 12);
                if (pushes++ < 5) BridgeLog("combat: %s #%u stopped by a Minecraft block", EnemyName(Rd<uint8_t>(e, off::id)), i);
            }
        }
    }

    // 4) Health. LinkHealth=1 (default): one shared health, RE4's. Enemies hurt Leon the normal RE4
    //    way and Minecraft loses the same share of its 20 health (magic damage, so armour doesn't
    //    change the share); Leon dying kills the Minecraft player, and the Minecraft player dying
    //    (fall, lava...) zeroes Leon's health, which RE4 turns into its own death screen
    //    (decompilation: gameDiedemoCheck). LinkHealth=0: the old way, Minecraft's health decides.
    static int16_t lastHp = -1;
    static bool killedMc = false, wasReady = false, syncPending = false, firstSync = true;
    static ULONGLONG lastHurtSent = 0;
    int16_t hp = Rd<int16_t>(gl, off::pl_hp), mx = Rd<int16_t>(gl, off::pl_hp_max);
    auto& S = bridge::S();
    bool ready = S.mcReady;
    ULONGLONG now = GetTickCount64();
    auto hurtMc = [&](float mcDmg) {
        bridge::PushInput(7 /*kInHurt*/, 2 /*magic: no armour*/, (int32_t)lroundf(mcDmg * 5.f * 100.f), 0, 0);
        lastHurtSent = now;
    };
    if (cfg::linkHealth && companion) {
        // One shared health, both ways, through RECraft's Minecraft mod: whichever game's health
        // changed (RE4: hits, herbs, death, continue; Minecraft: regeneration, food, falls...) sets the
        // other's to the same share.
        static float lastMc = -1; static int16_t lastRe = -1; static uint32_t lastDeaths = 0xFFFFFFFF;
        static ULONGLONG holdMcUntil = 0;
        uint8_t* b = view + mcx::kBase;
        float mcH = *(volatile float*)(b + mcx::oHealth), mcMax = *(volatile float*)(b + mcx::oMax);
        bool mcAlive = (*(volatile uint32_t*)(b + mcx::oFlags) & 1) != 0;
        uint32_t deaths = *(volatile uint32_t*)(b + mcx::oDeaths);
        if (lastDeaths == 0xFFFFFFFF) lastDeaths = deaths;
        if (mcMax <= 0) mcMax = 20.f;
        if (mx > 0) {
            if (lastRe < 0 || lastMc < 0) {               // link start: Minecraft takes Leon's share
                if (hp > 0 && mcAlive) { float t = (float)hp / mx * mcMax; RequestMcHealth(view, t); lastMc = t; lastRe = hp; holdMcUntil = now + 800;
                    BridgeLog("combat: shared health - Minecraft set to Leon's %d/%d (%.1f of %.0f)", hp, mx, t, mcMax); }
            } else if (hp != lastRe) {                    // RE4 changed it
                float am = Mx<float>(mcx::oArmorMul);
                if (!(am > 0.05f && am <= 1.0f)) am = 1.0f;
                float mul = am * cfg::enemyDamage;   // difficulty x armour
                if (hp > 0 && hp < lastRe && fabsf(mul - 1.0f) > 0.01f) {   // (RE4's own killing blows stay as they are)
                    int lost = lastRe - hp, kept = (int)lroundf(lost * mul);
                    if (kept < 1) kept = 1;
                    int nh = lastRe - kept; if (nh < 0) nh = 0;
                    BridgeLog("combat: RE4 hit Leon for %d -> %d (enemy damage x%.2f, armour x%.2f)%s", lost, kept, cfg::enemyDamage, am, nh <= 0 ? " - fatal" : "");
                    hp = (int16_t)nh; Wr<int16_t>(gl, off::pl_hp, hp);
                }
                float t = hp <= 0 ? 0.f : (float)hp / mx * mcMax;
                if (hp > 0 && t < 0.5f) t = 0.5f;
                if (mcAlive || hp > 0) RequestMcHealth(view, t);
                BridgeLog("combat: Leon's health %d -> %d of %d -> Minecraft %.1f of %.0f%s", lastRe, hp, mx, t, mcMax, hp <= 0 ? " (Leon died: so does the Minecraft player)" : "");
                lastRe = hp; lastMc = t; holdMcUntil = now + 800;
                if (hp <= 0) lastDeaths = deaths + 1;   // that death is ours
            } else if (deaths != lastDeaths) {            // the Minecraft player died on its own
                lastDeaths = deaths;
                if (hp > 0) { Wr<int16_t>(gl, off::pl_hp, (int16_t)0); hp = 0; lastRe = 0;
                    BridgeLog("combat: the Minecraft player died -> Leon's health set to 0 (RE4's death screen)"); }
            } else if (now > holdMcUntil && mcAlive && hp > 0 && fabsf(mcH - lastMc) > 0.2f) {   // Minecraft changed it
                int nh = (int)lroundf(mcH / mcMax * mx);
                if (nh < 1) nh = 1; if (nh > mx) nh = mx;
                BridgeLog("combat: Minecraft health %.1f -> %.1f -> Leon %d -> %d of %d", lastMc, mcH, hp, nh, mx);
                Wr<int16_t>(gl, off::pl_hp, (int16_t)nh); hp = (int16_t)nh; lastRe = hp; lastMc = mcH;
            }
        }
        g_mcDied = false;
    } else if (cfg::linkHealth) {
        // Minecraft can't be told its health, only hurt. So the Minecraft player's health is made
        // known once: on the first link it's knocked out (keepInventory + instant respawn are on in
        // SkyCraft's world, nothing is lost) and comes back at a full 20; after any respawn it's then
        // brought down to Leon's share.
        static ULONGLONG readySince = 0;
        if (!ready) readySince = 0; else if (!readySince) readySince = now;
        bool readyLong = readySince && now - readySince > 6000;   // give RECraft's Minecraft mod time to show up first
        if (ready && readyLong && firstSync && hp > 0 && mx > 0) {
            firstSync = false; syncPending = true; killedMc = true;
            hurtMc(10000.f);
            BridgeLog("combat: shared health - resetting the Minecraft player to full health so its hearts can follow Leon's");
        }
        if (ready && !wasReady && !firstSync) syncPending = true;            // just (re)spawned: 20 health
        if (syncPending && ready && hp > 0 && mx > 0 && now - lastHurtSent > 1500) {
            syncPending = false;
            float mcDmg = (float)(mx - hp) / mx * 20.f;
            if (mcDmg > 0.05f) hurtMc(mcDmg);
            BridgeLog("combat: Minecraft hearts synced to Leon's health (%d/%d -> %.1f of 20)", hp, mx, 20.f - mcDmg);
        }
        if (ready && lastHp > 0 && hp < lastHp && mx > 0 && !syncPending) {
            int lost = lastHp - hp;
            float mcDmg = hp <= 0 ? 10000.f : (float)lost / mx * 20.f;
            hurtMc(mcDmg);
            BridgeLog("combat: Leon lost %d of %d health -> Minecraft loses %.1f of 20%s", lost, mx, hp <= 0 ? 20.f : mcDmg,
                      hp <= 0 ? " (Leon died: Minecraft player dies too)" : "");
            if (hp <= 0) killedMc = true;
        }
        if (hp > 0 && now - lastHurtSent > 3000) killedMc = false;
        if (g_mcDied) {
            g_mcDied = false;
            syncPending = true;
            if (hp > 0 && !killedMc && now - lastHurtSent > 1500) {
                Wr<int16_t>(gl, off::pl_hp, (int16_t)0);
                BridgeLog("combat: the Minecraft player died on its own -> Leon's health set to 0 (RE4's death screen)");
                hp = 0;
            } else if (hp > 0) {
                BridgeLog("combat: the Minecraft player went down from a hit Leon survived (its hearts were behind) - Leon lives, hearts re-synced on respawn");
            }
        }
        wasReady = ready;
    } else if (puppet && lastHp > 0 && hp < lastHp) {
        int lost = lastHp - hp;
        bridge::PushInput(7 /*kInHurt*/, 0 /*melee*/, (int32_t)lroundf(lost * cfg::hurtScale * 5.f * 100.f), 0, 0);
        Wr<int16_t>(gl, off::pl_hp, lastHp);   // Leon keeps his RE4 health
        BridgeLog("combat: RE4 hit Leon for %d -> Minecraft damage %.1f", lost, lost * cfg::hurtScale);
        hp = lastHp;
    }
    lastHp = hp;
}

// Diagnostics: on a left click, what was under / near the crosshair among the stand-ins we publish.
void LogAim(uint8_t* view) {
    static int logs = 0; static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (!view || logs >= 80 || now - last < 400) return;
    last = now; logs++;
    auto& S = bridge::S();
    float yaw, pitch; bridge::GetLook(yaw, pitch);
    float yr = yaw * 0.0174532925f, pr = pitch * 0.0174532925f;
    double d[3] = {-sin(yr) * cos(pr), -sin(pr), cos(yr) * cos(pr)};
    double o[3] = {S.eyeX, S.eyeY, S.eyeZ};
    uint32_t n = *(volatile uint32_t*)(view + 0x12004); if (n > 256) n = 256;
    int best = -1; double bestAng = 1e9, bestDist = 0, bestDy = 0; int hit = -1; double hitT = 1e9;
    for (uint32_t i = 0; i < n; i++) {
        ActorRecord r; memcpy(&r, view + 0x12040 + i * 64, 64);
        if (r.flags & 2) continue;
        double lo[3] = {r.x - r.width / 2, r.y, r.z - r.width / 2}, hi[3] = {r.x + r.width / 2, r.y + r.height, r.z + r.width / 2};
        double t0 = 0, t1 = 1e9; bool ok = true;
        for (int a = 0; a < 3 && ok; a++) {
            if (fabs(d[a]) < 1e-9) { if (o[a] < lo[a] || o[a] > hi[a]) ok = false; continue; }
            double ta = (lo[a] - o[a]) / d[a], tb = (hi[a] - o[a]) / d[a]; if (ta > tb) { double t = ta; ta = tb; tb = t; }
            if (ta > t0) t0 = ta; if (tb < t1) t1 = tb; if (t0 > t1) ok = false;
        }
        if (ok && t0 < hitT) { hitT = t0; hit = (int)i; }
        double c[3] = {r.x - o[0], r.y + r.height / 2 - o[1], r.z - o[2]};
        double len = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        double ang = len > 1e-6 ? acos(fmax(-1.0, fmin(1.0, (c[0] * d[0] + c[1] * d[1] + c[2] * d[2]) / len))) * 57.2958 : 0;
        if (ang < bestAng) { bestAng = ang; best = (int)i; bestDist = len; bestDy = r.y - (S.feetY); }
    }
    char nm[25] = "-";
    if (hit >= 0) { ActorRecord r; memcpy(&r, view + 0x12040 + hit * 64, 64); memcpy(nm, r.name, 24); nm[24] = 0;
        BridgeLog("aim: click - crosshair on %s at %.1f blocks (%u stand-ins published, Minecraft reach is 3)", nm, hitT, n); }
    else if (best >= 0) { ActorRecord r; memcpy(&r, view + 0x12040 + best * 64, 64); memcpy(nm, r.name, 24); nm[24] = 0;
        BridgeLog("aim: click - nothing under the crosshair; closest is %s %.1f deg off, %.1f blocks away, %.1f blocks above your feet (%u published)", nm, bestAng, bestDist, bestDy, n); }
    else BridgeLog("aim: click - no stand-ins published at all (%u)", n);
}

} // namespace combat
