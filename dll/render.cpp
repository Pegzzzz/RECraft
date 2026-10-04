// render.cpp - per-frame work on RE4's threads:
//   * Direct3D 9 Present hook: run the bridge each frame and draw Minecraft's HUD/hand overlay.
//   * Camera hook (CameraQuasiFPS::hitCheck, same place re4_tweaks' free camera uses): while
//     Minecraft drives Leon, put RE4's camera at the Minecraft player's eye with Minecraft's look,
//     and place Leon at the Minecraft player's feet.

#include "link.h"
#include "memcheck.h"
#include <d3d9.h>
#include <tmmintrin.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <emmintrin.h>
#include "MinHook.h"

extern volatile ULONGLONG g_lastPresent;
namespace bridge { float McYawToHeading(float yaw); void TickStats(int& n, double& avg, double& worst, int& late, int& snaps); }
namespace input { extern volatile bool g_re4Control; }
namespace combat { void Tick(uint8_t* view, bool puppet); }
namespace blocks { void Draw(IDirect3DDevice9* dev, bool mcAlive); void SetAvatar(bool show, const float parts[7][3][4]); bool HaveAvatar(); bool CameraEye(float eye[3]); float AvatarDistance(const float parts[7][3][4], const float eye[3], float* nearest); void OnReset(); void DrawPortrait(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1); }

namespace items { void Tick(bool paused); }
namespace render {
float g_camEye[3] = {0, 0, 0};   // where the final camera was put this frame (blocks.cpp checks RE4's view matrix against it)

struct Vec { float x, y, z; };
struct CAMERA_POINT { Vec Campos; Vec Target; float Roll; float Fovy; };

uint8_t*** g_ppPlayer = nullptr;   // set by RECraft.cpp
uint8_t*** g_ppGlobal = nullptr;
static uint32_t g_subType = 0;      // which sub screen (SS_OPEN_*: 0x10 shop, 0x20 radio, 0x80 pick-up...)
uint8_t* g_subScreen = nullptr;     // SUB_SCREEN work (re4_tweaks' SubScreenWk), set by RECraft.cpp
uint8_t* g_evtMgr = nullptr;        // EventMgr (re4_tweaks' EvtMgr), set by RECraft.cpp
// Story cutscenes play Leon with their own copy of his body (decompilation event.cpp: the "pl0000" model,
// Event::PPl_80), not the gameplay Leon we hide. Returns it while such an event runs.
static uint8_t* EventLeon() {
    uint8_t* m = g_evtMgr;
    MEMORY_BASIC_INFORMATION mi;
    auto ok = [&](const void* p, size_t n) { return p && VirtualQuery(p, &mi, sizeof mi) && mi.State == MEM_COMMIT && (uint8_t*)mi.BaseAddress + mi.RegionSize >= (uint8_t*)p + n; };
    if (!ok(m, 0x20)) return nullptr;
    uint8_t* arr = *(uint8_t**)(m + 4); uint32_t n = *(uint32_t*)(m + 8), blk = *(uint32_t*)(m + 0xC);   // cManager<Event>
    if (n == 0 || n > 16 || blk < 0x110 || blk > 0x400 || !ok(arr, (size_t)n * blk)) return nullptr;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t* e = arr + i * blk;
        if (!(*(uint32_t*)(e + 4) & 1)) continue;                    // cUnit::be_flag: in use
        uint8_t* ppl = *(uint8_t**)(e + 0x80);                         // Event::PPl_80
        if (ok(ppl, 0x200)) return ppl;
    }
    return nullptr;
}
// One of RE4's menu screens is up (item pick-up, attache case, shop, map, files): they draw with their own
// camera, so nothing of Minecraft's (body, blocks, HUD) belongs on top of them.
static bool g_dieDemo = false;      // Leon's death (QTE failed, killed...) is playing
static bool SubScreenOpen() {
    uint8_t* w = g_subScreen;
    if (!w) return false;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(w + 0x2C, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
    bool open = *(volatile uint32_t*)(w + 0x2C) != 0;   // SUB_SCREEN::open_flag_2C
    {   // the item pick-up view (STA_ITEM_GET, Status word 1 bit 0x2) shows the item in front of the camera
        uint8_t* gl = g_ppGlobal ? (uint8_t*)*g_ppGlobal : nullptr;
        if (gl && VirtualQuery(gl + 0x5020, &mi, sizeof mi) && mi.State == MEM_COMMIT && (*(volatile uint32_t*)(gl + 0x5020) & 0x2)) open = true;
        g_dieDemo = gl && VirtualQuery(gl + 0x501C, &mi, sizeof mi) && mi.State == MEM_COMMIT && (*(volatile uint32_t*)(gl + 0x501C) & 0x100000);
        if (g_dieDemo) open = true;   // STA_DIEDEMO: Leon's death and "You are dead" (Minecraft's HUD goes; the body stays for the death)
    }
    static bool was = false;
    if (open != was) { BridgeLog("menu: RE4 sub screen %s (type %X)", open ? "open - Minecraft's body, blocks and HUD hidden" : "closed", *(volatile uint32_t*)(w + 0x2C)); was = open; }
    g_subType = open ? *(volatile uint32_t*)(w + 0x2C) : 0;
    return open;
}
static bool g_menuOpen = false;
static bool g_leonNeeded = false;   // RE4 moves Leon and no Minecraft body can stand in: keep him visible
static bool g_leonAction = false;   // RE4 is moving Leon itself (event, door, ladder, hit, grab, death)
static bool g_avatarOn = false;     // the Minecraft body is standing in for Leon this frame
static bool g_boat = false;          // Leon is in the lake boat (Del Lago): RE4's controls, RE4's life meter
static bool g_scripted = false;      // a story movie, or Leon hit / grabbed / in a scripted event (QTEs): RE4's own show
static bool g_sceneClean = false;    // this frame nothing of Minecraft's is drawn over RE4's picture
static uint32_t g_stopFlags = 0;    // GLOBAL_WK +0x170: what RE4 has halted (bit per system; ~all set = a full-screen menu)
uint8_t* g_idSys = nullptr;         // IDSystem (re4_tweaks' IdSys): the HUD pieces, set by RECraft.cpp
// RE4's own HUD while Minecraft is the player: the life meter (IDC_LIFE_METER 0x21) and the ammo icon
// (IDC_BLLT_ICON 0x32) are switched off through IDSystem::m_disp_off_2C (one bit per class, MSB first);
// action prompts and count-downs stay.
static void Re4HudTick(bool hide) {
    static bool hidden = false;
    uint8_t* s = g_idSys;
    MEMORY_BASIC_INFORMATION mi;
    if (!s || !VirtualQuery(s + 0x2C, &mi, sizeof mi) || mi.State != MEM_COMMIT) return;
    volatile uint32_t* off = (volatile uint32_t*)(s + 0x2C);
    const uint32_t lifeBit = 0x80000000u >> (0x21 & 31), bulletBit = 0x80000000u >> (0x32 & 31);   // both in word 1
    if (hide) { off[1] |= lifeBit | bulletBit; if (!hidden) { hidden = true; BridgeLog("hud: RE4's life meter and ammo icon hidden"); } }
    else if (hidden) { off[1] &= ~(lifeBit | bulletBit); hidden = false; BridgeLog("hud: RE4's life meter shown again"); }
}

static inline bool Readable(const void* p, size_t n) { return mem::Readable(p, n); }   // cached (memcheck.h)

static uint8_t* Player() {
    uint8_t* pl = g_ppPlayer ? (uint8_t*)*g_ppPlayer : nullptr;
    return Readable(pl, 0x110) ? pl : nullptr;
}

static bridge::Leon ReadLeon() {
    bridge::Leon l{};
    uint8_t* pl = Player();
    uint8_t* gl = g_ppGlobal ? (uint8_t*)*g_ppGlobal : nullptr;
    if (!pl || !Readable(gl, 0x4FB0)) return l;
    float pos[3], ang[3];
    memcpy(pos, pl + 0x94, 12); memcpy(ang, pl + 0xA0, 12); memcpy(&l.room, gl + 0x4FAC, 2);
    l.valid = true; l.x = pos[0]; l.y = pos[1]; l.z = pos[2]; l.yawRad = ang[1];
    return l;
}

// ------------------------------------------------------------------ camera
typedef void (__attribute__((fastcall)) *HitCheckFn)(void* self, void* edx, float (*plMat)[4], void* offset, CAMERA_POINT* aim);
static HitCheckFn g_hitCheckOrig = nullptr;
static int g_camLogs = 0;

static volatile ULONGLONG g_lastHitCheck = 0;   // RE4's gameplay camera ran (0 = never seen)
static int64_t g_camTicks = 0;                    // time in our camera hook (enemies, hits, health) since the last perf line
static volatile int g_presentsSinceHit = 0;      // frames shown since it last ran (a hitch stops both, so it doesn't count)
static uint8_t* volatile g_camCtrl = nullptr;      // RE4's CameraControl (CamCtrl)
static void HideLeonTick(bool fromCamera);

static void __attribute__((fastcall)) HitCheckHook(void* self, void* edx, float (*m)[4], void* offset, CAMERA_POINT* aim) {
    g_hitCheckOrig(self, edx, m, offset, aim);
    LARGE_INTEGER camT0; QueryPerformanceCounter(&camT0);
    struct CamTime { LARGE_INTEGER t0; ~CamTime() { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); g_camTicks += t1.QuadPart - t0.QuadPart; } } camTime{camT0};
    mem::Reset();
    g_lastHitCheck = GetTickCount64();
    g_presentsSinceHit = 0;
    if (self && Readable((uint8_t*)self - 0x278, 0x300)) g_camCtrl = (uint8_t*)self - 0x278;   // CameraControl::m_QuasiFPS_278
    auto& S = bridge::S();
    if (bridge::McAlive()) combat::Tick(bridge::View(), S.puppet);
    HideLeonTick(true);
    if (!S.puppet || !S.haveEye || !aim || !m) return;

    // Leon stands where Minecraft's player stands and faces where it looks.
    float yaw, pitch; bridge::GetLook(yaw, pitch);
    if (uint8_t* pl = Player()) {
        float p[3] = {(float)(S.feetX * bridge::kUnitsPerBlock), (float)(S.feetY * bridge::kUnitsPerBlock), (float)(S.feetZ * bridge::kUnitsPerBlock)};
        memcpy(pl + 0x94, p, 12);
        float h = bridge::McYawToHeading(yaw);
        memcpy(pl + 0xA4, &h, 4);   // ang_A0.y
    }

    float yr = yaw * 0.0174532925f, pr = pitch * 0.0174532925f;
    Vec dir = {-sinf(yr) * cosf(pr), -sinf(pr), cosf(yr) * cosf(pr)};   // Minecraft's view direction
    Vec eye = {(float)(S.eyeX * bridge::kUnitsPerBlock), (float)(S.eyeY * bridge::kUnitsPerBlock), (float)(S.eyeZ * bridge::kUnitsPerBlock)};
    Vec tgt = {eye.x + dir.x * 1000.f, eye.y + dir.y * 1000.f, eye.z + dir.z * 1000.f};

    Vec c0 = aim->Campos;
    bool world = sqrtf(c0.x * c0.x + c0.z * c0.z) > 10000.f;
    if (!world) {
        // The game's camera point is relative to the player matrix: undo it (R^T * (p - t)).
        auto toLocal = [&](Vec p) {
            float dx = p.x - m[0][3], dy = p.y - m[1][3], dz = p.z - m[2][3];
            return Vec{m[0][0] * dx + m[1][0] * dy + m[2][0] * dz,
                       m[0][1] * dx + m[1][1] * dy + m[2][1] * dz,
                       m[0][2] * dx + m[1][2] * dy + m[2][2] * dz};
        };
        eye = toLocal(eye); tgt = toLocal(tgt);
    }
    if (g_camLogs < 4) {
        g_camLogs++;
        BridgeLog("camera: game pos(%.0f %.0f %.0f) target(%.0f %.0f %.0f) fov %.1f plMat t(%.0f %.0f %.0f) -> %s",
                  c0.x, c0.y, c0.z, aim->Target.x, aim->Target.y, aim->Target.z, aim->Fovy, m[0][3], m[1][3], m[2][3],
                  world ? "world coords" : "player-relative coords");
    }
    aim->Campos = eye; aim->Target = tgt;
    if (aim->Fovy > 15.f && aim->Fovy < 130.f) aim->Fovy = S.fov;
}

static void InstallCameraHook(uint8_t* callSite) {
    uint8_t* target = callSite + 5 + *(int32_t*)(callSite + 1);
    MH_STATUS st = MH_CreateHook(target, (void*)&HitCheckHook, (void**)&g_hitCheckOrig);
    if (st == MH_OK) st = MH_EnableHook(target);
    BridgeLog("camera hook at %p: %s", target, MH_StatusToString(st));
}

// ------------------------------------------------------------------ hiding Leon
// Minecraft's player *is* the player now, so Leon's model goes away - but only his looks: his
// position, skeleton and hit boxes stay exactly as RE4 has them, so the camera works and enemies can
// still hit him. RE4 fades models with cModel::invisible_factor (0x154, 0..1 material alpha: corpses
// dissolve and the character-select screen fades Leon in/out with it), so we hold it at 0 for Leon and
// for the weapon objects in his hands (cPlayer::Wep_7D8 -> cPlWep::m_pWep_34 / m_pWepHand_38).
static volatile bool g_wantHidden = false;
static bool  g_hideBroken = false;           // hiding upset the game once: leave Leon visible
static bool  g_hidden = false;
static uint8_t* g_hidModels[8]; static int g_nHid = 0;
static ULONGLONG g_hideStart = 0;
static int   g_hiddenFrames = 0;

static void SetAlpha(uint8_t* m, float a) { if (Readable(m, 0x160)) memcpy(m + 0x154, &a, 4); }

static int LeonModels(uint8_t* pl, uint8_t* out[3]) {
    int n = 0; out[n++] = pl;
    uint8_t* wep = *(uint8_t**)(pl + 0x7D8);
    if (Readable(wep, 0x40))
        for (int o = 0x34; o <= 0x38; o += 4) {
            uint8_t* m = *(uint8_t**)(wep + o);
            if (Readable(m, 0x160) && m != pl && (n < 2 || out[1] != m)) out[n++] = m;
        }
    return n;
}

static void HideLeonTick(bool fromCamera) {
    uint8_t* pl = Player();
    if (!pl || !Readable(pl, 0x7E0)) { g_hidden = false; g_nHid = 0; return; }
    if (g_wantHidden && !g_hideBroken) {
        if (!g_hidden) {
            g_hidden = true; g_hideStart = GetTickCount64(); g_hiddenFrames = 0;
            float f; memcpy(&f, pl + 0x154, 4);
            BridgeLog("leon: hidden (fade %.2f -> 0)", f);
            static bool skel = false;
            if (!skel) {   // Leon's bones (for posing the Minecraft body from his skeleton later)
                skel = true;
                uint8_t* bones[128]; int nb = 0;
                for (uint8_t* b = *(uint8_t**)(pl + 0xF4); b && nb < 128 && Readable(b, 0xF8); b = *(uint8_t**)(b + 0xF4)) bones[nb++] = b;
                float root[3]; memcpy(root, pl + 0x94, 12);
                char line[512]; int len = 0;
                for (int i = 0; i < nb; i++) {
                    uint8_t* par = *(uint8_t**)(bones[i] + 0x6C); int pi = -1;
                    for (int k = 0; k < nb; k++) if (bones[k] == par) pi = k;
                    float w[3]; memcpy(w, bones[i] + 0x70, 12);
                    len += snprintf(line + len, sizeof line - len, " %d<%d(%.0f,%.0f,%.0f)", i, pi, w[0] - root[0], w[1] - root[1], w[2] - root[2]);
                    if (len > 400 || i == nb - 1) { BridgeLog("leon: bones%s", line); len = 0; }
                }
            }
        }
        uint8_t* ms[3]; int n = LeonModels(pl, ms);
        for (int i = 0; i < n; i++) {
            SetAlpha(ms[i], 0.0f);
            bool known = false; for (int k = 0; k < g_nHid; k++) known |= g_hidModels[k] == ms[i];
            if (!known && g_nHid < 8) g_hidModels[g_nHid++] = ms[i];
        }
        if (!fromCamera && ++g_hiddenFrames == 30) BridgeLog("leon: still hidden after 30 frames (%d models)", n);
    } else if (g_hidden) {
        for (int k = 0; k < g_nHid; k++) SetAlpha(g_hidModels[k], 1.0f);
        g_nHid = 0; g_hidden = false;
        BridgeLog("leon: shown again");
    }
}

// ------------------------------------------------------------------ Minecraft body on Leon's skeleton
// Leon's skeleton (pl00, logged by 0.12): 0 hips, 2 chest, 3 neck, 4 head, 64 crown; arms 7>8>9>10
// and 13>14>15>16 (shoulder, elbow, wrist, hand); legs 18>19>20 and 22>23>24 (thigh, knee, ankle).
// Each of Minecraft's six parts keeps its blocky shape and is hung on the matching joints: placed at
// the joint, pointed along the limb, stretched to the limb's length, turned with the torso.
struct V3 { float x, y, z; };
static V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
static float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static float Len(V3 a) { return sqrtf(Dot(a, a)); }
static V3 Norm(V3 a, V3 fb) { float l = Len(a); return l > 1e-4f ? a * (1.f / l) : fb; }

static void SetPart(float M[3][4], V3 X, V3 Y, V3 Z, float sy, V3 at, V3 pivot) {
    V3 c[3] = {X, Y * sy, Z};
    for (int r = 0; r < 3; r++) {
        float* row = M[r];
        row[0] = r == 0 ? c[0].x : r == 1 ? c[0].y : c[0].z;
        row[1] = r == 0 ? c[1].x : r == 1 ? c[1].y : c[1].z;
        row[2] = r == 0 ? c[2].x : r == 1 ? c[2].y : c[2].z;
    }
    V3 ap = {M[0][0] * pivot.x + M[0][1] * pivot.y + M[0][2] * pivot.z, M[1][0] * pivot.x + M[1][1] * pivot.y + M[1][2] * pivot.z,
             M[2][0] * pivot.x + M[2][1] * pivot.y + M[2][2] * pivot.z};
    M[0][3] = at.x - ap.x; M[1][3] = at.y - ap.y; M[2][3] = at.z - ap.z;
}

static bool PoseFromLeon(uint8_t* pl, float out[7][3][4]) {
    float m[3][4]; memcpy(m, pl + 0xC, sizeof m);
    for (int p = 0; p < 7; p++) memcpy(out[p], m, sizeof m);   // fallback: the whole body on Leon's root
    uint8_t* bones[128]; int nb = 0;
    for (uint8_t* b = *(uint8_t**)(pl + 0xF4); b && nb < 128 && Readable(b, 0xF8); b = *(uint8_t**)(b + 0xF4)) bones[nb++] = b;
    auto parent = [&](int i) { uint8_t* p = *(uint8_t**)(bones[i] + 0x6C); for (int k = 0; k < nb; k++) if (bones[k] == p) return k; return -1; };
    static const int kChk[][2] = {{3, 2}, {4, 3}, {8, 7}, {9, 8}, {14, 13}, {15, 14}, {19, 18}, {20, 19}, {23, 22}, {24, 23}, {18, 17}, {22, 17}};
    static int bad = -1;
    if (nb < 26) { if (bad != 1) { bad = 1; BridgeLog("avatar: that Leon has %d bones - not drawn there", nb); } return false; }
    for (auto& c : kChk) if (parent(c[0]) != c[1]) { if (bad != 2) { bad = 2; BridgeLog("avatar: unexpected skeleton (bone %d) - not drawn there", c[0]); } return false; }
    bad = 0;
    auto P = [&](int i) { V3 v; memcpy(&v, bones[i] + 0x70, 12); return v; };
    V3 hips = P(0), neck = P(3), head = P(4);
    V3 top = (nb > 64 && parent(64) == 4) ? P(64) : head + (head - neck);
    V3 sA = P(7), wA = P(9), sB = P(13), wB = P(15);
    V3 tA = P(18), aA = P(20), tB = P(22), aB = P(24);
    V3 leonX = {m[0][0], m[1][0], m[2][0]};
    bool aRight = Dot(sA - hips, leonX) < Dot(sB - hips, leonX);   // facing +Z, the right side is -X
    bool lRight = Dot(tA - hips, leonX) < Dot(tB - hips, leonX);
    V3 sR = aRight ? sA : sB, wR = aRight ? wA : wB, sL = aRight ? sB : sA, wL = aRight ? wB : wA;
    V3 tR = lRight ? tA : tB, kR = lRight ? aA : aB, tL = lRight ? tB : tA, kL = lRight ? aB : aA;
    V3 up = {0, 1, 0};
    V3 Y = Norm(neck - hips, up);
    V3 Xb = Norm(sL - sR, leonX);
    V3 Z = Norm(Cross(Xb, Y), {m[0][2], m[1][2], m[2][2]});
    V3 X = Cross(Y, Z);
    const float px = 1000.f / 16.f;   // one Minecraft model pixel in mm
    V3 midHips = (tR + tL) * 0.5f;
    // body + head hang from the neck
    SetPart(out[2], X, Y, Z, Len(neck - midHips) / (12 * px), neck, {0, 24 * px, 0});
    V3 Yh = Norm(top - head, Y), Xh = Norm(Cross(Yh, Z), X), Zh = Cross(Xh, Yh);
    SetPart(out[1], Xh, Yh, Zh, 1.f, neck, {0, 24 * px, 0});
    auto limb = [&](float M[3][4], V3 s, V3 e, float extra, float mcLen, V3 pivot) {
        V3 Yl = Norm(s - e, Y), Xl = Norm(Cross(Yl, Z), X), Zl = Cross(Xl, Yl);
        SetPart(M, Xl, Yl, Zl, (Len(s - e) + extra) / mcLen, s, pivot);
    };
    limb(out[3], sR, wR, 90.f, 10 * px, {-5 * px, 22 * px, 0});
    limb(out[4], sL, wL, 90.f, 10 * px, {5 * px, 22 * px, 0});
    limb(out[5], tR, kR, 90.f, 12 * px, {-1.9f * px, 12 * px, 0});
    limb(out[6], tL, kL, 90.f, 12 * px, {1.9f * px, 12 * px, 0});
    return true;
}

// ------------------------------------------------------------------ cutscenes
// RE4's gameplay camera (the hitCheck hook) stops running during cutscenes, doors, ladders, menus...
// Cutscene/movie status flags (re4_tweaks' Flags_STATUS, GLOBAL_WK+0x501C) are a second signal.
static bool GameBusy(const bridge::Leon& leon) {
    uint8_t* gl = g_ppGlobal ? (uint8_t*)*g_ppGlobal : nullptr;
    uint32_t st[4] = {}, stop = 0;
    if (Readable(gl, 0x5030)) { memcpy(st, gl + 0x501C, 16); memcpy(&stop, gl + 0x170, 4); }
    g_stopFlags = stop;
    bool movie = (st[0] & 0x10000000) || (st[0] & 0x04000000);   // STA_MOVIE_ON, STA_MOVIE2_ON
    bool cinesco = (st[0] & 0x01000000) != 0;                      // STA_CINESCO (letterbox)
    bool event = (st[2] & 0x00080000) != 0;                        // STA_EVENT_SYSYTEM (logged only)
    ULONGLONG last = g_lastHitCheck;
    bool camIdle = last && g_presentsSinceHit > 10 && GetTickCount64() - last > 150;
    // Leon doing one of RE4's own moves (decompilation player.cpp routine tables): r_no_0 1 = hit
    // reaction / grabbed, 5 = scripted event; under r_no_0 0 (Move), r_no_1 7/8 = climb up/down a
    // ledge, 9 = pushing, 10 = aux action (doors...), 12 = vaulting a window/fence, 15 = boat,
    // 16 = ladder. RE4 plays the move out; Minecraft then picks Leon up where it ends.
    uint8_t r0 = 0, r1 = 0;
    if (uint8_t* pl = Player()) { r0 = pl[0xFC]; r1 = pl[0xFD]; }
    // Anything but plain standing / walking / turning / falling counts (the PC version's numbers for
    // some moves - kicking a ladder down, freeing the dog - differ from the GameCube decompilation).
    bool locomotion = r0 == 0 && (r1 <= 5 || r1 == 14 || r1 == 17 || r1 == 19);
    {   // the lake boat (Del Lago): RE4 plays it with its own controls, so RE4's own life meter shows meanwhile
        bool boat = leon.valid && r0 == 0 && r1 == 15;
        if (boat != g_boat) BridgeLog("boat: %s", boat ? "Leon is in the boat - RE4's controls: W/S speed, A/D steer, hold the right mouse "
                                                  "button to raise a harpoon (a spear), left click to throw, F to act" : "off the boat");
        g_boat = boat;
    }
    bool known = r0 == 1 || r0 == 5 || (r0 == 0 && (r1 == 7 || r1 == 8 || r1 == 9 || r1 == 10 || r1 == 12 || r1 == 15 || r1 == 16));
    static int otherFrames = 0;
    otherFrames = (!locomotion && !known && r0 != 2) ? otherFrames + 1 : 0;
    bool action = leon.valid && (known || otherFrames >= 3);
    g_leonAction = leon.valid && (action || r0 == 2 || r0 == 4);   // + dying, + grabbed
    g_scripted = leon.valid && (r0 == 1 || r0 == 4 || r0 == 5 || movie);
    {   // what routines Leon goes through while Minecraft has him (to refine the list)
        static uint16_t seen[64]; static int nSeen = 0;
        uint16_t key = (uint16_t)(r0 << 8 | r1);
        bool known = false; for (int i = 0; i < nSeen; i++) known |= seen[i] == key;
        if (!known && nSeen < 64) { seen[nSeen++] = key; BridgeLog("leon: routine %u/%u seen (%s)", r0, r1, action ? "RE4 action" : "movement"); }
    }
    static bool wasAction = false; static uint8_t lastR0 = 0xFF, lastR1 = 0xFF;
    if (action != wasAction || (action && (r0 != lastR0 || r1 != lastR1)))
        BridgeLog("leon: %s (routine %u/%u)", action ? "RE4 action - RE4 moves Leon" : "action over - back to Minecraft", r0, r1);
    wasAction = action; lastR0 = r0; lastR1 = r1;
    // STA_DIEDEMO (Status word 0 bit 0x100000): Leon's death and "You are dead" - RE4 keeps him to the end. 0.33 handed
    // him back to Minecraft as soon as the death motion's routine ended, and the respawned Minecraft player walked the
    // dead Leon (and the camera) around behind the red screen.
    bool dying = (st[0] & 0x00100000) != 0;
    bool busy = leon.valid && (camIdle || movie || action || dying);   // (STA_CINESCO stays set during normal play in UHD: logged only)
    static bool was = false; static uint32_t lastSig = 0;
    uint32_t sig = (camIdle ? 1 : 0) | (movie ? 2 : 0) | (cinesco ? 4 : 0) | (event ? 8 : 0) | (dying ? 16 : 0);
    if (busy != was || (busy && sig != lastSig)) {
        BridgeLog("game %s (camera %s%s%s%s%s) status %08X %08X %08X %08X stop %08X", busy ? "BUSY - cutscene/door/menu, RE4 keeps Leon" : "back to gameplay",
                  camIdle ? "idle" : "running", movie ? ", movie" : "", cinesco ? ", cinesco" : "", event ? ", event" : "", dying ? ", death" : "",
                  st[0], st[1], st[2], st[3], stop);
        was = busy; lastSig = sig;
    }
    return busy;
}

// ------------------------------------------------------------------ final camera
// RE4 smooths its camera twice after the shoulder camera (CameraQuasiFPS::move's sideways lag and
// CameraControl's CamSmth), which made the view trail Minecraft's eye: jitter, and arrows landing
// off the crosshair. The last step of CameraControl::move is CameraSetOrientationRoll(&camera)
// (decompilation: cam_ctrl.cpp); we set the camera there, after all smoothing, to Minecraft's exact
// eye and look. RE4's field of view is calibrated so the picture matches Minecraft's (HUD, hand).
typedef void (__cdecl *SetOriFn)(uint8_t* cam);
static SetOriFn g_setOriOrig = nullptr;
static volatile float g_fovK = 1.0f, g_lastFovySet = 0;
static volatile ULONGLONG g_lastFinalCam = 0;

static void __cdecl SetOriHook(uint8_t* cam) {
    auto& S = bridge::S();
    uint8_t* cc = g_camCtrl;
    if (cc && cam == cc + 0x60 && S.puppet && S.haveEye) {
        float yaw, pitch; bridge::GetLook(yaw, pitch);
        float yr = yaw * 0.0174532925f, pr = pitch * 0.0174532925f;
        float dir[3] = {-sinf(yr) * cosf(pr), -sinf(pr), cosf(yr) * cosf(pr)};
        float* prm = (float*)(cam + 0xA4);   // CAMERA::param: pos, at, roll, fovy
        prm[0] = (float)(S.eyeX * bridge::kUnitsPerBlock);
        prm[1] = (float)(S.eyeY * bridge::kUnitsPerBlock);
        prm[2] = (float)(S.eyeZ * bridge::kUnitsPerBlock);
        memcpy(g_camEye, prm, 12);
        for (int i = 0; i < 3; i++) prm[3 + i] = prm[i] + dir[i] * 1000.f;
        prm[6] = 0.0f;
        float want = S.fov > 10.f && S.fov < 170.f ? S.fov : 70.f;
        float set = 2.f * atanf(tanf(want * 0.5f * 0.0174532925f) / g_fovK) / 0.0174532925f;
        if (set < 10.f) set = 10.f; if (set > 150.f) set = 150.f;
        prm[7] = set; g_lastFovySet = set;
        g_lastFinalCam = GetTickCount64();
    }
    g_setOriOrig(cam);
}

static void CalibrateFov() {   // RE4's projection vs the fovy we asked for -> keep Minecraft's fov on screen
    if (g_lastFovySet <= 0 || GetTickCount64() - g_lastFinalCam > 200) return;
    uint8_t* gl = g_ppGlobal ? (uint8_t*)*g_ppGlobal : nullptr;
    if (!Readable(gl, 0x118)) return;
    float p[4][4]; memcpy(p, gl + 0xD8, 64);
    if (!(fabsf(p[3][2] + 1.f) < 1e-3f && p[1][1] > 0.1f)) return;
    float k = (1.f / p[1][1]) / tanf(g_lastFovySet * 0.5f * 0.0174532925f);
    if (!(k > 0.4f && k < 2.5f)) return;
    g_fovK = g_fovK * 0.9f + k * 0.1f;
    static int logged = 0;
    if (++logged == 120) BridgeLog("camera: RE4 shows %.1f deg for fovy %.1f (factor %.3f); Minecraft fov %.1f",
                                   2.f * atanf(1.f / p[1][1]) / 0.0174532925f, g_lastFovySet, (float)g_fovK, bridge::S().fov);
}

bool InstallFinalCamera(uint8_t* site) {   // site: "8D 53 60 8D BB 04 01 00 00 B9 08 00 00 00 BE ? ? ? ? 52 F3 A5 E8"
    if (!site) { BridgeLog("camera: final-camera call not found (camera stays on the shoulder-camera hook)"); return false; }
    uint8_t* call = site + 22;
    uint8_t* target = call + 5 + *(int32_t*)(call + 1);
    if (target[0] == 0xE9) target = target + 5 + *(int32_t*)(target + 1);   // incremental-link thunk
    MH_STATUS st = MH_CreateHook(target, (void*)&SetOriHook, (void**)&g_setOriOrig);
    if (st == MH_OK) st = MH_EnableHook(target);
    BridgeLog("camera: final camera (CameraSetOrientationRoll) at %p: %s", target, MH_StatusToString(st));
    return st == MH_OK;
}

// ------------------------------------------------------------------ overlay drawing
typedef HRESULT (WINAPI *PresentFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT (WINAPI *ResetFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
static PresentFn g_present = nullptr;
static ResetFn   g_reset = nullptr;
static IDirect3DTexture9* g_tex = nullptr;          // the overlay texture drawn this frame (one of g_ovl)
static IDirect3DTexture9* g_ovl[2] = {nullptr, nullptr};
static std::vector<uint32_t> g_ovlSpan[2];          // per texture and row: x0 | x1 << 16 of what it holds that isn't transparent
static int g_ovlCur = 0;
static uint32_t g_texW = 0, g_texH = 0;
static bool     g_haveFrame = false;
static bool     g_ovlRgba = false;                  // texture in Minecraft's own byte order: a straight copy, no per-pixel swap
static IDirect3DTexture9* g_cursor = nullptr;
static int64_t  g_ovlTicks = 0, g_blkTicks = 0; static uint64_t g_ovlPx = 0, g_ovlAllPx = 0;   // for the perf line

static void ReleaseTextures() {
    for (auto& t : g_ovl) if (t) { t->Release(); t = nullptr; }
    g_tex = nullptr;
    if (g_cursor) { g_cursor->Release(); g_cursor = nullptr; }
    g_texW = g_texH = 0; g_haveFrame = false;
}

__attribute__((target("ssse3"))) static uint32_t SwapRB4(const uint32_t* src, uint32_t* dst, uint32_t w) {
    const __m128i mask = _mm_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
    uint32_t x = 0;
    for (; x + 4 <= w; x += 4)
        _mm_storeu_si128((__m128i*)(dst + x), _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(src + x)), mask));
    return x;
}
static void CopyPixels(const uint32_t* src, uint32_t* dst, uint32_t n, bool rgba) {
    if (rgba) { memcpy(dst, src, (size_t)n * 4); return; }
    static int fast = -1;
    if (fast < 0) { __builtin_cpu_init(); fast = __builtin_cpu_supports("ssse3") ? 1 : 0; }
    uint32_t x = fast ? SwapRB4(src, dst, n) : 0;   // 4 pixels per instruction
    for (; x < n; x++) {
        uint32_t c = src[x];   // RGBA in memory = 0xAABBGGRR little-endian -> D3D wants 0xAARRGGBB
        dst[x] = (c & 0xFF00FF00) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
    }
}
// The part of a row that isn't fully transparent (Minecraft's overlay is premultiplied: transparent = 0).
__attribute__((target("sse2"))) static void RowSpan(const uint32_t* r, uint32_t w, uint32_t& x0, uint32_t& x1) {
    const __m128i z = _mm_setzero_si128();
    uint32_t a = 0;
    while (a + 4 <= w && _mm_movemask_epi8(_mm_cmpeq_epi32(_mm_loadu_si128((const __m128i*)(r + a)), z)) == 0xFFFF) a += 4;
    while (a < w && r[a] == 0) a++;
    if (a >= w) { x0 = x1 = 0; return; }
    uint32_t b = w;
    while (b >= a + 4 && _mm_movemask_epi8(_mm_cmpeq_epi32(_mm_loadu_si128((const __m128i*)(r + b - 4)), z)) == 0xFFFF) b -= 4;
    while (b > a && r[b - 1] == 0) b--;
    x0 = a; x1 = b;
}

// Minecraft's HUD and hand: most of the frame is transparent. Two textures take turns; each remembers which
// part of every row holds something, so a frame only writes what changed from transparent to visible or
// back (the GPU has finished with a texture by the time it comes round again: one frame in flight, see
// LowLatency). Without LowLatency each frame is written in full.
static void UploadOverlay(IDirect3DDevice9* dev) {
    uint32_t w, h; bool bottomUp; uint64_t id;
    const uint8_t* px = bridge::AcquireOverlay(w, h, bottomUp, id);
    if (!px) return;
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    if (!g_ovl[0] || g_texW != w || g_texH != h) {
        ReleaseTextures();
        g_ovlRgba = false;
        IDirect3D9* d3d = nullptr; D3DDEVICE_CREATION_PARAMETERS cp{}; D3DDISPLAYMODE dm{};
        if (SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d) {
            if (SUCCEEDED(dev->GetCreationParameters(&cp)) && SUCCEEDED(dev->GetDisplayMode(0, &dm)))
                g_ovlRgba = SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, dm.Format, D3DUSAGE_DYNAMIC, D3DRTYPE_TEXTURE, D3DFMT_A8B8G8R8));
            d3d->Release();
        }
        for (int pass = g_ovlRgba ? 0 : 1; pass < 2; pass++) {   // pass 0: Minecraft's byte order, 1: the usual one
            bool ok = true;
            for (int k = 0; k < 2 && ok; k++)
                ok = SUCCEEDED(dev->CreateTexture(w, h, 1, D3DUSAGE_DYNAMIC, pass ? D3DFMT_A8R8G8B8 : D3DFMT_A8B8G8R8, D3DPOOL_DEFAULT, &g_ovl[k], nullptr));
            if (ok) { g_ovlRgba = pass == 0; break; }
            for (auto& t : g_ovl) if (t) { t->Release(); t = nullptr; }
        }
        if (!g_ovl[0]) { BridgeLog("overlay: CreateTexture %ux%u failed", w, h); return; }
        for (int k = 0; k < 2; k++) g_ovlSpan[k].assign(h, (uint32_t)w << 16);   // contents unknown: the first write clears every row
        g_texW = w; g_texH = h;
        BridgeLog("overlay: %ux%u from Minecraft (%s, only the visible parts are copied)", w, h, g_ovlRgba ? "straight copy" : "colour swap");
    }
    bool keep = cfg::lowLatency;
    int k = keep ? (g_ovlCur ^ 1) : 0;
    D3DLOCKED_RECT lr;
    if (FAILED(g_ovl[k]->LockRect(0, &lr, nullptr, keep ? 0 : D3DLOCK_DISCARD))) return;
    std::vector<uint32_t>& sp = g_ovlSpan[k];
    uint64_t copied = 0;
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t* src = (const uint32_t*)(px + (size_t)(bottomUp ? h - 1 - y : y) * w * 4);
        uint32_t* dst = (uint32_t*)((uint8_t*)lr.pBits + (size_t)y * lr.Pitch);
        uint32_t nx0, nx1; RowSpan(src, w, nx0, nx1);
        uint32_t ox0 = keep ? (sp[y] & 0xFFFF) : 0, ox1 = keep ? (sp[y] >> 16) : w;
        if (nx1 > nx0) {
            if (ox1 > ox0) {   // clear what was visible before and isn't now
                if (ox0 < nx0) memset(dst + ox0, 0, (size_t)((ox1 < nx0 ? ox1 : nx0) - ox0) * 4);
                if (ox1 > nx1) { uint32_t c0 = ox0 > nx1 ? ox0 : nx1; memset(dst + c0, 0, (size_t)(ox1 - c0) * 4); }
            }
            CopyPixels(src + nx0, dst + nx0, nx1 - nx0, g_ovlRgba);
            copied += nx1 - nx0;
        } else if (ox1 > ox0) {
            memset(dst + ox0, 0, (size_t)(ox1 - ox0) * 4);
        }
        sp[y] = nx0 | (nx1 << 16);
    }
    g_ovl[k]->UnlockRect(0);
    g_ovlCur = k; g_tex = g_ovl[k];
    g_haveFrame = true;
    LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
    g_ovlTicks += t1.QuadPart - t0.QuadPart; g_ovlPx += copied; g_ovlAllPx += (uint64_t)w * h;
}

struct QuadV { float x, y, z, rhw, u, v; };

static void DrawQuad(IDirect3DDevice9* dev, IDirect3DTexture9* tex, float x0, float y0, float x1, float y1) {
    QuadV q[4] = {{x0 - .5f, y0 - .5f, 0, 1, 0, 0}, {x1 - .5f, y0 - .5f, 0, 1, 1, 0},
                  {x0 - .5f, y1 - .5f, 0, 1, 0, 1}, {x1 - .5f, y1 - .5f, 0, 1, 1, 1}};
    dev->SetTexture(0, tex);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadV));
}

static void EnsureCursor(IDirect3DDevice9* dev) {
    if (g_cursor) return;
    if (FAILED(dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_cursor, nullptr))) return;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(g_cursor->LockRect(0, &lr, nullptr, 0))) {
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                bool inside = x <= y && x + y / 2 < 12;           // simple arrow
                bool edge = inside && (x == 0 || x == y || x + y / 2 == 11);
                ((uint32_t*)((uint8_t*)lr.pBits + y * lr.Pitch))[x] = !inside ? 0 : edge ? 0xFF000000 : 0xFFFFFFFF;
            }
        g_cursor->UnlockRect(0);
    }
}

static IDirect3DStateBlock9* g_ovlSb = nullptr; static IDirect3DDevice9* g_ovlSbDev = nullptr;
static void DrawOverlay(IDirect3DDevice9* dev) {
    auto& S = bridge::S();
    bool show = S.puppet || (cfg::hudInCutscenes && S.busy && S.mcReady && S.everPuppet && !input::g_re4Control);
    if (getenv("R4NOOVL") || g_menuOpen) show = false;
    if (!show || !g_haveFrame || !g_tex) return;
    IDirect3DStateBlock9*& sb = g_ovlSb;
    if (sb && g_ovlSbDev != dev) { sb->Release(); sb = nullptr; }
    if (!sb) { if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) return; g_ovlSbDev = dev; }
    else sb->Capture();
    IDirect3DSurface9* oldRt = nullptr; IDirect3DSurface9* bb = nullptr;
    dev->GetRenderTarget(0, &oldRt);
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) dev->SetRenderTarget(0, bb);
    D3DSURFACE_DESC d{}; if (bb) bb->GetDesc(&d);
    D3DVIEWPORT9 vp = {0, 0, d.Width, d.Height, 0, 1};
    dev->SetViewport(&vp);
    if (SUCCEEDED(dev->BeginScene())) {
        dev->SetVertexShader(nullptr); dev->SetPixelShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);            // Minecraft's overlay is premultiplied
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        DrawQuad(dev, g_tex, 0, 0, (float)d.Width, (float)d.Height);
        if (S.mcScreenOpen) {
            EnsureCursor(dev);
            float sx = (float)d.Width / (g_texW ? g_texW : d.Width), sy = (float)d.Height / (g_texH ? g_texH : d.Height);
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            float cx = S.cursorX * sx, cy = S.cursorY * sy;
            if (g_cursor) DrawQuad(dev, g_cursor, cx, cy, cx + 16, cy + 16);
        }
        dev->EndScene();
    }
    if (oldRt) { dev->SetRenderTarget(0, oldRt); oldRt->Release(); }
    if (bb) bb->Release();
    sb->Apply();
}

// Frame timing for the log: how long RE4's frames take and how much of that is RECraft's own work, with the
// main parts: copying Minecraft's HUD in (overlay), drawing Minecraft's world (blocks) and the camera hook
// (enemies, hits, health - it runs inside RE4's own frame, before Present).
static void PerfTick(LARGE_INTEGER t0, LARGE_INTEGER t1) {
    static LARGE_INTEGER f{}, last{}; static double ours = 0, worst = 0, sum = 0; static int n = 0, slow = 0, quick = 0; static ULONGLONG at = 0;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    if (last.QuadPart) {
        double frame = (t0.QuadPart - last.QuadPart) * 1000.0 / f.QuadPart;
        sum += frame; n++; ours += ms; if (frame > worst) worst = frame; if (frame > 25.0) slow++; if (frame < 12.0) quick++;
    }
    last = t0;
    ULONGLONG now = GetTickCount64();
    if (!at) at = now;
    if (now - at > 60000 && n) {
        double k = 1000.0 / f.QuadPart / n;
        BridgeLog("perf: %.1f fps average, RECraft %.2f ms of each %.1f ms frame (overlay %.2f, blocks %.2f) + camera hook %.2f ms, %d frames over 25 ms (worst %.0f ms) and %d under 12 ms; overlay copied %.0f%% of its pixels",
                  1000.0 * n / sum, ours / n, sum / n, g_ovlTicks * k, g_blkTicks * k, g_camTicks * k, slow, worst, quick,
                  g_ovlAllPx ? 100.0 * g_ovlPx / g_ovlAllPx : 0.0);
        {   int tn, late, snaps; double tavg, tworst; bridge::TickStats(tn, tavg, tworst, late, snaps);
            if (tn) BridgeLog("perf: Minecraft ticks every %.1f ms on average (should be 50), worst %.0f ms, %d late (over 75 ms); Leon pulled back %d times", tavg, tworst, late, snaps); }
        if (slow > 60 && quick > slow / 2) BridgeLog("perf: slow frames come with short ones - a refresh beat, not overload: HalfRateVsync=1 should even it out");
        at = now; ours = worst = sum = 0; n = slow = quick = 0;
        g_ovlTicks = g_blkTicks = g_camTicks = 0; g_ovlPx = g_ovlAllPx = 0;
    }
}

// Input lag: the driver lets the CPU run up to 3 frames ahead of what's on screen, so a mouse move or a key
// press shows up 2-3 frames late. After each Present we wait until the GPU has finished the frame before
// (one frame in flight, like "low latency mode" in the GPU control panels).
static IDirect3DQuery9* g_llq[2] = {nullptr, nullptr};
static IDirect3DDevice9* g_llDev = nullptr;
static unsigned g_llFrame = 0;
static void LowLatencyRelease() { for (auto& q : g_llq) if (q) { q->Release(); q = nullptr; } g_llDev = nullptr; }
static void LowLatency(IDirect3DDevice9* dev) {
    if (!cfg::lowLatency) return;
    if (g_llDev != dev) {
        LowLatencyRelease();
        if (FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_llq[0])) || FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_llq[1]))) {
            LowLatencyRelease(); static bool said = false; if (!said) { said = true; BridgeLog("latency: GPU event queries unavailable - frame queue left as is"); } return;
        }
        g_llDev = dev; g_llFrame = 0;
        BridgeLog("latency: at most one frame queued ahead of the screen");
    }
    IDirect3DQuery9* cur = g_llq[g_llFrame & 1];
    IDirect3DQuery9* prev = g_llq[(g_llFrame + 1) & 1];
    cur->Issue(D3DISSUE_END);
    if (g_llFrame > 0) {
        LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        while (prev->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
            QueryPerformanceCounter(&t);
            int64_t us = (t.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart;
            if (us > 50000) break;   // never stall a frame for long
            if (us > 500) SwitchToThread(); else YieldProcessor();   // a longer wait leaves the core to others (Minecraft)
        }
    }
    g_llFrame++;
}

static HRESULT WINAPI PresentHook(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    mem::Reset();
    {   static IDirect3DDevice9* said = nullptr;   // which GPU RE4 draws with
        if (said != dev) {
            said = dev; IDirect3D9* d3d = nullptr; D3DDEVICE_CREATION_PARAMETERS cp{}; D3DADAPTER_IDENTIFIER9 id{};
            if (SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d) {
                if (SUCCEEDED(dev->GetCreationParameters(&cp)) && SUCCEEDED(d3d->GetAdapterIdentifier(cp.AdapterOrdinal, 0, &id)))
                    BridgeLog("display: RE4 draws with %s (adapter %u of %u)", id.Description, cp.AdapterOrdinal, d3d->GetAdapterCount());
                d3d->Release();
            }
        }
    }
    ::g_lastPresent = GetTickCount64();
    IDirect3DSurface9* bb = nullptr; D3DSURFACE_DESC desc{}; desc.Width = 1280; desc.Height = 720;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) { bb->GetDesc(&desc); bb->Release(); }
    g_presentsSinceHit++;
    {   // Minecraft's window appears for a moment when it starts and takes the focus: hand it back to RE4
        static bool wasAlive = false; static ULONGLONG connectAt = 0; static bool done = false;
        bool alive = bridge::McAlive();
        if (alive && !wasAlive) { connectAt = GetTickCount64(); done = false; }
        wasAlive = alive;
        ULONGLONG since = connectAt ? GetTickCount64() - connectAt : 0;
        if (alive && !done && since > 2500) {
            D3DDEVICE_CREATION_PARAMETERS cp{};
            HWND me = SUCCEEDED(dev->GetCreationParameters(&cp)) ? cp.hFocusWindow : nullptr;
            HWND fg = GetForegroundWindow();
            if (!me || fg == me || since > 15000) done = true;
            else {
                DWORD fgT = GetWindowThreadProcessId(fg, nullptr), myT = GetCurrentThreadId();
                if (fgT && fgT != myT) AttachThreadInput(myT, fgT, TRUE);
                if (IsIconic(me)) ShowWindow(me, SW_RESTORE);
                SetForegroundWindow(me); SetFocus(me);
                if (fgT && fgT != myT) AttachThreadInput(myT, fgT, FALSE);
                done = GetForegroundWindow() == me;
                BridgeLog("display: Minecraft took the focus - giving it back to RE4 (%s)", done ? "ok" : "will retry");
            }
        }
    }
    bridge::Leon leon = ReadLeon();
    bool busy = GameBusy(leon);
    g_menuOpen = SubScreenOpen();
    {   // RE4 halts (nearly) everything for its full-screen menus that aren't sub screens: the chapter results, the
        // typewriter's save screen, the pause / options menu, "Continue?". 0.34 drew the body over the results.
        bool frozen = __builtin_popcount(g_stopFlags) >= 24;
        static bool wasFrozen = false;
        if (frozen != wasFrozen) { BridgeLog("menu: RE4 %s (stop %08X)", frozen ? "has stopped the game world (results / save / pause screen) - Minecraft's body and blocks hidden" : "runs again", g_stopFlags); wasFrozen = frozen; }
        if (frozen) g_menuOpen = true;
    }
    // OverlayHalfRes: Minecraft draws its HUD at half the size (GUI scale halves with it) and it's shown 2x:
    // the same picture for the HUD, a quarter of the pixels to copy each frame
    bridge::Frame(leon, input::g_re4Control, busy, cfg::overlayHalf ? desc.Width / 2 : desc.Width, cfg::overlayHalf ? desc.Height / 2 : desc.Height);
    auto& S = bridge::S();
    {   // the Minecraft player's body stands in for Leon while RE4 moves him (cutscenes, vaults, ladders...)
        static bool was = false;
        uint8_t* pl = Player();
        uint8_t* evl = busy ? EventLeon() : nullptr;   // a story cutscene's own Leon: hide it too, the body follows it
        static uint8_t* lastEvl = nullptr;
        if (evl != lastEvl) { BridgeLog("cutscene: %s", evl ? "it has its own Leon - hidden too, Minecraft body follows it" : "event Leon gone"); lastEvl = evl; }
        // RE4 itself moves Leon (QTEs, grabs, vaults, ladders, his death) or a cutscene has its own Leon: the
        // Minecraft body stands in. Not for pause / options menus; Leon's death counts even though it raises a menu flag.
        // Leon's death: the body plays it out for its first seconds, then the red "You are dead" screen comes up over the
        // scene and nothing of Minecraft's belongs on top of it (we draw after RE4, so it would cover the screen).
        static ULONGLONG dieAt = 0;
        ULONGLONG nowT = GetTickCount64();
        if (!g_dieDemo) dieAt = 0; else if (!dieAt) dieAt = nowT;
        bool dieEarly = g_dieDemo && nowT - dieAt < 2200;
        bool actor = busy && !S.puppet && (!g_menuOpen || dieEarly) && (g_leonAction || evl || dieEarly);
        bool show = false;
        float parts[7][3][4];
        // a cutscene's own Leon that is going away has no skeleton any more: use the real Leon, or nothing
        bool posed = pl && actor && blocks::HaveAvatar() &&
                     (PoseFromLeon(evl ? evl : pl, parts) || (evl && PoseFromLeon(pl, parts)));
        static int nearState = 0, holdFrames = 18, actorFrames = 0;
        actorFrames = actor ? actorFrames + 1 : 0;
        float eye[3], nearest = 1e9f, head = 1e9f;
        if (posed && blocks::CameraEye(eye)) head = blocks::AvatarDistance(parts, eye, &nearest);
        bool blank = false;
        if (!cfg::avatarInCutscenes) {
            // 0.37 default: RE4's own Leon plays every cutscene and RE4 move (the drawn-on-top body covered QTE prompts,
            // swapped with Leon by camera distance and went missing in close shots). Only while RE4's camera is still
            // inside him - it starts from Minecraft's eye when RE4 takes Leon over - is he hidden, for those few frames.
            static bool inside = false;
            float lim = inside ? 300.f : (actorFrames < 60 ? 220.f : 100.f);
            bool now = posed && nearest < lim;
            if (now != inside) { static int logs = 0; if (logs++ < 40) BridgeLog("avatar: camera %s Leon (%.0f mm)", now ? "inside" : "clear of", nearest); inside = now; }
            blank = now;
        } else {
            // MinecraftBodyInCutscenes=1: the Minecraft body stands in for wider shots, RE4's Leon for close shots,
            // with some hysteresis and at most one switch every 0.3 s; the first frames show nobody.
            show = posed;
            bool nearShot = false;
            if (show) {
                bool want = nearState ? (head < 2100.f || nearest < 300.f) : (head < 1700.f || nearest < 220.f);
                if (++holdFrames > 100000) holdFrames = 100000;
                if (want != (nearState != 0) && (holdFrames >= 18 || (want && nearest < 120.f))) {
                    nearState = want; holdFrames = 0;
                    static int logs = 0;
                    if (logs++ < 40) BridgeLog("avatar: %s shot (camera %.1f m from the head) - %s", want ? "close" : "wide", head / 1000.f, want ? "RE4's Leon" : "Minecraft body");
                }
                nearShot = nearState != 0;
            } else { nearState = 0; holdFrames = 18; }
            if (nearShot) show = false;
            blank = nearShot && actorFrames < 10 && !evl;
        }
        g_leonNeeded = actor && !show && !blank;
        static uint8_t* evlZeroed = nullptr;
        bool standIn = show || blank;
        if (evl && standIn && g_wantHidden && cfg::hideLeonInCutscenes) { float z = 0.f; memcpy(evl + 0x154, &z, 4); evlZeroed = evl; }   // cModel::invisible_factor
        else if (evl && evl == evlZeroed) { float o = 1.f; memcpy(evl + 0x154, &o, 4); evlZeroed = nullptr; }
        blocks::SetAvatar(show, show ? parts : nullptr);
        g_avatarOn = show;
        static bool wasNeeded = false;
        if (g_leonNeeded != wasNeeded) { BridgeLog("avatar: %s", g_leonNeeded ? "the Minecraft body can't stand in here - RE4's Leon shown" : "Leon hidden again"); wasNeeded = g_leonNeeded; }
        if (show != was) BridgeLog("avatar: Minecraft body %s", show ? "shown in Leon's place" : "hidden");
        was = show;
        // RE4's own Leon plays a story cutscene, a grab or a QTE: Minecraft's blocks, items and arrows aren't drawn
        // either - they're drawn over RE4's finished picture, so they'd cover the characters and the button prompts
        g_sceneClean = actor && !show && (evl || g_scripted);
        static bool wasClean = false;
        if (g_sceneClean != wasClean) { BridgeLog("scene: %s", g_sceneClean ? "RE4's own show - nothing of Minecraft's drawn over it" : "Minecraft's blocks drawn again"); wasClean = g_sceneClean; }
    }
    if (g_presentsSinceHit > 10 && bridge::McAlive()) combat::Tick(bridge::View(), false);   // camera hook idle: keep the hit queue drained
    // (Minecraft's player being dead - it dies with Leon - doesn't bring Leon back: his death plays as the Minecraft body)
    g_wantHidden = cfg::hideLeon && leon.valid && bridge::McAlive() && S.everPuppet && !input::g_re4Control &&
                   (!busy || cfg::hideLeonInCutscenes) && !g_leonNeeded;
    HideLeonTick(false);
    CalibrateFov();
    items::Tick(g_menuOpen);   // Minecraft items on the floor turn slowly (not while RE4 shows one up close)
    // Leon's death: Minecraft's world and body only while the body plays the death - not over "You are dead" (0.33 drew
    // the blocks on top of the red screen)
    if ((!g_menuOpen && !g_sceneClean) || (g_dieDemo && g_avatarOn)) { LARGE_INTEGER b0, b1; QueryPerformanceCounter(&b0); blocks::Draw(dev, bridge::McAlive()); QueryPerformanceCounter(&b1); g_blkTicks += b1.QuadPart - b0.QuadPart; }
    Re4HudTick(S.everPuppet && bridge::McAlive() && !input::g_re4Control && !g_boat);
    // the radio (Hunnigan's video call): the Minecraft player's face in Leon's "out going image" panel
    if (g_menuOpen && (g_subType & 0x20) && S.everPuppet && !input::g_re4Control) blocks::DrawPortrait(dev, 0.699f, 0.148f, 0.921f, 0.619f);
    UploadOverlay(dev);
    DrawOverlay(dev);
    { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); PerfTick(t0, t1); }
    HRESULT hr = g_present(dev, a, b, c, d);
    LowLatency(dev);
    return hr;
}

// RE4 runs at 60 fps. On a 144 Hz screen with V-Sync, 60 frames don't fit evenly into 144 refreshes:
// frames alternate between 2 and 3 refreshes (14 / 21 ms, sometimes 28 ms) - a constant judder that feels
// like lag. In fullscreen we pick the same resolution at a multiple of 60 Hz instead (120, else 60).
static void EvenRefresh(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (!cfg::evenRefresh || !pp || pp->Windowed || pp->FullScreen_RefreshRateInHz == 0 || pp->FullScreen_RefreshRateInHz % 60 == 0) return;
    IDirect3D9* d3d = nullptr;
    if (FAILED(dev->GetDirect3D(&d3d)) || !d3d) return;
    D3DFORMAT fmt = pp->BackBufferFormat == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : pp->BackBufferFormat;
    if (fmt == D3DFMT_A8R8G8B8) fmt = D3DFMT_X8R8G8B8;   // display modes are enumerated without alpha
    UINT best = 0, n = d3d->GetAdapterModeCount(D3DADAPTER_DEFAULT, fmt);
    for (UINT i = 0; i < n; i++) {
        D3DDISPLAYMODE m;
        if (FAILED(d3d->EnumAdapterModes(D3DADAPTER_DEFAULT, fmt, i, &m))) continue;
        if (m.Width != pp->BackBufferWidth || m.Height != pp->BackBufferHeight || m.RefreshRate % 60 != 0) continue;
        if (m.RefreshRate <= 120 && m.RefreshRate > best) best = m.RefreshRate;
    }
    d3d->Release();
    if (best) {
        BridgeLog("display: %u Hz doesn't fit 60 fps evenly (judder) - using %u Hz", pp->FullScreen_RefreshRateInHz, best);
        pp->FullScreen_RefreshRateInHz = best;
    } else {
        BridgeLog("display: %u Hz doesn't fit 60 fps evenly, and the screen offers no 60/120 Hz mode at %ux%u", pp->FullScreen_RefreshRateInHz, pp->BackBufferWidth, pp->BackBufferHeight);
    }
}

// RE4 caps itself at 60 fps (re4_tweaks' frame limiter) and presents with V-Sync on every refresh: at 120 Hz
// the two clocks drift against each other and frames land 1 or 3 refreshes apart instead of 2 (the 0.30 logs
// counted ~470 frames a minute over 25 ms at a 60 fps average). Presenting every 2nd refresh lets V-Sync
// alone pace the game: an even 16.7 ms.
static bool g_vsyncHalved = false;   // we changed RE4's own settings struct: undo it when it no longer applies
static void VsyncHalfRate(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (!pp) return;
    if (g_vsyncHalved && pp->PresentationInterval == D3DPRESENT_INTERVAL_TWO) pp->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    g_vsyncHalved = false;
    UINT was = pp->PresentationInterval;
    if (!cfg::vsyncHalfRate || pp->Windowed || pp->FullScreen_RefreshRateInHz != 120 ||
        (was != D3DPRESENT_INTERVAL_ONE && was != D3DPRESENT_INTERVAL_DEFAULT)) {
        BridgeLog("display: present interval 0x%X kept", was); return;
    }
    D3DCAPS9 caps{};
    if (FAILED(dev->GetDeviceCaps(&caps)) || !(caps.PresentationIntervals & D3DPRESENT_INTERVAL_TWO)) {
        BridgeLog("display: the driver can't present every 2nd refresh - V-Sync left as is"); return;
    }
    pp->PresentationInterval = D3DPRESENT_INTERVAL_TWO; g_vsyncHalved = true;
    BridgeLog("display: 120 Hz - presenting every 2nd refresh (HalfRateVsync=1)");
}

static HRESULT WINAPI ResetHook(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    EvenRefresh(dev, pp);
    VsyncHalfRate(dev, pp);
    if (pp) BridgeLog("display: RE4 reset its display to %ux%u %s, %u Hz", pp->BackBufferWidth, pp->BackBufferHeight, pp->Windowed ? "windowed" : "fullscreen", pp->FullScreen_RefreshRateInHz);
    ReleaseTextures();   // D3DPOOL_DEFAULT textures must go before Reset
    if (g_ovlSb) { g_ovlSb->Release(); g_ovlSb = nullptr; }   // state blocks must go before Reset too
    blocks::OnReset();
    LowLatencyRelease();
    return g_reset(dev, pp);
}

static LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

bool Install(uint8_t* cameraCallSite) {
    if (cameraCallSite) InstallCameraHook(cameraCallSite);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { BridgeLog("overlay: Direct3DCreate9 failed"); return false; }
    WNDCLASSA wc{}; wc.lpfnWndProc = DummyProc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "RECraft_d3d";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("RECraft_d3d", "", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    D3DPRESENT_PARAMETERS pp{}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd;
    pp.BackBufferFormat = D3DFMT_UNKNOWN; pp.BackBufferWidth = 64; pp.BackBufferHeight = 64;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    bool ok = false;
    if (SUCCEEDED(hr) && dev) {
        void** vt = *(void***)dev;
        MH_STATUS a = MH_CreateHook(vt[17], (void*)&PresentHook, (void**)&g_present);
        MH_STATUS r = MH_CreateHook(vt[16], (void*)&ResetHook, (void**)&g_reset);
        if (a == MH_OK) MH_EnableHook(vt[17]);
        if (r == MH_OK) MH_EnableHook(vt[16]);
        BridgeLog("overlay: Present hook %s, Reset hook %s", MH_StatusToString(a), MH_StatusToString(r));
        ok = a == MH_OK;
        dev->Release();
    } else {
        BridgeLog("overlay: dummy device failed (0x%08lX)", hr);
    }
    d3d->Release();
    DestroyWindow(hwnd);
    return ok;
}

} // namespace render
