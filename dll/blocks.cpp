#include <algorithm>
// blocks.cpp - draws Minecraft's world (placed blocks, dropped items, mobs, arrows, particles) inside
// RE4's frame. SkyCraft's Minecraft mod streams its own block meshes, atlas and entity geometry
// through the render ring; we draw them with RE4's camera matrices, depth-tested against RE4's
// depth buffer so RE4's walls hide blocks behind them.
// Runs on RE4's render thread (Present hook).

#include "link.h"
#include <d3d9.h>
#include <vector>
#include <map>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace render { extern float g_camEye[3]; }
namespace collision { bool Occluders(uint32_t& version, uint16_t& room, std::vector<float>& out); }
namespace combat { void EnemyPositions(std::vector<float>& out); }
namespace blocks {

constexpr uint64_t kOffRender = 0x20000ull + (32ull << 20) + 3840ull * 2160 * 4 * 3;
constexpr uint64_t kRenderBytes = 64ull << 20;
constexpr uint64_t kRenData = kRenderBytes - 0x80;

uint8_t*** g_ppGlobal = nullptr;
bool g_depthTest = true;

#pragma pack(push, 1)
struct RenVertex { float x, y, z, u, v; uint32_t color, light, flags; };
struct RenBatch { uint32_t texture, first, count, flags; };
struct RenScene { double ox, oy, oz; uint32_t batchCount, vertexCount; };
#pragma pack(pop)

struct Vtx { float x, y, z; uint32_t color; float u, v; };   // D3DFVF_XYZ | DIFFUSE | TEX1
static const DWORD kFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

struct Section { std::vector<Vtx> solid, translucent; };
static std::map<int64_t, Section> g_sections;
struct Batch { uint32_t tex, first, count; bool translucent; };
static std::vector<Vtx> g_sceneV; static std::vector<Batch> g_sceneB;
// The Minecraft player's body (SkyCraft's ragdoll message: standing, facing +Z, feet at the origin,
// in Minecraft's six parts) - drawn where Leon is during cutscenes.
struct AvBatch { uint32_t tex, first, count; int part; bool translucent; };
static std::vector<Vtx> g_avV; static std::vector<AvBatch> g_avB;
static bool g_avShow = false; static float g_avPart[7][3][4];   // per part: world = M * model vertex (mm)
void SetAvatar(bool show, const float parts[7][3][4]) { g_avShow = show; if (parts) memcpy(g_avPart, parts, sizeof g_avPart); }
bool HaveAvatar() { return !g_avB.empty(); }

static uint8_t* g_ring = nullptr;
static IDirect3DDevice9* g_dev = nullptr;
static IDirect3DTexture9* g_atlas = nullptr;
static uint32_t g_atlasW = 0, g_atlasH = 0;
static std::vector<uint8_t> g_atlasCpu;   // kept to rebuild after a device reset
static std::map<uint32_t, IDirect3DTexture9*> g_textures;
static std::map<uint32_t, std::vector<uint8_t>> g_texturesCpu; static std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_texSize;

static int64_t Key(int x, int y, int z) { return ((int64_t)(x & 0x1FFFFF) << 42) | ((int64_t)(y & 0x1FFFFF) << 21) | (int64_t)(z & 0x1FFFFF); }

// Minecraft's fixed face shading (left out of the mesh) and its light levels, as a colour multiplier
static uint32_t Shade(uint32_t rgba, uint32_t light, uint32_t flags) {
    static const float kFace[7] = {1.0f, 0.5f, 1.0f, 0.8f, 0.8f, 0.6f, 0.6f};   // none, down, up, N, S, W, E
    int face = (flags >> 4) & 7;
    float bl = (light & 0xFF) / 15.f, sl = ((light >> 8) & 0xFF) / 15.f;
    float lit = 0.3f + 0.7f * fmaxf(bl, sl * 0.6f);    // RE4 is a night game: sky light counts for less
    float k = (face < 7 ? kFace[face] : 1.f) * lit;
    uint32_t r = (uint32_t)fminf(255.f, (rgba & 0xFF) * k), g = (uint32_t)fminf(255.f, ((rgba >> 8) & 0xFF) * k),
             b = (uint32_t)fminf(255.f, ((rgba >> 16) & 0xFF) * k), a = (rgba >> 24) & 0xFF;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static IDirect3DTexture9* MakeTexture(uint32_t w, uint32_t h, const uint8_t* rgba) {
    if (!g_dev) return nullptr;
    IDirect3DTexture9* t = nullptr;
    if (FAILED(g_dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) {
        BridgeLog("blocks: CreateTexture %ux%u failed", w, h); return nullptr;
    }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(t->LockRect(0, &lr, nullptr, 0))) {
        for (uint32_t y = 0; y < h; y++) {
            const uint32_t* s = (const uint32_t*)(rgba + (size_t)y * w * 4);
            uint32_t* d = (uint32_t*)((uint8_t*)lr.pBits + (size_t)y * lr.Pitch);
            for (uint32_t x = 0; x < w; x++) { uint32_t c = s[x]; d[x] = (c & 0xFF00FF00) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF); }
        }
        t->UnlockRect(0);
    }
    return t;
}

static void Convert(const RenVertex* v, uint32_t n, double ox, double oy, double oz, std::vector<Vtx>& solid, std::vector<Vtx>* translucent) {
    for (uint32_t i = 0; i + 2 < n + 0 && i < n; i += 3) {
        bool tr = translucent && (v[i].flags & 2);
        std::vector<Vtx>& out = tr ? *translucent : solid;
        for (int k = 0; k < 3; k++) {
            const RenVertex& s = v[i + k];
            out.push_back({(float)((ox + s.x) * bridge::kUnitsPerBlock), (float)((oy + s.y) * bridge::kUnitsPerBlock),
                           (float)((oz + s.z) * bridge::kUnitsPerBlock), Shade(s.color, s.light, s.flags), s.u, s.v});
        }
    }
}

// Drain the render ring (called each frame before drawing).
static void Drain() {
    if (!g_ring) {
        HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"Local\\SkyCraft_v1");
        if (!map) return;
        SYSTEM_INFO si; GetSystemInfo(&si);
        uint64_t aligned = kOffRender & ~(uint64_t)(si.dwAllocationGranularity - 1), lead = kOffRender - aligned;
        uint8_t* view = (uint8_t*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, (DWORD)(aligned >> 32), (DWORD)aligned, (SIZE_T)(kRenderBytes + lead));
        CloseHandle(map);
        if (!view) {
            static bool once = false;
            if (!once) { once = true; BridgeLog("blocks: render ring map failed (%lu)", GetLastError()); }
            return;
        }
        g_ring = view + lead;
        BridgeLog("blocks: render ring mapped");
    }
    volatile uint64_t* head = (volatile uint64_t*)g_ring;
    volatile uint64_t* tail = (volatile uint64_t*)(g_ring + 0x40);
    uint64_t t = *tail, h = *head;
    int budget = 64;   // messages per frame (sections are small; the atlas is one big one)
    while (t < h && budget-- > 0) {
        uint64_t pos = t % kRenData;
        const uint8_t* m = g_ring + 0x80 + pos;
        uint32_t type, n; memcpy(&type, m, 4); memcpy(&n, m + 4, 4);
        if (type == 0) { t += kRenData - pos; continue; }
        if (pos + 8 + (uint64_t)n > kRenData || t + 8 + n > h) {   // a message that can't be right: skip what's there
            static int bad = 0; if (bad++ < 3) BridgeLog("blocks: render ring message %u of %u bytes at %llu doesn't fit - skipped", type, n, (unsigned long long)pos);
            t = h; break;
        }
        const uint8_t* p = m + 8;
        switch (type) {
        case 1: {   // atlas
            uint32_t w, hh; memcpy(&w, p, 4); memcpy(&hh, p + 4, 4);
            if (w && hh && (uint64_t)w * hh * 4 + 8 <= n) {
                g_atlasCpu.assign(p + 8, p + 8 + (size_t)w * hh * 4); g_atlasW = w; g_atlasH = hh;
                if (g_atlas) { g_atlas->Release(); g_atlas = nullptr; }
                g_atlas = MakeTexture(w, hh, g_atlasCpu.data());
                BridgeLog("blocks: atlas %ux%u", w, hh);
            }
            break;
        }
        case 7: {   // atlas region (animated textures: water, lava, fire...)
            uint32_t r[4]; memcpy(r, p, 16);
            if (!g_atlasCpu.empty() && (uint64_t)r[0] + r[2] <= g_atlasW && (uint64_t)r[1] + r[3] <= g_atlasH && 16 + (uint64_t)r[2] * r[3] * 4 <= n) {
                for (uint32_t y = 0; y < r[3]; y++)
                    memcpy(&g_atlasCpu[((size_t)(r[1] + y) * g_atlasW + r[0]) * 4], p + 16 + (size_t)y * r[2] * 4, (size_t)r[2] * 4);
                if (g_atlas) {
                    D3DLOCKED_RECT lr; RECT rc = {(LONG)r[0], (LONG)r[1], (LONG)(r[0] + r[2]), (LONG)(r[1] + r[3])};
                    if (SUCCEEDED(g_atlas->LockRect(0, &lr, &rc, 0))) {
                        for (uint32_t y = 0; y < r[3]; y++) {
                            const uint32_t* s = (const uint32_t*)(p + 16 + (size_t)y * r[2] * 4);
                            uint32_t* d = (uint32_t*)((uint8_t*)lr.pBits + (size_t)y * lr.Pitch);
                            for (uint32_t x = 0; x < r[2]; x++) { uint32_t c = s[x]; d[x] = (c & 0xFF00FF00) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF); }
                        }
                        g_atlas->UnlockRect(0);
                    }
                }
            }
            break;
        }
        case 2: {   // section mesh
            int32_t s[3]; uint32_t vc; memcpy(s, p, 12); memcpy(&vc, p + 12, 4);
            int64_t k = Key(s[0], s[1], s[2]);
            if (vc == 0 || 16 + (uint64_t)vc * 32 > n) { g_sections.erase(k); break; }
            Section sec;
            Convert((const RenVertex*)(p + 16), vc, s[0] * 16.0, s[1] * 16.0, s[2] * 16.0, sec.solid, &sec.translucent);
            g_sections[k] = std::move(sec);
            break;
        }
        case 3:     // clear all (world change)
            g_sections.clear();
            break;
        case 4: {   // entity texture
            uint32_t hdr[4]; memcpy(hdr, p, 16);
            if (hdr[1] && hdr[2] && 16 + (uint64_t)hdr[1] * hdr[2] * 4 <= n) {
                auto it = g_textures.find(hdr[0]);
                if (it != g_textures.end() && it->second) it->second->Release();
                g_texturesCpu[hdr[0]].assign(p + 16, p + 16 + (size_t)hdr[1] * hdr[2] * 4);
                g_texSize[hdr[0]] = {hdr[1], hdr[2]};
                g_textures[hdr[0]] = MakeTexture(hdr[1], hdr[2], p + 16);
            }
            break;
        }
        case 6: {   // scene: mobs, items, arrows, particles (replaces last frame's)
            RenScene sc; memcpy(&sc, p, sizeof sc);
            const RenBatch* b = (const RenBatch*)(p + sizeof sc);
            const RenVertex* v = (const RenVertex*)(p + sizeof sc + sc.batchCount * sizeof(RenBatch));
            if (sizeof sc + (uint64_t)sc.batchCount * sizeof(RenBatch) + (uint64_t)sc.vertexCount * 32 > n) break;
            g_sceneV.clear(); g_sceneB.clear();
            Convert(v, sc.vertexCount, sc.ox, sc.oy, sc.oz, g_sceneV, nullptr);
            for (uint32_t i = 0; i < sc.batchCount; i++)
                if (b[i].first + b[i].count <= g_sceneV.size())
                    g_sceneB.push_back({b[i].texture, b[i].first, b[i].count, (b[i].flags & 1) != 0});
            break;
        }
        case 9: {   // ragdoll: the player's body in parts (refreshed about once a second)
            uint32_t hdr2[2]; memcpy(hdr2, p, 8);
            const RenBatch* b = (const RenBatch*)(p + 8);
            const RenVertex* v = (const RenVertex*)(p + 8 + hdr2[0] * sizeof(RenBatch));
            if (!hdr2[0] || 8 + (uint64_t)hdr2[0] * sizeof(RenBatch) + (uint64_t)hdr2[1] * 32 > n) break;
            std::vector<Vtx> av; std::vector<AvBatch> ab;
            Convert(v, hdr2[1], 0, 0, 0, av, nullptr);
            for (uint32_t i = 0; i < hdr2[0]; i++)
                if (b[i].first + b[i].count <= av.size()) ab.push_back({b[i].texture, b[i].first, b[i].count, (int)((b[i].flags >> 8) & 15), (b[i].flags & 1) != 0});
            if (g_avB.empty()) BridgeLog("blocks: Minecraft player body received (%u parts, %u vertices)", hdr2[0], hdr2[1]);
            g_avV.swap(av); g_avB.swap(ab);
            break;
        }
        default:    // avatar, lights, solids, dug: not used on RE4's side yet
            break;
        }
        t += (8 + n + 7) & ~7ull;
    }
    *tail = t;
}

// ------------------------------------------------------------------ world entities (SkyCraft's table @0x1C000)
// Minecraft leaves some things out of the scene mesh for the game to draw itself: the outline of the targeted
// block (or where a held block would go), the cracks on a block being broken, dropped items and blocks (what a
// broken block drops), arrows, and thrown items (ender pearls, fire charges). 0.31 and earlier never read this
// table, so building and breaking had no outline, no crack progress, and drops/arrows were invisible.
#pragma pack(push, 1)
struct WEnt { uint32_t kind, id; float x, y, z, yaw, pitch, scale, ext[3], uv[3][4]; uint32_t tint; };
#pragma pack(pop)
static_assert(sizeof(WEnt) == 96, "WorldEntity layout");
static WEnt  g_we[160]; static int g_weN = 0;
static bool  g_sel = false; static float g_selBox[6];
static void ReadWorldEntities() {
    uint8_t* v = bridge::View();
    if (!v) { g_weN = 0; g_sel = false; return; }
    uint8_t* b = v + 0x1C000;
    for (int tries = 0; tries < 4; tries++) {
        uint32_t s1 = *(volatile uint32_t*)b;
        if (s1 & 1) { YieldProcessor(); continue; }   // Minecraft is writing it
        MemoryBarrier();
        uint32_t n = *(volatile uint32_t*)(b + 4); if (n > 160) n = 160;
        uint32_t hs = *(volatile uint32_t*)(b + 8);
        float sel[6]; memcpy(sel, b + 0xC, 24);
        static WEnt tmp[160]; memcpy(tmp, b + 0x40, (size_t)n * sizeof(WEnt));
        MemoryBarrier();
        if (*(volatile uint32_t*)b != s1) continue;
        memcpy(g_we, tmp, (size_t)n * sizeof(WEnt)); g_weN = (int)n;
        g_sel = hs != 0; memcpy(g_selBox, sel, 24);
        return;
    }   // torn every time: keep last frame's
}

static std::vector<Vtx> g_weV;
static void WQuad(std::vector<Vtx>& o, const float c[4][3], const float* uv, uint32_t col) {
    // corners: 0 top-left, 1 top-right, 2 bottom-right, 3 bottom-left (uv {u0, v0, u1, v1}: v0 = top)
    float u[4] = {uv[0], uv[2], uv[2], uv[0]}, w[4] = {uv[1], uv[1], uv[3], uv[3]};
    static const int ix[6] = {0, 1, 2, 0, 2, 3};
    for (int k : ix) o.push_back({c[k][0] * 1000.f, c[k][1] * 1000.f, c[k][2] * 1000.f, col, u[k], w[k]});
}
static uint32_t Tinted(uint32_t base, uint32_t rgba) {   // SkyCraft's tint is RGBA8 (r in the low byte); 0 = none
    if (!rgba) return base;
    uint32_t r = ((base >> 16) & 0xFF) * (rgba & 0xFF) / 255, g = ((base >> 8) & 0xFF) * ((rgba >> 8) & 0xFF) / 255, b = (base & 0xFF) * ((rgba >> 16) & 0xFF) / 255;
    return (base & 0xFF000000) | (r << 16) | (g << 8) | b;
}
// a box's six faces; rot (radians about the vertical) turns it about its centre
static void WBox(std::vector<Vtx>& o, const float lo[3], const float hi[3], float rot, const float* side, const float* top, const float* bottom, uint32_t col, uint32_t topCol) {
    float cx = (lo[0] + hi[0]) * 0.5f, cz = (lo[2] + hi[2]) * 0.5f, cs = cosf(rot), sn = sinf(rot);
    auto P = [&](float x, float y, float z, float* out) { float dx = x - cx, dz = z - cz; out[0] = cx + dx * cs + dz * sn; out[1] = y; out[2] = cz - dx * sn + dz * cs; };
    float c[8][3];
    for (int i = 0; i < 8; i++) P((i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2], c[i]);
    // corner index bits: 1 = +x, 2 = +y (top), 4 = +z
    static const int F[6][4] = {{6, 7, 5, 4}, {3, 2, 0, 1}, {2, 6, 4, 0}, {7, 3, 1, 5}, {2, 3, 7, 6}, {4, 5, 1, 0}};   // +z, -z, -x, +x, top, bottom
    for (int f = 0; f < 6; f++) {
        float q[4][3]; for (int k = 0; k < 4; k++) memcpy(q[k], c[F[f][k]], 12);
        WQuad(o, q, f == 4 ? top : f == 5 ? bottom : side, f == 4 ? topCol : col);
    }
}
// Opaque (alpha-tested) world entities: items, blocks, arrows. Cracks and the outline come after the translucent pass.
static void BuildWorldEntities() {
    g_weV.clear();
    const uint32_t lit = 0xFFC8C8C8;   // no light values in the table: a fixed, slightly dim tone (RE4 is a night game)
    for (int i = 0; i < g_weN; i++) {
        const WEnt& e = g_we[i];
        if (!(e.x == e.x) || fabsf(e.x) > 1e6f || fabsf(e.y) > 1e6f || fabsf(e.z) > 1e6f) continue;
        float yr = e.yaw * 0.0174532925f;
        switch (e.kind) {
        case 2: case 3: {   // a dropped/thrown item (or trident): a flat sprite turning about the vertical
            float h = (e.scale > 0.01f && e.scale < 4.f ? e.scale : 0.5f) * 0.5f;
            float rx = cosf(yr) * h, rz = -sinf(yr) * h;
            float q[4][3] = {{e.x - rx, e.y + h, e.z - rz}, {e.x + rx, e.y + h, e.z + rz}, {e.x + rx, e.y - h, e.z + rz}, {e.x - rx, e.y - h, e.z - rz}};
            WQuad(g_weV, q, e.uv[0], lit);
            break;
        }
        case 4: {   // a dropped block: a small spinning cube with its own faces
            float h = (e.scale > 0.01f && e.scale < 4.f ? e.scale : 0.25f) * 0.5f;
            float lo[3] = {e.x - h, e.y - h, e.z - h}, hi[3] = {e.x + h, e.y + h, e.z + h};
            WBox(g_weV, lo, hi, yr, e.uv[0], e.uv[1], e.uv[2], lit, Tinted(lit, e.tint));
            break;
        }
        case 1: {   // an arrow: two crossed strips along its flight direction (Minecraft's arrow is 0.9 blocks long)
            float pr = e.pitch * 0.0174532925f;
            float d[3] = {sinf(yr) * cosf(pr), sinf(pr), cosf(yr) * cosf(pr)};
            float a[3] = {d[2], 0.f, -d[0]}; float al = sqrtf(a[0] * a[0] + a[2] * a[2]);
            if (al < 1e-4f) { a[0] = 1.f; a[2] = 0.f; al = 1.f; }
            a[0] /= al; a[2] /= al;
            float b[3] = {d[1] * a[2] - d[2] * a[1], d[2] * a[0] - d[0] * a[2], d[0] * a[1] - d[1] * a[0]};
            const float L = 0.45f, W = 0.14f;
            for (const float* s : {a, b}) {
                float q[4][3];
                for (int k = 0; k < 3; k++) {
                    q[0][k] = (&e.x)[k] - d[k] * L + s[k] * W; q[1][k] = (&e.x)[k] + d[k] * L + s[k] * W;
                    q[2][k] = (&e.x)[k] + d[k] * L - s[k] * W; q[3][k] = (&e.x)[k] - d[k] * L - s[k] * W;
                }
                WQuad(g_weV, q, e.uv[0], lit);
            }
            break;
        }
        default: break;   // 5 cracks: drawn later; 6 shadows: not drawn
        }
    }
}
static void DrawCracksAndOutline(IDirect3DDevice9* dev) {
    static std::vector<Vtx> cv; cv.clear();
    for (int i = 0; i < g_weN; i++) {
        const WEnt& e = g_we[i];
        if (e.kind != 5 || !(e.x == e.x) || e.ext[0] <= 0 || e.ext[0] > 4 || e.ext[1] <= 0 || e.ext[1] > 4 || e.ext[2] <= 0 || e.ext[2] > 4) continue;
        const float g = 0.003f;
        float lo[3] = {e.x - g, e.y - g, e.z - g}, hi[3] = {e.x + e.ext[0] + g, e.y + e.ext[1] + g, e.z + e.ext[2] + g};
        WBox(cv, lo, hi, 0.f, e.uv[0], e.uv[0], e.uv[0], 0xFFFFFFFF, 0xFFFFFFFF);
    }
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    if (!cv.empty()) {   // Minecraft's crumbling look: 2 x texture x what's behind (darkens where the cracks are)
        dev->SetTexture(0, g_atlas);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        dev->SetRenderState(D3DRS_ALPHAREF, 0x10);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(cv.size() / 3), cv.data(), sizeof(Vtx));
    }
    if (g_sel) {   // the outline of the targeted block: thin black edges at 60%, like Minecraft's
        const float* m = g_selBox; const float g = 0.003f;
        bool ok = true; for (int k = 0; k < 6; k++) ok &= m[k] == m[k] && fabsf(m[k]) < 1e6f;
        if (ok && m[3] > m[0] && m[4] > m[1] && m[5] > m[2] && m[3] - m[0] < 4 && m[4] - m[1] < 4 && m[5] - m[2] < 4) {
            float c[8][3];
            for (int i = 0; i < 8; i++) { c[i][0] = ((i & 1) ? m[3] + g : m[0] - g) * 1000.f; c[i][1] = ((i & 2) ? m[4] + g : m[1] - g) * 1000.f; c[i][2] = ((i & 4) ? m[5] + g : m[2] - g) * 1000.f; }
            static const int E[12][2] = {{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
            Vtx lv[24]; int n = 0;
            for (auto& ed : E) for (int k = 0; k < 2; k++) lv[n++] = {c[ed[k]][0], c[ed[k]][1], c[ed[k]][2], 0x99000000u, 0.f, 0.f};
            dev->SetTexture(0, nullptr);
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            dev->DrawPrimitiveUP(D3DPT_LINELIST, 12, lv, sizeof(Vtx));
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        }
    }
}

// ------------------------------------------------------------------ matrices
struct M4 { float m[4][4]; };
static M4 Identity() { M4 r{}; for (int i = 0; i < 4; i++) r.m[i][i] = 1; return r; }

static bool g_loggedCam = false;

// RE4's own camera (GLOBAL_WK::Camera_74): GX-style view (Mtx 3x4) and projection (Mtx44), column vectors.
static bool GameMatrices(M4& view, M4& proj, float aspect) {
    uint8_t* gl = g_ppGlobal ? (uint8_t*)*g_ppGlobal : nullptr;
    MEMORY_BASIC_INFORMATION mbi;
    if (!gl || !VirtualQuery(gl, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    float a[3][4], b[3][4], p[4][4];
    memcpy(a, gl + 0x74, 48); memcpy(b, gl + 0xA4, 48); memcpy(p, gl + 0xD8, 64);
    auto& S = bridge::S();
    float eye[3] = {(float)(S.eyeX * bridge::kUnitsPerBlock), (float)(S.eyeY * bridge::kUnitsPerBlock), (float)(S.eyeZ * bridge::kUnitsPerBlock)};
    if (S.puppet && (render::g_camEye[0] != 0 || render::g_camEye[2] != 0)) memcpy(eye, render::g_camEye, 12);   // the eye the camera really got
    if (!S.puppet) {   // cutscenes, actions, RE4 control: RE4's own camera (GLOBAL_WK::Camera param.pos), not Minecraft's eye
        float cp[3]; memcpy(cp, gl + 0x74 + 0xA4, 12);
        if (cp[0] != 0 || cp[1] != 0 || cp[2] != 0) memcpy(eye, cp, 12);
    }
    // which of the two matrices maps the camera position to the origin? that's the view matrix
    auto err = [&](float (*m)[4]) {
        float e = 0;
        for (int r = 0; r < 3; r++) { float v = m[r][0] * eye[0] + m[r][1] * eye[1] + m[r][2] * eye[2] + m[r][3]; e += v * v; }
        return sqrtf(e);
    };
    auto ortho = [](float (*m)[4]) {   // a real rotation: unit-length rows
        for (int r = 0; r < 3; r++) { float l = m[r][0] * m[r][0] + m[r][1] * m[r][1] + m[r][2] * m[r][2]; if (fabsf(l - 1.f) > 0.05f) return false; }
        return true;
    };
    float ea = ortho(a) ? err(a) : 1e30f, eb = ortho(b) ? err(b) : 1e30f;
    float (*v)[4] = ea < eb ? a : b;
    bool viewOk = fminf(ea, eb) < 300.f;   // within 30 cm
    // Once one of the two has been confirmed as RE4's view matrix, keep using it when the eye check fails for a
    // frame (the first frame after a menu, a pick-up, a door: our eye and RE4's camera are a frame apart). RE4
    // drew that frame with it, so the blocks stay put; 0.31 switched to Minecraft's eye there and they jumped.
    static int known = -1;
    if (viewOk) known = ea < eb ? 0 : 1;
    else if (known >= 0 && (known == 0 ? ea : eb) < 1e29f) { v = known == 0 ? a : b; viewOk = true; }
    // projection: GX perspective has -1 in [3][2]
    bool projOk = fabsf(p[3][2] + 1.f) < 1e-3f && p[1][1] > 0.1f && p[2][2] < 0;
    static int lastChoice = -2; int choice = viewOk ? (v == a ? 0 : 1) : -1;
    static int changes = 0;
    if (choice != lastChoice && lastChoice != -2 && g_loggedCam && changes++ < 20) { BridgeLog("blocks: camera now %s (%s)", choice == 0 ? "mat_0" : choice == 1 ? "v_mat" : "Minecraft's eye (RE4's view not recognised)", S.puppet ? "Minecraft has Leon" : "RE4 has Leon"); }
    lastChoice = choice;
    if (!g_loggedCam) {
        g_loggedCam = true;
        BridgeLog("blocks: camera mat_0 err %.0f, v_mat err %.0f -> %s; proj [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f] %s",
                  ea, eb, viewOk ? (ea < eb ? "mat_0" : "v_mat") : "neither",
                  p[0][0], p[0][1], p[0][2], p[0][3], p[1][0], p[1][1], p[1][2], p[1][3], p[2][0], p[2][1], p[2][2], p[2][3], p[3][0], p[3][1], p[3][2], p[3][3],
                  projOk ? "(GX perspective)" : "(not recognised)");
    }
    if (!viewOk) return false;
    // D3D fixed function uses row vectors: transpose.
    view = Identity();
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) view.m[i][j] = v[j][i];
    for (int j = 0; j < 3; j++) view.m[3][j] = v[j][3];
    // Projection: rebuild a D3D (0..1 depth) right-handed perspective with RE4's own fov/near/far,
    // which gives the same depth values RE4 writes.
    float ys, xs, n, f;
    if (projOk) {
        float A = p[2][2], B = p[2][3];
        f = B / A; n = A * f / (A - 1.f);
        ys = p[1][1]; xs = p[0][0];
        if (!(n > 0.1f && f > n)) { n = 100.f; f = 300000.f; }
    } else {
        ys = 1.f / tanf(S.fov * 0.5f * 0.0174532925f); xs = ys / aspect; n = 100.f; f = 300000.f;
    }
    proj = M4{};
    proj.m[0][0] = xs; proj.m[1][1] = ys;
    proj.m[2][2] = f / (n - f); proj.m[2][3] = -1.f;
    proj.m[3][2] = n * f / (n - f);
    static bool loggedNF = false;
    if (!loggedNF) { loggedNF = true; BridgeLog("blocks: near %.0f far %.0f, fov %.1f deg", n, f, 2 * atanf(1.f / ys) * 57.29578f); }
    return true;
}

// Fallback when RE4's matrices can't be read: our own camera (we put it there anyway).
static void OwnMatrices(M4& view, M4& proj, float aspect) {
    auto& S = bridge::S();
    float yaw, pitch; bridge::GetLook(yaw, pitch);
    float yr = yaw * 0.0174532925f, pr = pitch * 0.0174532925f;
    float d[3] = {-sinf(yr) * cosf(pr), -sinf(pr), cosf(yr) * cosf(pr)};
    float e[3] = {(float)(S.eyeX * 1000), (float)(S.eyeY * 1000), (float)(S.eyeZ * 1000)};
    float zaxis[3] = {-d[0], -d[1], -d[2]};   // RH: camera looks down -z
    float up[3] = {0, 1, 0};
    float xaxis[3] = {up[1] * zaxis[2] - up[2] * zaxis[1], up[2] * zaxis[0] - up[0] * zaxis[2], up[0] * zaxis[1] - up[1] * zaxis[0]};
    float xl = sqrtf(xaxis[0] * xaxis[0] + xaxis[1] * xaxis[1] + xaxis[2] * xaxis[2]); for (float& c : xaxis) c /= xl;
    float yaxis[3] = {zaxis[1] * xaxis[2] - zaxis[2] * xaxis[1], zaxis[2] * xaxis[0] - zaxis[0] * xaxis[2], zaxis[0] * xaxis[1] - zaxis[1] * xaxis[0]};
    view = Identity();
    for (int i = 0; i < 3; i++) { view.m[i][0] = xaxis[i]; view.m[i][1] = yaxis[i]; view.m[i][2] = zaxis[i]; }
    view.m[3][0] = -(xaxis[0] * e[0] + xaxis[1] * e[1] + xaxis[2] * e[2]);
    view.m[3][1] = -(yaxis[0] * e[0] + yaxis[1] * e[1] + yaxis[2] * e[2]);
    view.m[3][2] = -(zaxis[0] * e[0] + zaxis[1] * e[1] + zaxis[2] * e[2]);
    float ys = 1.f / tanf(S.fov * 0.5f * 0.0174532925f), n = 100.f, f = 300000.f;
    proj = M4{}; proj.m[0][0] = ys / aspect; proj.m[1][1] = ys; proj.m[2][2] = f / (n - f); proj.m[2][3] = -1.f; proj.m[3][2] = n * f / (n - f);
}

// ------------------------------------------------------------------ drawing
void OnDevice(IDirect3DDevice9* dev) {
    if (g_dev == dev) return;
    g_dev = dev;
    // (managed textures survive resets; a new device needs new ones)
    if (g_atlas) { g_atlas->Release(); g_atlas = nullptr; }
    if (!g_atlasCpu.empty()) g_atlas = MakeTexture(g_atlasW, g_atlasH, g_atlasCpu.data());
    for (auto& t : g_textures) { if (t.second) t.second->Release(); t.second = MakeTexture(g_texSize[t.first].first, g_texSize[t.first].second, g_texturesCpu[t.first].data()); }
}

static int g_drawFails = 0;
static void DrawList(IDirect3DDevice9* dev, const std::vector<Vtx>& v, uint32_t first, uint32_t count) {
    const uint32_t kMax = 3 * 20000;
    for (uint32_t at = 0; at < count; at += kMax) {
        uint32_t c = count - at < kMax ? count - at : kMax;
        HRESULT hr = dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, c / 3, &v[first + at], sizeof(Vtx));
        if (FAILED(hr) && g_drawFails++ < 3) BridgeLog("blocks: draw failed 0x%08lX", hr);
    }
}

static void PoseAvatar(std::vector<Vtx>& out) {
    out.resize(g_avV.size());
    for (auto& b : g_avB) {
        const float (*M)[4] = g_avPart[b.part >= 1 && b.part <= 6 ? b.part : 0];
        for (uint32_t i = b.first; i < b.first + b.count; i++) {
            Vtx v = g_avV[i];
            float x = v.x, y = v.y, z = v.z;
            v.x = M[0][0] * x + M[0][1] * y + M[0][2] * z + M[0][3];
            v.y = M[1][0] * x + M[1][1] * y + M[1][2] * z + M[1][3];
            v.z = M[2][0] * x + M[2][1] * y + M[2][2] * z + M[2][3];
            out[i] = v;
        }
    }
}

// ------------------------------------------------------------------ occlusion
// RE4's depth buffer no longer holds the scene when Present runs, so Minecraft's blocks showed
// through walls. We keep our own depth buffer and first draw the room's collision mesh into it
// (depth only, pushed 3 cm away from the eye so blocks built against a wall don't flicker).
static IDirect3DSurface9* g_depth = nullptr; static UINT g_depthW = 0, g_depthH = 0; static bool g_depthBroken = false;
static std::vector<float> g_occ; static uint32_t g_occVer = 0; static uint16_t g_occRoom = 0xFFFF;
static IDirect3DStateBlock9* g_sb = nullptr; static IDirect3DDevice9* g_sbDev = nullptr;
void OnReset() { if (g_sb) { g_sb->Release(); g_sb = nullptr; } if (g_depth) { g_depth->Release(); g_depth = nullptr; } g_depthW = g_depthH = 0; }

// The room's walls sit in a vertex buffer of their own (rebuilt when the room changes) instead of being
// re-sent every frame; the 3 cm push away from the eye is done by the view matrix (everything drawn 3 cm
// further along the view direction) instead of moving every vertex on the CPU.
static IDirect3DVertexBuffer9* g_occVB = nullptr; static IDirect3DDevice9* g_occVBDev = nullptr; static UINT g_occVBTris = 0;
static void DrawOccluders(IDirect3DDevice9* dev, const float eye[3], const M4& view) {
    bool changed = collision::Occluders(g_occVer, g_occRoom, g_occ);
    if (changed || g_occVBDev != dev) {
        if (g_occVB) { g_occVB->Release(); g_occVB = nullptr; }
        g_occVBTris = 0; g_occVBDev = dev;
        size_t bytes = g_occ.size() * sizeof(float);
        if (bytes >= 36 && SUCCEEDED(dev->CreateVertexBuffer((UINT)bytes, D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_MANAGED, &g_occVB, nullptr))) {
            void* p = nullptr;
            if (SUCCEEDED(g_occVB->Lock(0, 0, &p, 0))) { memcpy(p, g_occ.data(), bytes); g_occVB->Unlock(); g_occVBTris = (UINT)(g_occ.size() / 9); }
            else { g_occVB->Release(); g_occVB = nullptr; }
        }
        static int said = 0;
        if (said++ < 8) BridgeLog("blocks: room %04X walls for hiding blocks: %u triangles%s", g_occRoom, g_occVBTris, g_occVB || g_occ.empty() ? "" : " (vertex buffer failed - drawn directly)");
    }
    M4 pushed = view; pushed.m[3][2] -= 30.f;   // view space looks down -z: 30 mm further away
    dev->SetTransform(D3DTS_VIEW, (D3DMATRIX*)&pushed);
    dev->SetFVF(D3DFVF_XYZ);
    dev->SetTexture(0, nullptr);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    const UINT kMaxTris = 20000;
    if (g_occVB) {
        dev->SetStreamSource(0, g_occVB, 0, 12);
        for (UINT at = 0; at < g_occVBTris; at += kMaxTris) dev->DrawPrimitive(D3DPT_TRIANGLELIST, at * 3, (g_occVBTris - at) < kMaxTris ? (g_occVBTris - at) : kMaxTris);
        dev->SetStreamSource(0, nullptr, 0, 0);
    } else if (!g_occ.empty()) {
        size_t tris = g_occ.size() / 9;
        for (size_t at = 0; at < tris; at += kMaxTris)
            dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)((tris - at) < kMaxTris ? (tris - at) : kMaxTris), &g_occ[at * 9], 12);
    }
    // RE4's enemies hide blocks behind them too (a slim box per live enemy - RE4 drew them, but its depth is gone)
    static std::vector<float> en, boxes; combat::EnemyPositions(en);
    boxes.clear();
    for (size_t e = 0; e + 2 < en.size(); e += 3) {
        float x = en[e], y = en[e + 1], z = en[e + 2];
        if (fabsf(eye[0] - x) < 600.f && fabsf(eye[2] - z) < 600.f) continue;   // grabbing us: don't hide the world
        const float hw = 250.f, h = 1700.f;
        float c[8][3];
        for (int i = 0; i < 8; i++) { c[i][0] = x + (i & 1 ? hw : -hw); c[i][1] = y + (i & 2 ? h : 0.f); c[i][2] = z + (i & 4 ? hw : -hw); }
        static const int f[12][3] = {{0,1,3},{0,3,2},{4,6,7},{4,7,5},{0,4,5},{0,5,1},{2,3,7},{2,7,6},{0,2,6},{0,6,4},{1,5,7},{1,7,3}};
        for (auto& t : f) for (int k = 0; k < 3; k++) boxes.insert(boxes.end(), c[t[k]], c[t[k]] + 3);
    }
    if (!boxes.empty()) dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(boxes.size() / 9), boxes.data(), 12);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    dev->SetFVF(kFvf);
    dev->SetTransform(D3DTS_VIEW, (D3DMATRIX*)&view);
}

void Draw(IDirect3DDevice9* dev, bool mcAlive) {
    OnDevice(dev);
    if (!mcAlive) return;
    Drain();
    bool avatar = g_avShow && !g_avB.empty();
    ReadWorldEntities();
    if (g_sections.empty() && g_sceneB.empty() && !avatar && !g_weN && !g_sel) return;
    if (!g_atlas) return;

    IDirect3DStateBlock9*& sb = g_sb;
    if (sb && g_sbDev != dev) { sb->Release(); sb = nullptr; }
    if (!sb) { if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) return; g_sbDev = dev; }
    else sb->Capture();
    IDirect3DSurface9 *oldRt = nullptr, *oldDs = nullptr, *bb = nullptr;
    dev->GetRenderTarget(0, &oldRt);
    dev->GetDepthStencilSurface(&oldDs);
    D3DSURFACE_DESC bd{}, dd{};
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) { sb->Apply(); if (oldRt) oldRt->Release(); if (oldDs) oldDs->Release(); return; }
    bb->GetDesc(&bd);
    (void)dd;
    if (g_depth && (g_depthW != bd.Width || g_depthH != bd.Height)) OnReset();
    static bool depthFailed = false;
    if (!g_depth && !depthFailed) {
        // must match the back buffer's anti-aliasing, or nothing drawn with it shows up
        HRESULT hr = dev->CreateDepthStencilSurface(bd.Width, bd.Height, D3DFMT_D24S8, bd.MultiSampleType, bd.MultiSampleQuality, TRUE, &g_depth, nullptr);
        if (FAILED(hr)) hr = dev->CreateDepthStencilSurface(bd.Width, bd.Height, D3DFMT_D16, bd.MultiSampleType, bd.MultiSampleQuality, TRUE, &g_depth, nullptr);
        if (SUCCEEDED(hr)) { g_depthW = bd.Width; g_depthH = bd.Height; }
        else { depthFailed = true; g_depth = nullptr; BridgeLog("blocks: no depth buffer (0x%08lX) - blocks drawn without wall hiding", hr); }
    }
    bool depth = g_depth != nullptr && !g_depthBroken && cfg::blocksBehindWalls;
    static int logged = 0;
    if (!logged++) BridgeLog("blocks: drawing into %ux%u (anti-aliasing %d/%lu), %s", bd.Width, bd.Height, (int)bd.MultiSampleType, bd.MultiSampleQuality,
                             depth ? "hidden behind the room's walls (own depth buffer)" : "no depth buffer");
    dev->SetRenderTarget(0, bb);
    dev->SetDepthStencilSurface(depth ? g_depth : nullptr);
    D3DVIEWPORT9 vp = {0, 0, bd.Width, bd.Height, 0, 1};
    dev->SetViewport(&vp);

    M4 view, proj; float aspect = bd.Height ? (float)bd.Width / bd.Height : 16.f / 9.f;
    if (!GameMatrices(view, proj, aspect)) OwnMatrices(view, proj, aspect);
    if (getenv("R4DBG")) {
        static int n = 0;
        if (n++ % 30 == 0 && !g_sections.empty()) {
            const Vtx& v0 = g_sections.begin()->second.solid.empty() ? g_sections.begin()->second.translucent[0] : g_sections.begin()->second.solid[0];
            float in[4] = {v0.x, v0.y, v0.z, 1}, a[4] = {0}, c[4] = {0};
            for (int j = 0; j < 4; j++) for (int k = 0; k < 4; k++) a[j] += in[k] * view.m[k][j];
            for (int j = 0; j < 4; j++) for (int k = 0; k < 4; k++) c[j] += a[k] * proj.m[k][j];
            auto& S = bridge::S();
            BridgeLog("dbg puppet %d vertex (%.0f %.0f %.0f) col %08X eye (%.0f %.0f %.0f) -> view (%.0f %.0f %.0f) ndc (%.2f %.2f %.4f) w %.0f",
                      (int)S.puppet, v0.x, v0.y, v0.z, v0.color, S.eyeX * 1000, S.eyeY * 1000, S.eyeZ * 1000, a[0], a[1], a[2], c[0] / c[3], c[1] / c[3], c[2] / c[3], c[3]);
        }
    }
    M4 world = Identity();
    dev->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&world);
    dev->SetTransform(D3DTS_VIEW, (D3DMATRIX*)&view);
    dev->SetTransform(D3DTS_PROJECTION, (D3DMATRIX*)&proj);

    if (SUCCEEDED(dev->BeginScene())) {
        dev->SetVertexShader(nullptr); dev->SetPixelShader(nullptr); dev->SetFVF(kFvf);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZENABLE, depth ? D3DZB_TRUE : D3DZB_FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, depth);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        dev->SetRenderState(D3DRS_ALPHAREF, 0x80);
        dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        if (depth) {
            dev->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
            auto& S = bridge::S();
            float eye[3] = {(float)(S.eyeX * bridge::kUnitsPerBlock), (float)(S.eyeY * bridge::kUnitsPerBlock), (float)(S.eyeZ * bridge::kUnitsPerBlock)};
            uint8_t* gl = g_ppGlobal ? (uint8_t*)*g_ppGlobal : nullptr;   // RE4's camera position (cutscenes)
            if (gl && !S.puppet) { float cp[3]; memcpy(cp, gl + 0x74 + 0xA4, 12); if (cp[0] != 0 || cp[2] != 0) memcpy(eye, cp, 12); }
            DrawOccluders(dev, eye, view);
        }
        // solid + cutout blocks, then opaque entities
        dev->SetTexture(0, g_atlas);
        for (auto& s : g_sections) if (!s.second.solid.empty()) DrawList(dev, s.second.solid, 0, (uint32_t)s.second.solid.size());
        for (auto& b : g_sceneB) {
            if (b.translucent) continue;
            auto it = g_textures.find(b.tex);
            dev->SetTexture(0, b.tex == 0 ? g_atlas : (it != g_textures.end() ? it->second : nullptr));
            DrawList(dev, g_sceneV, b.first, b.count);
        }
        static int frames = 0;
        if (++frames == 120) {
            size_t tris = 0; for (auto& sct : g_sections) tris += (sct.second.solid.size() + sct.second.translucent.size()) / 3;
            BridgeLog("blocks: %u chunk sections, %u block triangles, %u entity batches, %u draw failures", (unsigned)g_sections.size(), (unsigned)tris, (unsigned)g_sceneB.size(), g_drawFails);
            if (g_drawFails && depth) { g_depthBroken = true; BridgeLog("blocks: drawing failed with the own depth buffer - switching wall hiding off"); }
        }
        if (avatar) {   // the Minecraft player standing in for Leon
            static std::vector<Vtx> posed; PoseAvatar(posed);
            for (auto& b : g_avB) {
                if (b.translucent) continue;
                auto it = g_textures.find(b.tex);
                dev->SetTexture(0, b.tex == 0 ? g_atlas : (it != g_textures.end() ? it->second : nullptr));
                DrawList(dev, posed, b.first, b.count);
            }
        }
        // dropped items and blocks, arrows, thrown pearls / fire charges (alpha-tested, like the blocks)
        BuildWorldEntities();
        if (!g_weV.empty()) { dev->SetTexture(0, g_atlas); DrawList(dev, g_weV, 0, (uint32_t)g_weV.size()); }
        // translucent (water, glass, ice, particles)
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetTexture(0, g_atlas);
        for (auto& s : g_sections) if (!s.second.translucent.empty()) DrawList(dev, s.second.translucent, 0, (uint32_t)s.second.translucent.size());
        for (auto& b : g_sceneB) {
            if (!b.translucent) continue;
            auto it = g_textures.find(b.tex);
            dev->SetTexture(0, b.tex == 0 ? g_atlas : (it != g_textures.end() ? it->second : nullptr));
            DrawList(dev, g_sceneV, b.first, b.count);
        }
        DrawCracksAndOutline(dev);   // last: blended over what's there, depth-tested against the walls and blocks
        dev->EndScene();
    }
    dev->SetRenderTarget(0, oldRt);
    dev->SetDepthStencilSurface(oldDs);
    if (oldRt) oldRt->Release();
    if (oldDs) oldDs->Release();
    bb->Release();
    sb->Apply();
}

// ------------------------------------------------------------------ radio portrait
// Hunnigan's video calls show Leon's face in the right-hand panel; draw the Minecraft player's
// head and shoulders there instead (front view, a little turned, orthographic).
void DrawPortrait(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1) {
    OnDevice(dev);
    Drain();
    if (g_avB.empty() || !g_atlas) return;
    IDirect3DStateBlock9*& sb = g_sb;
    if (sb && g_sbDev != dev) { sb->Release(); sb = nullptr; }
    if (!sb) { if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) return; g_sbDev = dev; }
    else sb->Capture();
    IDirect3DSurface9 *oldRt = nullptr, *oldDs = nullptr, *bb = nullptr;
    dev->GetRenderTarget(0, &oldRt);
    dev->GetDepthStencilSurface(&oldDs);
    D3DSURFACE_DESC bd{};
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) { sb->Apply(); if (oldRt) oldRt->Release(); if (oldDs) oldDs->Release(); return; }
    bb->GetDesc(&bd);
    bool depth = g_depth && g_depthW == bd.Width && g_depthH == bd.Height && !g_depthBroken;
    dev->SetRenderTarget(0, bb);
    dev->SetDepthStencilSurface(depth ? g_depth : nullptr);
    DWORD px0 = (DWORD)(x0 * bd.Width), py0 = (DWORD)(y0 * bd.Height), px1 = (DWORD)(x1 * bd.Width), py1 = (DWORD)(y1 * bd.Height);
    if (px1 <= px0 + 8 || py1 <= py0 + 8) { dev->SetRenderTarget(0, oldRt); dev->SetDepthStencilSurface(oldDs); if (oldRt) oldRt->Release(); if (oldDs) oldDs->Release(); bb->Release(); sb->Apply(); return; }
    D3DVIEWPORT9 vp = {px0, py0, px1 - px0, py1 - py0, 0, 1};
    dev->SetViewport(&vp);

    // head and shoulders: the model is in mm, standing, facing +Z, feet at the origin
    const float yaw = 20.f * 0.0174532925f, cy = cosf(yaw), sy = sinf(yaw);
    float top = 1880.f, bottom = 960.f, aspect = (float)(px1 - px0) / (float)(py1 - py0);
    float halfW = (top - bottom) * aspect * 0.5f;
    static std::vector<Vtx> pv; pv.resize(g_avV.size());
    for (size_t i = 0; i < g_avV.size(); i++) {
        Vtx v = g_avV[i];
        float x = v.x * cy + v.z * sy, z = -v.x * sy + v.z * cy;
        v.x = x; v.z = -z;   // depth grows away from the viewer (who stands at +Z)
        pv[i] = v;
    }
    M4 id = Identity(), proj{};
    float l = -halfW, r = halfW, b = bottom, t = top, zn = -1500.f, zf = 1500.f;
    proj.m[0][0] = 2.f / (r - l); proj.m[1][1] = 2.f / (t - b); proj.m[2][2] = 1.f / (zf - zn); proj.m[3][3] = 1.f;
    proj.m[3][0] = (l + r) / (l - r); proj.m[3][1] = (t + b) / (b - t); proj.m[3][2] = zn / (zn - zf);

    if (SUCCEEDED(dev->BeginScene())) {
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | (depth ? D3DCLEAR_ZBUFFER : 0), D3DCOLOR_XRGB(0x1B, 0x26, 0x2C), 1.0f, 0);
        dev->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&id);
        dev->SetTransform(D3DTS_VIEW, (D3DMATRIX*)&id);
        dev->SetTransform(D3DTS_PROJECTION, (D3DMATRIX*)&proj);
        dev->SetVertexShader(nullptr); dev->SetPixelShader(nullptr); dev->SetFVF(kFvf);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZENABLE, depth ? D3DZB_TRUE : D3DZB_FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, depth);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        dev->SetRenderState(D3DRS_ALPHAREF, 0x80);
        dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        // without a depth buffer, far triangles first (painter's order) - fine for a few boxes
        static std::vector<Vtx> sorted; static std::vector<std::pair<float, uint32_t>> order;
        for (int pass = 0; pass < 2; pass++) {
            for (auto& bt : g_avB) {
                if (bt.translucent != (pass == 1)) continue;
                auto it = g_textures.find(bt.tex);
                dev->SetTexture(0, bt.tex == 0 ? g_atlas : (it != g_textures.end() ? it->second : nullptr));
                if (depth) { DrawList(dev, pv, bt.first, bt.count); continue; }
                order.clear();
                for (uint32_t i = bt.first; i + 2 < bt.first + bt.count; i += 3) order.push_back({pv[i].z + pv[i + 1].z + pv[i + 2].z, i});
                std::sort(order.begin(), order.end(), [](const std::pair<float, uint32_t>& a, const std::pair<float, uint32_t>& c) { return a.first > c.first; });
                sorted.clear();
                for (auto& o : order) { sorted.push_back(pv[o.second]); sorted.push_back(pv[o.second + 1]); sorted.push_back(pv[o.second + 2]); }
                if (!sorted.empty()) DrawList(dev, sorted, 0, (uint32_t)sorted.size());
            }
        }
        dev->EndScene();
    }
    static bool logged = false;
    if (!logged) { logged = true; BridgeLog("radio: Minecraft portrait in the call panel (%lux%lu at %lu,%lu)%s", vp.Width, vp.Height, px0, py0, depth ? "" : ", painter's order"); }
    dev->SetRenderTarget(0, oldRt);
    dev->SetDepthStencilSurface(oldDs);
    if (oldRt) oldRt->Release();
    if (oldDs) oldDs->Release();
    bb->Release();
    sb->Apply();
}

} // namespace blocks
