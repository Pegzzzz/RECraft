// RECraft: Minecraft models for the items lying in RE4's world.
//
// Every room carries its own item models (the room's "ITM" block: one bin/tpl pair per item id, registered
// into cItmSys's table by ItemModelDataLoad; decompilation game/item_model.cpp). When a room loads we swap
// the pair of every item RECraft maps for RECraft's own model - the files BIO4\SS\cmn\itmXX.bin/.tpl that
// tools/build_world_items.py makes from the player's Minecraft jar - and load that model's texture pack
// (ImagePackHD\220000XX.pack.yz2), the same way the PC port itself loads SS\cmn\itmXX for items a room has
// no model for. Ammo, herbs, grenades and weapons then look like Minecraft items on the floor, on tables,
// in the "Take it?" view and when dropped by enemies.
#include <windows.h>
#include <malloc.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "MinHook.h"
#include "memcheck.h"

void BridgeLog(const char* fmt, ...);

namespace items {

// the ids tools/build_world_items.py builds (ammo, herbs, grenades, weapons)
static const uint8_t kIds[] = {
    0x00, 0x04, 0x07, 0x18, 0x1A, 0x20, 0x6A,                           // ammo -> arrow
    0x06, 0x19, 0x1C, 0x12, 0x13, 0x14, 0x15, 0x16, 0xA8,               // herbs -> steak
    0x01, 0x02, 0x0E,                                                   // grenades -> TNT, fire charge, ender pearl
    0x23, 0x21, 0x40, 0x25, 0x26, 0x27, 0x03, 0x29, 0x2A, 0x37,         // handguns -> swords
    0x2C, 0x94, 0x2D, 0x34,                                             // shotguns -> axes
    0x2E, 0x6B, 0x2F, 0x6C, 0x51,                                       // rifles -> bow
    0x30, 0x32, 0x3E,                                                   // TMP -> enchanted book
    0x78, 0x79,                                                         // pesetas -> emerald
};

struct Model { std::vector<uint8_t> bin, tpl; uint8_t* liveBin = nullptr; uint8_t* liveTpl = nullptr; };
static Model g_models[256];
static bool g_have[256];
static uint8_t** g_ppSys = nullptr;                       // cItmSys* g_pItemModelSys; table of {bin, tpl} per id
typedef int(__cdecl* DataLoadFn)(void*);
static DataLoadFn g_orig = nullptr;
typedef int(__cdecl* PackFn)(uint32_t, uint32_t);
static PackFn g_loadPack = nullptr;

// Spinning like a dropped Minecraft item: the item models made from our files (setItemObj is handed our
// bin), turned about their vertical axis every frame. cModel: vtable +0, be_flag +4, ang +0xA0.
typedef uint8_t*(__cdecl* SetItemObjFn)(void* bin, void* tpl, void* pos, void* ang);
static SetItemObjFn g_setItemObj = nullptr;
struct Spin { uint8_t* m; uint32_t vtbl; float phase; };
static Spin g_spin[96];
static int g_nSpin = 0;

static uint8_t* __cdecl SetItemObjHook(void* bin, void* tpl, void* pos, void* ang) {
    uint8_t* m = g_setItemObj(bin, tpl, pos, ang);
    if (!m) return m;
    for (int id = 0; id < 256; id++) {
        if (!g_have[id] || bin != g_models[id].liveBin) continue;
        if (g_nSpin < (int)(sizeof g_spin / sizeof g_spin[0])) {
            g_spin[g_nSpin].m = m; g_spin[g_nSpin].vtbl = *(uint32_t*)m;
            g_spin[g_nSpin].phase = (float)(g_nSpin * 1.7);   // not all in step, like Minecraft's bob offset
            g_nSpin++;
        }
        break;
    }
    return m;
}

void Tick(bool paused) {
    static LARGE_INTEGER f{}, last{};
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    float dt = last.QuadPart ? (float)((now.QuadPart - last.QuadPart) / (double)f.QuadPart) : 0.f;
    last = now;
    if (paused || dt <= 0.f || dt > 0.25f) return;
    for (int i = 0; i < g_nSpin; i++) {
        Spin& s = g_spin[i];
        uint8_t* m = s.m;
        bool ok = mem::Readable(m, 0xB0) && *(uint32_t*)m == s.vtbl && (*(uint32_t*)(m + 4) & 1);
        if (!ok) { g_spin[i] = g_spin[--g_nSpin]; i--; continue; }   // picked up or gone
        float* ang = (float*)(m + 0xA0);
        ang[1] += dt * 1.0f;   // Minecraft: about one radian a second
        if (ang[1] > 6.2831853f) ang[1] -= 6.2831853f;
    }
}

static bool ReadFile_(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    bool ok = n > 0 && fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

static int __cdecl DataLoadHook(void* data) {
    int r = g_orig(data);
    g_nSpin = 0;   // a new room: the old models are gone
    uint8_t* sys = g_ppSys ? *g_ppSys : nullptr;
    if (!sys) return r;
    int swapped = 0;
    for (int id = 0; id < 256; id++) {
        if (!g_have[id]) continue;
        uint32_t* w = (uint32_t*)(sys + id * 8);
        if (!w[0] || !w[1]) continue;   // this room has no model for it (the game falls back to SS\cmn itself)
        Model& m = g_models[id];
        // a fresh copy per room: the engine may fix up model data in place, like the room's own copy
        memcpy(m.liveBin, m.bin.data(), m.bin.size());
        memcpy(m.liveTpl, m.tpl.data(), m.tpl.size());
        w[0] = (uint32_t)(uintptr_t)m.liveBin;
        w[1] = (uint32_t)(uintptr_t)m.liveTpl;
        if (g_loadPack) g_loadPack(0x22000000u | (uint32_t)id, 0);
        swapped++;
    }
    static int logs = 0;
    if (logs < 12) { logs++; BridgeLog("items: room item models - %d swapped for Minecraft ones", swapped); }
    return r;
}

// itmCall: the call to ItemModelDataLoad after GetDataExt(room, "ITM"); packCall: the PC port's
// "load image pack 0x22000000 + id" call in its SS\cmn item model fallback.
void Install(uint8_t* itmCall, uint8_t* packCall, uint8_t* setObjCall) {
    char dir[MAX_PATH]; GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\'); if (slash) *slash = 0;
    int n = 0;
    for (uint8_t id : kIds) {
        char pb[MAX_PATH + 64], pt[MAX_PATH + 64];
        snprintf(pb, sizeof pb, "%s\\..\\BIO4\\SS\\cmn\\itm%02x.bin", dir, id);
        snprintf(pt, sizeof pt, "%s\\..\\BIO4\\SS\\cmn\\itm%02x.tpl", dir, id);
        Model& m = g_models[id];
        if (!ReadFile_(pb, m.bin) || !ReadFile_(pt, m.tpl)) continue;
        m.liveBin = (uint8_t*)_aligned_malloc(m.bin.size(), 64);
        m.liveTpl = (uint8_t*)_aligned_malloc(m.tpl.size(), 64);
        if (!m.liveBin || !m.liveTpl) continue;
        g_have[id] = true; n++;
    }
    if (!n) { BridgeLog("items: no Minecraft item models in BIO4\\SS\\cmn - RE4's own models stay"); return; }
    if (packCall && packCall[0] == 0xE8) g_loadPack = (PackFn)(packCall + 5 + *(int32_t*)(packCall + 1));
    uint8_t* fn = nullptr;
    if (itmCall && itmCall[0] == 0xE8) fn = itmCall + 5 + *(int32_t*)(itmCall + 1);
    if (fn && fn[0] == 0xE9) fn = fn + 5 + *(int32_t*)(fn + 1);   // incremental-link thunk
    // ItemModelDataLoad: push ebp; mov ebp,esp; mov eax,[ebp+8]; mov ecx,[g_pItemModelSys]; ...
    static const uint8_t head[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x8B, 0x0D};
    if (!fn || memcmp(fn, head, sizeof head) != 0) { BridgeLog("items: ItemModelDataLoad not recognised (%p) - item models unchanged", fn); return; }
    g_ppSys = *(uint8_t***)(fn + 8);
    MH_STATUS st = MH_CreateHook(fn, (void*)&DataLoadHook, (void**)&g_orig);
    if (st == MH_OK) st = MH_EnableHook(fn);
    uint8_t* so = (setObjCall && setObjCall[0] == 0xE8) ? setObjCall + 5 + *(int32_t*)(setObjCall + 1) : nullptr;
    if (so && so[0] == 0xE9) so = so + 5 + *(int32_t*)(so + 1);
    MH_STATUS ss = so ? MH_CreateHook(so, (void*)&SetItemObjHook, (void**)&g_setItemObj) : MH_ERROR_NOT_EXECUTABLE;
    if (ss == MH_OK) ss = MH_EnableHook(so);
    BridgeLog("items: spinning %s (setItemObj %p)", ss == MH_OK ? "on" : "off", so);
    BridgeLog("items: %d Minecraft item models loaded; room hook %s (ItemModelDataLoad %p, table %p, pack loader %p)",
              n, st == MH_OK ? "on" : "FAILED", fn, (void*)g_ppSys, (void*)g_loadPack);
}

} // namespace items
