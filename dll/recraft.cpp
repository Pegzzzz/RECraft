// RECraft - RE4 UHD side of a SkyCraft-style link with Minecraft.
// Loaded as winmm.dll next to bio4.exe; forwards every winmm export to the system copy.
// Phase 2: Minecraft drives Leon. Input goes to Minecraft (input.cpp), RE4's camera follows the
// Minecraft player's eye and Minecraft's HUD/hand is drawn over RE4 (render.cpp). F6 hands control
// back and forth between Minecraft and RE4.

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <string>
#include "winmm_names.h"
#include "link.h"
#include "MinHook.h"
#include "memcheck.h"

namespace mem { __thread Reg t_reg[16]; __thread int t_n = 0, t_next = 0; }

static char  g_dir[MAX_PATH];
static FILE* g_log = nullptr;

void BridgeLog(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}
#define Log BridgeLog

namespace input { bool Install(); extern volatile bool g_re4Control; }
namespace collision { bool Load(const char* path); }
namespace blocks { extern uint8_t*** g_ppGlobal; }
namespace names { void Start(); }
namespace items { void Install(uint8_t* itmCall, uint8_t* packCall, uint8_t* setObjCall); }
namespace combat { extern uint8_t* g_itemMgr; extern void (__fastcall* g_itemErase)(void*, void*, void*); extern uint8_t* g_cand[8]; extern int g_candN; extern uint8_t*** g_ppGlobal; extern uint8_t*** g_ppPlayer; }
namespace render { extern uint8_t* g_subScreen; extern uint8_t* g_evtMgr; extern uint8_t* g_idSys; bool Install(uint8_t* cameraCallSite); bool InstallFinalCamera(uint8_t* site); extern uint8_t*** g_ppPlayer; extern uint8_t*** g_ppGlobal; }
volatile ULONGLONG g_lastPresent = 0;

// ------------------------------------------------------------------ pattern scan (re4_tweaks syntax)
static int FindPatternAll(const char* pat, uint8_t** out, int maxOut);
static uint8_t* FindPattern(const char* pat) {
    uint8_t* r[2]; int c = FindPatternAll(pat, r, 2);
    Log("pattern \"%s\" -> %p (%d match%s)", pat, c ? r[0] : nullptr, c, c == 1 ? "" : "es");
    return c == 1 ? r[0] : nullptr;
}
static int FindPatternAll(const char* pat, uint8_t** out, int maxOut) {
    uint8_t bytes[128]; bool mask[128]; int n = 0;
    for (const char* p = pat; *p && n < 128;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '?') { bytes[n] = 0; mask[n++] = false; p++; if (*p == '?') p++; }
        else { bytes[n] = (uint8_t)strtoul(p, (char**)&p, 16); mask[n++] = true; }
    }
    HMODULE exe = GetModuleHandleA(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)((uint8_t*)exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    uint8_t* base = (uint8_t*)exe; size_t size = nt->OptionalHeader.SizeOfImage;
    int count = 0;
    for (size_t i = 0; i + n <= size; i++) {
        int j = 0;
        for (; j < n; j++) if (mask[j] && base[i + j] != bytes[j]) break;
        if (j == n) { if (count < maxOut) out[count] = base + i; count++; }
    }
    return count;
}

static bool Readable(const void* p, size_t n) {
    if ((uintptr_t)p < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return (uint8_t*)p + n <= (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
}

// ------------------------------------------------------------------ settings (RECraft.ini)
namespace cfg {
bool  hideLeon = true, hideLeonInCutscenes = true, hudInCutscenes = false, hitReactions = false, avatarInCutscenes = true, blocksBehindWalls = true, lowLatency = true, predictMovement = true, evenRefresh = true, mcLowPriority = false, doorCollision = true, overlayHalf = false, vsyncHalfRate = false;
float damageScale = 45.0f, hurtScale = 0.015f;
bool  linkHealth = true;
int   staggerCooldownMs = 2500, staggerChance = 10, axeCooldownSec = 20, minEnemyRank = 7;
float axeDamage = 2.5f;
float staggerMinDamage = 3.5f, enemyDamage = 1.25f, arrowsPerAmmo = 0.5f;
}

static const char kIniDefault[] =
    "; RECraft settings - edit, save, restart the game\r\n"
    "[RECraft]\r\n"
    "; Hide Leon's model while Minecraft is the player (1 = yes)\r\n"
    "HideLeon=1\r\n"
    "; Also hide him during cutscenes (0 = Leon appears in cutscenes)\r\n"
    "HideLeonInCutscenes=1\r\n"
    "; Keep Minecraft's hotbar/hearts on screen during cutscenes\r\n"
    "MinecraftHudInCutscenes=0\r\n"
    "; Hide Minecraft blocks behind RE4's walls (0 = blocks always drawn on top, like before)\r\n"
    "HideBlocksBehindWalls=1\r\n"
    "; Show your Minecraft character (your skin) in Leon's place during cutscenes\r\n"
    "MinecraftBodyInCutscenes=1\r\n"
    "; RE4 enemy health removed per point of Minecraft damage (fist 1, diamond sword 7; a Ganado has ~400)\r\n"
    "DamagePerMinecraftPoint=45\r\n"
    "; 1 = one shared health: RE4's health bar is the real one, Minecraft's hearts follow it,\r\n"
    ";     and dying in either game kills you in both. 0 = Minecraft's hearts decide instead.\r\n"
    "LinkHealth=1\r\n"
    "; (LinkHealth=0 only) Minecraft damage per point of RE4 damage Leon takes\r\n"
    "HurtScale=0.015\r\n"
    "; Difficulty. An RE4 enemy flinches from a Minecraft hit only if it's a critical hit or deals at least\r\n"
    ";   FlinchMinDamage, and at most once every FlinchCooldownMs (killing blows always play out)\r\n"
    "FlinchCooldownMs=2500\r\n"
    "FlinchMinDamage=3.5\r\n"
    "; % chance that a weaker hit still makes the enemy flinch\r\n"
    "FlinchChance=10\r\n"
    "; The axe is the heavy weapon: one swing every AxeCooldownSec seconds, AxeDamage times harder, always knocks down\r\n"
    "AxeDamage=2.5\r\n"
    "AxeCooldownSec=20\r\n"
    "; RE4's adaptive difficulty (1-10) never drops below this: 7+ = enemies rush and attack more often. 0 = RE4 decides\r\n"
    "MinEnemyRank=7\r\n"
    "DifficultyVersion=4\r\n"
    "; RE4's hits on Leon are multiplied by this (before armour). 1 = RE4's own damage\r\n"
    "EnemyDamage=1.25\r\n"
    "; Arrows per round of RE4 ammo picked up (handgun ammo; shotgun/rifle x2, magnum x3). 1 = one arrow per round\r\n"
    "ArrowsPerAmmo=0.5\r\n"
    "; Performance: Minecraft's HUD at half resolution, shown 2x (same look for the HUD, slightly blockier hand; 4x less copying)\r\n"
    "OverlayHalfRes=0\r\n"
    "; 120 Hz fullscreen: show a new frame every 2nd refresh, an even 60 fps (a frame that runs late waits 33 ms instead of 25)\r\n"
    "HalfRateVsync=0\r\n"
    ;

static void LoadConfig() {
    char ini[MAX_PATH]; snprintf(ini, sizeof ini, "%s\\RECraft.ini", g_dir);
    {   // settings from before the rename (re4craft.ini): carried over once
        char old[MAX_PATH]; snprintf(old, sizeof old, "%s\\re4craft.ini", g_dir);
        if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES && GetFileAttributesA(old) != INVALID_FILE_ATTRIBUTES) {
            if (FILE* in = fopen(old, "rb")) {
                std::string text; char buf[4096]; size_t n;
                while ((n = fread(buf, 1, sizeof buf, in)) > 0) text.append(buf, n);
                fclose(in);
                size_t at;
                while ((at = text.find("re4craft")) != std::string::npos) text.replace(at, 8, "RECraft");
                if (FILE* out = fopen(ini, "wb")) { fwrite(text.data(), 1, text.size(), out); fclose(out); }
                DeleteFileA(old);
            }
        }
    }
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = fopen(ini, "wb")) { fwrite(kIniDefault, 1, sizeof kIniDefault - 1, f); fclose(f); }
    }
    auto I = [&](const char* k, int d) { return (int)GetPrivateProfileIntA("RECraft", k, d, ini); };
    auto F = [&](const char* k, float d) { char b[32]; GetPrivateProfileStringA("RECraft", k, "", b, sizeof b, ini); return b[0] ? (float)atof(b) : d; };
    cfg::hideLeon = I("HideLeon", 1) != 0;
    cfg::hideLeonInCutscenes = I("HideLeonInCutscenes", 1) != 0;
    if (I("DifficultyVersion", 0) < 4) {   // 0.26: Minecraft's HUD no longer shown over cutscenes
        WritePrivateProfileStringA("RECraft", "MinecraftHudInCutscenes", "0", ini);
        if (I("DifficultyVersion", 0) >= 3) WritePrivateProfileStringA("RECraft", "DifficultyVersion", "4", ini);
    }
    cfg::hudInCutscenes = I("MinecraftHudInCutscenes", 0) != 0;
    cfg::hitReactions = I("HitReactions", 0) != 0;
    cfg::damageScale = F("DamagePerMinecraftPoint", 45.0f);
    cfg::hurtScale = F("HurtScale", 0.015f);
    if (I("LinkHealth", -1) == -1) {   // settings file from an older version: add the new option
        WritePrivateProfileStringA("RECraft", "LinkHealth", "1", ini);
    }
    cfg::linkHealth = I("LinkHealth", 1) != 0;
    if (I("MinecraftBodyInCutscenes", -1) == -1) WritePrivateProfileStringA("RECraft", "MinecraftBodyInCutscenes", "1", ini);
    if (I("DoorCollision", -1) == -1) WritePrivateProfileStringA("RECraft", "DoorCollision", "1", ini);
    if (I("MinecraftLowPriority", -1) == -1) WritePrivateProfileStringA("RECraft", "MinecraftLowPriority", "0", ini);
    if (I("EvenRefreshRate", -1) == -1) WritePrivateProfileStringA("RECraft", "EvenRefreshRate", "1", ini);
    if (I("LowLatency", -1) == -1) WritePrivateProfileStringA("RECraft", "LowLatency", "1", ini);
    if (I("PredictMovement", -1) == -1) WritePrivateProfileStringA("RECraft", "PredictMovement", "1", ini);
    cfg::avatarInCutscenes = I("MinecraftBodyInCutscenes", 1) != 0;
    if (I("HideBlocksBehindWalls", -1) == -1) WritePrivateProfileStringA("RECraft", "HideBlocksBehindWalls", "1", ini);
    cfg::blocksBehindWalls = I("HideBlocksBehindWalls", 1) != 0;
    cfg::lowLatency = I("LowLatency", 1) != 0;
    cfg::evenRefresh = I("EvenRefreshRate", 1) != 0;
    if (I("PerfVersion", 0) < 1) {   // 0.32: Minecraft back at normal priority - below normal can put it on a CPU's slow cores (late ticks, Leon snapping back)
        WritePrivateProfileStringA("RECraft", "MinecraftLowPriority", "0", ini); WritePrivateProfileStringA("RECraft", "PerfVersion", "1", ini);
    }
    cfg::mcLowPriority = I("MinecraftLowPriority", 0) != 0;
    cfg::doorCollision = I("DoorCollision", 1) != 0;
    cfg::predictMovement = I("PredictMovement", 1) != 0;
    if (I("OverlayHalfRes", -1) == -1) WritePrivateProfileStringA("RECraft", "OverlayHalfRes", "0", ini);
    if (I("HalfRateVsync", -1) == -1) WritePrivateProfileStringA("RECraft", "HalfRateVsync", "0", ini);
    cfg::overlayHalf = I("OverlayHalfRes", 0) != 0;
    cfg::vsyncHalfRate = I("HalfRateVsync", 0) != 0;
    auto addKey = [&](const char* k, const char* v) { char b[32]; GetPrivateProfileStringA("RECraft", k, "", b, sizeof b, ini); if (!b[0]) WritePrivateProfileStringA("RECraft", k, v, ini); };
    WritePrivateProfileStringA("RECraft", "StaggerCooldownMs", nullptr, ini);   // 0.22's keys, replaced by Flinch*
    WritePrivateProfileStringA("RECraft", "StaggerMinDamage", nullptr, ini);
    if (I("DifficultyVersion", 0) < 2) {   // 0.23's defaults flinched a bit too much: move them once
        WritePrivateProfileStringA("RECraft", "FlinchCooldownMs", "2500", ini); WritePrivateProfileStringA("RECraft", "FlinchChance", "10", ini);
        WritePrivateProfileStringA("RECraft", "DifficultyVersion", "2", ini);
    }
    if (I("DifficultyVersion", 0) < 3) {   // rank 7+ enemies have ~2.5x the health: Minecraft hits count for more, RE4's a bit less extra
        WritePrivateProfileStringA("RECraft", "DamagePerMinecraftPoint", "45", ini); WritePrivateProfileStringA("RECraft", "EnemyDamage", "1.25", ini);
        WritePrivateProfileStringA("RECraft", "DifficultyVersion", "4", ini);
        cfg::damageScale = F("DamagePerMinecraftPoint", 45.0f);
    }
    addKey("FlinchCooldownMs", "2500"); addKey("FlinchMinDamage", "3.5"); addKey("FlinchChance", "10"); addKey("EnemyDamage", "1.25"); addKey("ArrowsPerAmmo", "0.5");
    addKey("AxeDamage", "2.5"); addKey("AxeCooldownSec", "20"); addKey("MinEnemyRank", "7");
    cfg::staggerCooldownMs = I("FlinchCooldownMs", 2500);
    cfg::staggerMinDamage = F("FlinchMinDamage", 3.5f);
    cfg::staggerChance = I("FlinchChance", 10);
    cfg::axeDamage = F("AxeDamage", 2.5f);
    cfg::axeCooldownSec = I("AxeCooldownSec", 20);
    cfg::minEnemyRank = I("MinEnemyRank", 7);
    if (cfg::minEnemyRank > 10) cfg::minEnemyRank = 10;
    cfg::enemyDamage = F("EnemyDamage", 1.25f);
    cfg::arrowsPerAmmo = F("ArrowsPerAmmo", 0.5f);
    if (cfg::enemyDamage <= 0) cfg::enemyDamage = 1.0f;
    if (cfg::arrowsPerAmmo < 0) cfg::arrowsPerAmmo = 0.5f;
    if (cfg::damageScale <= 0) cfg::damageScale = 45.0f;
    if (cfg::hurtScale < 0) cfg::hurtScale = 0.015f;
    Log("settings: HideLeon %d, InCutscenes %d, HudInCutscenes %d, damage x%.1f, hurt x%.3f, shared health %d",
        cfg::hideLeon, cfg::hideLeonInCutscenes, cfg::hudInCutscenes, cfg::damageScale, cfg::hurtScale, cfg::linkHealth);
    Log("difficulty: enemies flinch at most every %d ms from hits of %.1f+ (or crits, or %d%% of weaker ones), enemy damage x%.2f, %.2f arrows per round",
        cfg::staggerCooldownMs, cfg::staggerMinDamage, cfg::staggerChance, cfg::enemyDamage, cfg::arrowsPerAmmo);
    Log("difficulty: axe x%.1f every %d s, enemy rank at least %d", cfg::axeDamage, cfg::axeCooldownSec, cfg::minEnemyRank);
    Log("display: low latency %d, even refresh %d, half-rate vsync %d, half-size Minecraft HUD %d, Minecraft low priority %d, door collision %d",
        cfg::lowLatency, cfg::evenRefresh, cfg::vsyncHalfRate, cfg::overlayHalf, cfg::mcLowPriority, cfg::doorCollision);
}

// Game layout (re4_tweaks SDK): cCoord::pos_94, cCoord::ang_A0, GLOBAL_WK::curRoomId_4FAC
static uint8_t*** g_ppPlayer = nullptr;
static uint8_t*** g_ppGlobal = nullptr;

static DWORD WINAPI Worker(LPVOID) {
    LoadConfig();
    Sleep(4000);  // let the game finish starting
    uint8_t* p;
    if ((p = FindPattern("A1 ? ? ? ? D8 CC D8 C9 D8 CA D9 5D ? D9 45 ?"))) g_ppPlayer = *(uint8_t****)(p + 1);
    if ((p = FindPattern("A1 ? ? ? ? B9 FF FF FF 7F 21 48 ? A1")))          g_ppGlobal = *(uint8_t****)(p + 1);
    if (!g_ppPlayer || !g_ppGlobal) { Log("ERROR: game structures not found - is this RE4 UHD 1.1.0? Link disabled."); return 0; }
    render::g_ppPlayer = g_ppPlayer; render::g_ppGlobal = g_ppGlobal;
    combat::g_ppPlayer = g_ppPlayer; combat::g_ppGlobal = g_ppGlobal; blocks::g_ppGlobal = g_ppGlobal;
    {   // enemy manager: this pattern points at 3 managers (enemies, objects, ...); combat.cpp picks at runtime
        uint8_t* hits[64]; int nh = FindPatternAll("81 E1 01 02 00 00 83 F9 01 75 ? 50 B9 ? ? ? ? E8", hits, 64);
        for (int h = 0; h < nh && h < 64; h++) {
            uint8_t* t = *(uint8_t**)(hits[h] + 0xD); bool dup = false;
            for (int k = 0; k < combat::g_candN; k++) dup |= combat::g_cand[k] == t;
            if (!dup && combat::g_candN < 8) combat::g_cand[combat::g_candN++] = t;
        }
        Log("combat: %d manager candidates", combat::g_candN);
    }
    MH_Initialize();
    uint8_t* cam = FindPattern("52 8D 85 ? ? ? ? 50 8D 4D ? 51 8B CB E8");
    bridge::Open();
    {   char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\RECraft_rooms.bin", g_dir);
        if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) { char o[MAX_PATH]; snprintf(o, sizeof o, "%s\\re4craft_rooms.bin", g_dir); MoveFileA(o, p); }
        collision::Load(p); }
    bool in = input::Install();
    bool rn = render::Install(cam ? cam + 0xE : nullptr);
    render::InstallFinalCamera(FindPattern("8D 53 60 8D BB 04 01 00 00 B9 08 00 00 00 BE ? ? ? ? 52 F3 A5 E8"));
    {   // Leon's attache case (re4_tweaks' ItemMgr / cItemMgr::erase patterns) for the merchant -> Minecraft gear
        // the pattern can match twice in this exe: both copies load the same ItemMgr address
        uint8_t* hits[4]; int nh = FindPatternAll("80 B9 C0 4F 00 00 10 0F 84 ? ? ? ? B9", hits, 4);
        for (int h = 0; h < nh; h++) {
            uint8_t* m = *(uint8_t**)(hits[h] + 0xE);
            Log("gear: ItemMgr candidate %d at %p -> %p", h, hits[h], m);
            if (!combat::g_itemMgr) combat::g_itemMgr = m;
            else if (m != combat::g_itemMgr) Log("gear: candidates disagree - using the first");
        }
        uint8_t* q = FindPattern("E8 ? ? ? ? 8A 45 ? 8B 4D ? 24 ? 66 0F ? ? 8D 04 FD ? ? ? ? 66 0B ? 66 89 53");
        if (q) combat::g_itemErase = (decltype(combat::g_itemErase))(q + 5 + *(int32_t*)(q + 1));
        Log("gear: attache case %p, erase %p", combat::g_itemMgr, (void*)combat::g_itemErase);
    }
    names::Start();   // RE4's mapped items get their Minecraft names
    {   // world item models: the room's ITM block load, and the PC port's pack loader for SS\cmn item models
        uint8_t* hits[8]; int nh = FindPatternAll("68 ? ? ? ? 50 E8 ? ? ? ? 83 C4 0C 3B C3 74 ? 50 E8", hits, 8);
        uint8_t* itm = nullptr;
        for (int h = 0; h < nh && h < 8; h++) {
            const char* tag = *(const char**)(hits[h] + 1);
            if (Readable(tag, 4) && memcmp(tag, "ITM", 4) == 0) itm = hits[h] + 0x13;
        }
        uint8_t* pk[2]; int np = FindPatternAll("05 00 00 00 22 6A 00 50 E8 ? ? ? ? 8B 45 ? 8B 4D ? 68 ? ? ? ? 68 ? ? ? ? 50 51 E8", pk, 2);
        items::Install(itm, np == 1 ? pk[0] + 8 : nullptr, np == 1 ? pk[0] + 31 : nullptr);
    }
    {   uint8_t* hits[4]; int nh = FindPatternAll("68 ? ? ? ? E8 ? ? ? ? 68 00 00 00 F0 E8", hits, 4);   // re4_tweaks' SubScreenWk
        if (nh > 0) render::g_subScreen = *(uint8_t**)(hits[0] + 1);
        Log("menu: %d match(es)", nh); }
    {   uint8_t* hits[4]; int nh = FindPatternAll("75 ? 6A 00 6A 00 68 ? ? ? ? B9 ? ? ? ? E8 ? ? ? ? 84 C0", hits, 4);   // re4_tweaks' EvtMgr
        if (nh > 0) render::g_evtMgr = *(uint8_t**)(hits[0] + 0xC);
        Log("cutscene: event manager at %p (%d match(es))", render::g_evtMgr, nh); }
    {   uint8_t* hits[4]; int nh = FindPatternAll("B9 ? ? ? ? E8 ? ? ? ? 8B ? ? ? ? ? 8B C8 D9", hits, 4);   // re4_tweaks' IdSys
        if (nh > 0) render::g_idSys = *(uint8_t**)(hits[0] + 1);
        Log("hud: ID system at %p (%d match(es))", render::g_idSys, nh); }
    Log("menu: sub screen work at %p", render::g_subScreen);
    Log("ready (input %s, overlay %s). F6 switches control between Minecraft and RE4.", in ? "ok" : "FAILED", rn ? "ok" : "FAILED");
    // Fallback: if RE4 isn't presenting through the hooked Present, keep the link alive from here.
    ULONGLONG start = GetTickCount64();
    for (;;) {
        Sleep(16);
        if (GetTickCount64() - g_lastPresent < 1000) continue;
        static ULONGLONG warned = 0;
        if (!warned && g_lastPresent == 0 && GetTickCount64() - start > 20000) { warned = 1; Log("warning: no Present calls seen yet; running the link from a timer"); }
        bridge::Leon l{};
        uint8_t* pl = (uint8_t*)*g_ppPlayer; uint8_t* gl = (uint8_t*)*g_ppGlobal;
        if (Readable(pl, 0x110) && Readable(gl, 0x4FB0)) {
            float pos[3], ang[3]; memcpy(pos, pl + 0x94, 12); memcpy(ang, pl + 0xA0, 12); memcpy(&l.room, gl + 0x4FAC, 2);
            l.valid = true; l.x = pos[0]; l.y = pos[1]; l.z = pos[2]; l.yawRad = ang[1];
        }
        bridge::Frame(l, true, false, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));   // RE4 keeps control
    }
}

// ------------------------------------------------------------------ winmm forwarding + entry
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        char sys[MAX_PATH]; GetSystemDirectoryA(sys, sizeof sys); strcat(sys, "\\winmm.dll");
        HMODULE real = LoadLibraryA(sys);
        for (int i = 0; i < WINMM_EXPORT_COUNT; i++)
            *g_winmmPtrs[i] = real ? (void*)GetProcAddress(real, g_winmmNames[i]) : nullptr;
        GetModuleFileNameA(inst, g_dir, MAX_PATH);
        char* s = strrchr(g_dir, '\\'); if (s) *s = 0;
        char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\RECraft.log", g_dir);
        g_log = fopen(path, "w");
        Log("RECraft 0.33 loaded (Minecraft drives Leon, room collision, combat, blocks, cutscenes, merchant gear, Minecraft item names)");
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }
    return TRUE;
}
