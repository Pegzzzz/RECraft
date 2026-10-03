// bridge.cpp - RE4 side of the SkyCraft link: shared memory, state, input ring, overlay, collision.
// Layout follows SkyCraft/protocol/skycraft_protocol.h (MIT License, (c) 2026 chasmlol), protocol v11.
//
// Coordinates: 1 Minecraft block = 1000 RE4 units; axes map 1:1 (x->x, y->y, z->z).

#include "link.h"
#include <cstring>
#include <cmath>
#include <vector>

namespace input { bool MovementHeld(); }
namespace collision { bool TakeDoors(uint32_t epoch, std::vector<uint8_t>& msgs); bool Available(); void Request(uint16_t room, float lx, float ly, float lz, uint32_t epoch); bool Take(uint32_t epoch, std::vector<uint8_t>& msgs); }

namespace bridge {

constexpr uint32_t kMagic = 0x43594B53, kVersion = 11;
constexpr uint64_t kOffSky = 0x100, kOffMc = 0x200, kOffOvlCtl = 0x300, kOffOvlHdr = 0x340,
                   kOffInput = 0x1000, kOffActors = 0x12000, kOffEvents = 0x17000,
                   kOffEntities = 0x1C000, kOffCol = 0x20000, kColBytes = 32ull << 20;
constexpr uint64_t kOffPixels = kOffCol + kColBytes;
constexpr uint64_t kSlotBytes = 3840ull * 2160 * 4;
constexpr uint64_t kOffRender = kOffPixels + kSlotBytes * 3;
constexpr uint64_t kRenderBytes = 64ull << 20;
constexpr uint64_t kMappingBytes = kOffRender + kRenderBytes;
constexpr uint64_t kColData = kColBytes - 0x80;
constexpr uint32_t kInputEntries = 4096;

#pragma pack(push, 1)
struct SkyState {
    uint32_t seq, flags, worldId, collisionEpoch;
    double posX, posY, posZ;
    float yaw, pitch;
    uint32_t teleportSeq, viewportW, viewportH;
    float gameHour;
};
struct ColRegion { int32_t minX, minY, minZ, maxX, maxY, maxZ; uint32_t epoch, count; };
struct ColTri { float v[9]; uint32_t flags; };
struct InputEvent { uint16_t type, code; int32_t a, b, c; };
struct OvlHdr { uint32_t width, height, flags, pad; uint64_t frameId; };
#pragma pack(pop)
static_assert(sizeof(SkyState) == 0x40, "SkyState layout");

static Shared   g_shared;
Shared& S() { return g_shared; }

static HANDLE   g_map = nullptr;
static uint8_t* g_view = nullptr;   // control + collision area only (RE4 is a 32-bit game)
static uint32_t g_skySeq = 0, g_epoch = 1, g_teleportSeq = 1;
static uint64_t g_colHead = 0;
static bool     g_mcConnected = false;
static uint32_t g_lastMcPid = 0;
static int      g_floorKey[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
static uint32_t g_lastRoom = 0xFFFFFFFF;
static CRITICAL_SECTION g_inputLock;
static volatile LONG g_lookLock = 0;
static float    g_yaw = 0, g_pitch = 0;
static bool     g_lookInit = false;
static bool     g_wasPuppet = false;
static bool     g_needCollision = true;   // (re)send the room's collision
static bool     g_colDoors = false;       // the pending messages are a door update
static uint32_t g_mcTeleportAck = 0;      // last teleport Minecraft confirmed
static ULONGLONG g_teleportAt = 0;        // when the current teleport was asked for
uint32_t CollisionEpoch() { return g_epoch; }
uint32_t CurrentRoom() { return g_lastRoom; }
static int      g_tickN = 0, g_tickLate = 0, g_snaps = 0; static double g_tickSum = 0, g_tickWorst = 0;
// Minecraft tick timing since the last call (perf line): count, average and worst gap, gaps over 75 ms, snap-backs
void TickStats(int& n, double& avg, double& worst, int& late, int& snaps) {
    n = g_tickN; avg = g_tickN ? g_tickSum / g_tickN : 0; worst = g_tickWorst; late = g_tickLate; snaps = g_snaps;
    g_tickN = g_tickLate = g_snaps = 0; g_tickSum = g_tickWorst = 0;
}
static int      g_colMode = 0;            // 0 idle, 1 building, 2 sent, 3 flat-floor fallback
static std::vector<uint8_t> g_colPending; static size_t g_colPendingAt = 0;

template<class T> static T& At(uint64_t off) { return *reinterpret_cast<T*>(g_view + off); }

bool Open() {
    if (g_view) return true;
    InitializeCriticalSection(&g_inputLock);
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                               (DWORD)(kMappingBytes >> 32), (DWORD)kMappingBytes, L"Local\\SkyCraft_v1");
    if (!g_map) { BridgeLog("bridge: CreateFileMapping failed (%lu)", GetLastError()); return false; }
    bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    g_view = (uint8_t*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)kOffPixels);
    if (!g_view) { BridgeLog("bridge: MapViewOfFile failed (%lu)", GetLastError()); CloseHandle(g_map); g_map = nullptr; return false; }
    memset(g_view, 0, 0x100);
    memset(g_view + kOffSky, 0, 0x100);
    memset(g_view + kOffOvlCtl, 0, 0x100);
    memset(g_view + kOffInput, 0, 0x80);
    memset(g_view + kOffActors, 0, 0x40);
    memset(g_view + kOffEvents, 0, 0x80);
    memset(g_view + kOffEntities, 0, 0x40);
    memset(g_view + kOffCol, 0, 0x80);
    At<uint32_t>(0x0) = kMagic;
    At<uint32_t>(0x4) = kVersion;
    At<uint32_t>(0x8) = GetCurrentProcessId();
    BridgeLog("bridge: shared memory ready (%s, %.0f MB, protocol v%u)", existed ? "reused" : "new",
              kMappingBytes / 1048576.0, kVersion);
    return true;
}

// ------------------------------------------------------------------ look
static void LookLock()   { while (InterlockedExchange(&g_lookLock, 1)) YieldProcessor(); }
static void LookUnlock() { InterlockedExchange(&g_lookLock, 0); }

void AddLook(float dx, float dy) {
    // Minecraft's own mouse formula, so sensitivity matches Minecraft's option
    float s = g_shared.sensitivity * 0.6f + 0.2f;
    float f = s * s * s * 8.0f * 0.15f;
    LookLock();
    g_yaw = fmodf(g_yaw + dx * f, 360.0f);
    g_pitch += dy * f;
    if (g_pitch > 90) g_pitch = 90;
    if (g_pitch < -90) g_pitch = -90;
    LookUnlock();
}
void GetLook(float& yaw, float& pitch) { LookLock(); yaw = g_yaw; pitch = g_pitch; LookUnlock(); }
static void SetLook(float yaw, float pitch) { LookLock(); g_yaw = yaw; g_pitch = pitch; LookUnlock(); }

// RE4 heading (radians, 0 = facing +Z, forward = (sin, cos)) <-> Minecraft yaw (degrees, forward = (-sin, cos))
static float HeadingToMcYaw(float h) { return -h * 57.2957795f; }
float McYawToHeading(float yaw) { return -yaw * 0.0174532925f; }

// ------------------------------------------------------------------ input ring
void PushInput(uint16_t type, uint16_t code, int32_t a, int32_t b, int32_t c) {
    if (!g_view) return;
    EnterCriticalSection(&g_inputLock);
    uint64_t head = At<volatile uint64_t>(kOffInput);
    uint64_t tail = At<volatile uint64_t>(kOffInput + 0x40);
    if (head - tail < kInputEntries) {
        InputEvent& e = At<InputEvent>(kOffInput + 0x80 + (head % kInputEntries) * 16);
        e.type = type; e.code = code; e.a = a; e.b = b; e.c = c;
        MemoryBarrier();
        At<volatile uint64_t>(kOffInput) = head + 1;
    }
    LeaveCriticalSection(&g_inputLock);
}

// ------------------------------------------------------------------ Minecraft state
uint8_t* View() { return g_view; }

bool McAlive() {
    if (!g_view) return false;
    uint64_t beat = At<volatile uint64_t>(0x18);
    return beat && GetTickCount64() - beat < 3000;
}

bool ReadMc(McState& m) {
    if (!g_view) return false;
    for (int tries = 0; tries < 100; tries++) {
        uint32_t s1 = At<volatile uint32_t>(kOffMc);
        if (s1 & 1) { YieldProcessor(); continue; }
        MemoryBarrier(); memcpy(&m, g_view + kOffMc, sizeof m); MemoryBarrier();
        if (At<volatile uint32_t>(kOffMc) == s1) return true;
    }
    return false;
}

// ------------------------------------------------------------------ overlay (triple buffer, MC writes)
static uint8_t* g_slotView[3] = {};
static uint64_t g_slotMapped[3] = {};
static uint8_t* g_slotBase[3] = {};
static uint32_t g_front = 2;   // SkyCraft convention: state 0 = middle slot 0, reader holds 2, writer 1

static uint8_t* MapSlot(uint32_t i, uint64_t bytes) {
    if (g_slotView[i] && g_slotMapped[i] >= bytes) return g_slotBase[i];
    if (g_slotView[i]) { UnmapViewOfFile(g_slotView[i]); g_slotView[i] = nullptr; }
    SYSTEM_INFO si; GetSystemInfo(&si);
    uint64_t off = kOffPixels + kSlotBytes * i;
    uint64_t aligned = off & ~(uint64_t)(si.dwAllocationGranularity - 1);
    uint64_t lead = off - aligned;
    g_slotView[i] = (uint8_t*)MapViewOfFile(g_map, FILE_MAP_READ, (DWORD)(aligned >> 32), (DWORD)aligned, (SIZE_T)(bytes + lead));
    if (!g_slotView[i]) { BridgeLog("bridge: overlay slot %u map failed (%lu)", i, GetLastError()); return nullptr; }
    g_slotMapped[i] = bytes;
    g_slotBase[i] = g_slotView[i] + lead;
    return g_slotBase[i];
}

const uint8_t* AcquireOverlay(uint32_t& w, uint32_t& h, bool& bottomUp, uint64_t& frameId) {
    if (!g_view) return nullptr;
    volatile LONG* state = (volatile LONG*)(g_view + kOffOvlCtl);
    if (*state & 4) {
        LONG prev = InterlockedExchange(state, (LONG)g_front);
        g_front = (uint32_t)prev & 3;
    } else {
        return nullptr;   // nothing new
    }
    if (g_front > 2) return nullptr;
    OvlHdr hd; memcpy(&hd, g_view + kOffOvlHdr + g_front * 0x40, sizeof hd);
    if (hd.width == 0 || hd.height == 0 || hd.width > 3840 || hd.height > 2160) return nullptr;
    w = hd.width; h = hd.height; bottomUp = hd.flags & 1; frameId = hd.frameId;
    return MapSlot(g_front, (uint64_t)w * h * 4);
}

// ------------------------------------------------------------------ collision (temporary flat floor)
static void WriteCol(uint32_t type, const void* payload, uint32_t bytes) {
    uint64_t tail = At<volatile uint64_t>(kOffCol + 0x40);
    uint32_t msg = (8 + bytes + 7) & ~7u;
    if (g_colHead + msg + 8 - tail > kColData) return;
    uint64_t pos = g_colHead % kColData;
    if (pos + msg > kColData) {
        uint32_t* pad = (uint32_t*)(g_view + kOffCol + 0x80 + pos); pad[0] = 0; pad[1] = 0;
        g_colHead += kColData - pos; pos = 0;
    }
    uint8_t* base = g_view + kOffCol + 0x80 + pos;
    ((uint32_t*)base)[0] = type; ((uint32_t*)base)[1] = bytes;
    memcpy(base + 8, payload, bytes);
    MemoryBarrier();
    g_colHead += msg;
    At<volatile uint64_t>(kOffCol) = g_colHead;
}

// Flat floor at the height RE4 itself puts Leon on (RE4 keeps snapping him to its real floor).
// Phase 3 replaces this with RE4's room collision.
static void StreamFloor(double fx, double fy, double fz, bool force) {
    int rx = (int)floor(fx / 8), rz = (int)floor(fz / 8);
    int fyq = (int)floor(fy * 8 + 0.5);
    if (!force && g_floorKey[0] == rx && g_floorKey[1] == fyq && g_floorKey[2] == rz) return;
    g_floorKey[0] = rx; g_floorKey[1] = fyq; g_floorKey[2] = rz;
    g_epoch++;
    WriteCol(1, &g_epoch, 4);
    float y = fyq / 8.0f;
    int ry = (int)floor((y - 0.001) / 8);
    for (int dz = -2; dz <= 2; dz++)
        for (int dx = -2; dx <= 2; dx++) {
            for (int dy = -2; dy <= 1; dy++) {   // empty regions so Minecraft knows the area
                if (dy == 0) continue;
                int ex = (rx + dx) * 8, ey = (ry + dy) * 8, ez = (rz + dz) * 8;
                ColRegion none{ex, ey, ez, ex + 7, ey + 7, ez + 7, g_epoch, 0};
                WriteCol(2, &none, sizeof none);
            }
            int ax = (rx + dx) * 8, az = (rz + dz) * 8, ay = ry * 8;
            struct { ColRegion r; ColTri t[2]; } msg{};
            msg.r = {ax, ay, az, ax + 7, ay + 7, az + 7, g_epoch, 2};
            float x0 = (float)ax, z0 = (float)az, x1 = x0 + 8, z1 = z0 + 8;
            msg.t[0] = {{x0, y, z0, x1, y, z0, x1, y, z1}, 0};
            msg.t[1] = {{x0, y, z0, x1, y, z1, x0, y, z1}, 0};
            WriteCol(3, &msg, sizeof msg);
            ColRegion empty{ax, ay, az, ax + 7, ay + 7, az + 7, g_epoch, 0};
            WriteCol(2, &empty, sizeof empty);
        }
}

// ------------------------------------------------------------------ room collision streaming
static double g_floorY = 0;

static void UpdateCollision(const Leon& leon, bool puppet) {
    if (g_needCollision) {
        g_needCollision = false;
        g_epoch++;
        WriteCol(1, &g_epoch, 4);   // Minecraft drops everything older
        g_colPending.clear(); g_colPendingAt = 0; g_colDoors = false;
        if (collision::Available()) {
            collision::Request(leon.room, leon.x, leon.y, leon.z, g_epoch);
            g_colMode = 1;
        } else {
            g_colMode = 3; g_floorKey[0] = INT32_MIN;
        }
    }
    if (g_colMode == 1 && collision::Take(g_epoch, g_colPending)) {
        g_colPendingAt = 0;
        g_colMode = g_colPending.empty() ? 3 : 2;   // a room without data falls back to a flat floor
        if (g_colMode == 3) g_floorKey[0] = INT32_MIN;
    }
    // Feed the prepared messages into the ring as space allows (Minecraft drains it every tick).
    while (g_colMode == 2 && g_colPendingAt < g_colPending.size()) {
        const uint8_t* m = g_colPending.data() + g_colPendingAt;
        uint32_t type, len; memcpy(&type, m, 4); memcpy(&len, m + 4, 4);
        uint64_t tail = At<volatile uint64_t>(kOffCol + 0x40);
        uint32_t msg = (8 + len + 7) & ~7u;
        if (g_colHead + msg + 8 - tail > kColData) break;   // full: continue next frame
        WriteCol(type, m + 8, len);
        g_colPendingAt += msg;
        if (g_colPendingAt >= g_colPending.size()) { if (!g_colDoors) BridgeLog("bridge: room collision sent (%.1f MB)", g_colPending.size() / 1048576.0); }
    }
    // doors opening and closing: only the regions they touch, after the room itself has gone out
    if (g_colMode == 2 && g_colPendingAt >= g_colPending.size()) {
        std::vector<uint8_t> d;
        if (collision::TakeDoors(g_epoch, d) && !d.empty()) { g_colPending.swap(d); g_colPendingAt = 0; g_colDoors = true; }
    }
    // Minecraft still holding the player 8 s after the room went out: it never got ground under him
    // (Leon's position was stale when the room was asked for) - ask again from where he is now.
    {
        static ULONGLONG sentAt = 0; static uint32_t sentEpoch = 0, retried = 0;
        if (g_colMode == 2 && g_colPendingAt >= g_colPending.size() && sentEpoch != g_epoch) { sentEpoch = g_epoch; sentAt = GetTickCount64(); }
        // (timed from the later of the room going out and the teleport being asked for: a teleport after a
        // pick-up or a door, minutes after the room was sent, used to set this off at once - 0.30's log)
        ULONGLONG since = sentAt > g_teleportAt ? sentAt : g_teleportAt;
        if (sentEpoch == g_epoch && sentAt && g_mcTeleportAck != g_teleportSeq && GetTickCount64() - since > 8000 && retried != g_epoch && leon.valid) {
            retried = g_epoch + 1; sentAt = 0;
            BridgeLog("bridge: Minecraft still waiting for ground after 8 s - rebuilding room %04X around Leon (%.1f %.1f %.1f)", leon.room, leon.x / 1000.0, leon.y / 1000.0, leon.z / 1000.0);
            g_needCollision = true;
        }
    }
    if (g_colMode == 3) StreamFloor(puppet ? g_shared.feetX : leon.x / kUnitsPerBlock, g_floorY, puppet ? g_shared.feetZ : leon.z / kUnitsPerBlock, false);
}

// ------------------------------------------------------------------ per frame   // RE4's floor under Leon (blocks), sampled while RE4 owns him

bool Frame(const Leon& leon, bool re4Control, bool gameBusy, uint32_t vw, uint32_t vh) {
    bool re4HasControl = re4Control || gameBusy;
    if (!Open()) return false;
    At<volatile uint64_t>(0x10) = GetTickCount64();
    g_shared.viewportW = vw; g_shared.viewportH = vh;

    McState mc{};
    bool alive = McAlive();
    if (alive && cfg::mcLowPriority) {   // RE4 first: Minecraft runs hidden next to it and competes for the CPU in big fights
        static uint32_t donePid = 0;
        uint32_t pid = At<volatile uint32_t>(0x0C);   // SkyCraft link header: Minecraft's process id
        if (pid && pid != donePid) {
            donePid = pid;
            HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
            BOOL ok = h && SetPriorityClass(h, BELOW_NORMAL_PRIORITY_CLASS);
            if (h) CloseHandle(h);
            BridgeLog("perf: Minecraft (pid %u) %s", pid, ok ? "set to below-normal priority - RE4 gets the CPU first" : "priority unchanged");
        }
    }
    bool haveMc = alive && ReadMc(mc);
    uint32_t mcPid = At<volatile uint32_t>(0xC);
    if (alive != g_mcConnected || (alive && mcPid != g_lastMcPid)) {
        BridgeLog("bridge: Minecraft %s (pid %u)", alive ? "CONNECTED" : "not connected", mcPid);
        if (alive) {
            g_teleportSeq++; g_teleportAt = GetTickCount64(); g_floorKey[0] = INT32_MIN; g_needCollision = true;
            InterlockedExchange((volatile LONG*)(g_view + kOffOvlCtl), 0); g_front = 2;   // reset the overlay triple buffer
        }
        g_mcConnected = alive; g_lastMcPid = mcPid;
    }
    if (haveMc && mc.sensitivity > 0) g_shared.sensitivity = mc.sensitivity;
    bool screen = haveMc && (mc.flags & kMcScreenOpen);
    if (screen && !g_shared.mcScreenOpen) { g_shared.cursorX = vw / 2; g_shared.cursorY = vh / 2; }
    g_shared.mcScreenOpen = screen;

    double lx = leon.x / kUnitsPerBlock, ly = leon.y / kUnitsPerBlock, lz = leon.z / kUnitsPerBlock;

    if (leon.valid && leon.room != g_lastRoom) {
        g_lastRoom = leon.room; g_teleportSeq++; g_teleportAt = GetTickCount64(); g_floorKey[0] = INT32_MIN; g_needCollision = true;
        BridgeLog("bridge: room %04X, Leon at (%.2f %.2f %.2f)", leon.room, lx, ly, lz);
    }

    bool mcReady = haveMc && (mc.flags & kMcInWorld) && !(mc.flags & kMcDead);
    g_shared.mcReady = mcReady;
    g_shared.busy = gameBusy;
    bool wantPuppet = leon.valid && !re4HasControl && mcReady;
    static bool wasWant = false;
    if (wantPuppet && !wasWant) {
        // Minecraft takes Leon back (after F6, a cutscene, a door, a pause...): if RE4 moved him, first
        // move the Minecraft player to wherever RE4 left him, so Leon doesn't snap back. If he's still
        // where Minecraft's player stands (a pause, a short hiccup), nothing to do: no teleport, no jolt.
        double dx = mc.x - lx, dy = mc.y - ly, dz = mc.z - lz;
        if (dx * dx + dz * dz > 0.35 * 0.35 || fabs(dy) > 0.6) {
            g_teleportSeq++; g_teleportAt = GetTickCount64();
            SetLook(HeadingToMcYaw(leon.yawRad), 0);
        }
    }
    wasWant = wantPuppet;
    if (haveMc) g_mcTeleportAck = mc.teleportAck;
    bool puppet = wantPuppet && mc.teleportAck == g_teleportSeq;
    if (puppet) g_shared.everPuppet = true;
    if (!wantPuppet || !g_lookInit) {   // (the rising edge above also re-seeds the look)
        // RE4 owns Leon: Minecraft follows him, and the look starts from his heading
        SetLook(HeadingToMcYaw(leon.yawRad), 0);
        g_lookInit = leon.valid;
        g_floorY = ly;
    }
    if (g_wasPuppet && !puppet && leon.valid) PushInput(kInReleaseAll, 0);   // Minecraft player stops where it is
    if (puppet != g_wasPuppet) BridgeLog("bridge: puppet %s", puppet ? "ON (Minecraft drives Leon)" : "off");
    g_wasPuppet = puppet;
    g_shared.puppet = puppet;

    // Minecraft player's eye/feet. Minecraft moves its player 20 times a second and its own renderer shows
    // the position between the last two ticks - a tick (50 ms) behind. While a movement key is held we show
    // it a tick AHEAD instead (the last tick's motion carried on), which takes that 50 ms out of walking and
    // stopping; small corrections (a wall, letting go) are eased in over a few frames.
    if (haveMc && puppet && mc.tickQpc) {   // how evenly Minecraft's ticks arrive (for the perf line): late ones make Leon stall and snap
        static int64_t lastTick = 0;
        if (mc.tickQpc != lastTick) {
            if (lastTick) {
                static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
                double ms = (double)(mc.tickQpc - lastTick) * 1000.0 / f.QuadPart;
                if (ms > 0 && ms < 2000) { g_tickN++; g_tickSum += ms; if (ms > g_tickWorst) g_tickWorst = ms; if (ms > 75) g_tickLate++; }
            }
            lastTick = mc.tickQpc;
        }
    }
    if (haveMc) {
        double fx = mc.x, fy = mc.y, fz = mc.z, eye = mc.eyeHeight;
        if (mc.tickQpc && mc.tickMs > 0) {
            LARGE_INTEGER f, now; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&now);
            double t = (double)(now.QuadPart - mc.tickQpc) / (f.QuadPart / 1000.0) / mc.tickMs;
            if (t < 0) t = 0; if (t > 1) t = 1;
            double vx = mc.curX - mc.prevX, vz = mc.curZ - mc.prevZ;
            bool jump = vx * vx + vz * vz > 4.0;   // a teleport, not a step
            static double sx = 0, sz = 0; static bool have = false; static double lastT = -1;
            double tx, tz;
            if (cfg::predictMovement && !jump && input::MovementHeld()) { tx = mc.curX + vx * t; tz = mc.curZ + vz * t; }
            else if (cfg::predictMovement && !jump) { tx = mc.curX; tz = mc.curZ; }
            else { tx = mc.prevX + vx * t; tz = mc.prevZ + vz * t; }
            if (!have || jump || !cfg::predictMovement) { sx = tx; sz = tz; have = true; }
            else {
                double ex = tx - sx, ez = tz - sz;
                if (puppet && ex * vx + ez * vz < 0 && ex * ex + ez * ez > 0.15 * 0.15) g_snaps++;   // shown ahead, pulled back
                if (ex * ex + ez * ez > 1.0) { sx = tx; sz = tz; }   // far off: just go there
                else { sx += ex * 0.55; sz += ez * 0.55; }
            }
            (void)lastT;
            fx = sx; fz = sz;
            fy = mc.prevY + (mc.curY - mc.prevY) * t;   // height stays Minecraft's own (landing, stairs)
            eye = mc.tickEyeO + (mc.tickEye - mc.tickEyeO) * t;
        }
        g_shared.feetX = fx; g_shared.feetY = fy; g_shared.feetZ = fz;
        g_shared.eyeX = fx; g_shared.eyeY = fy + eye; g_shared.eyeZ = fz;
        g_shared.fov = mc.fovDeg > 1 ? mc.fovDeg : 70.0f;
        g_shared.haveEye = true;
    }

    float yaw, pitch; GetLook(yaw, pitch);
    SkyState& s = At<SkyState>(kOffSky);
    g_skySeq++;
    *(volatile uint32_t*)&s.seq = g_skySeq * 2 - 1; MemoryBarrier();
    s.flags = (leon.valid ? 1u : 0u) | (re4HasControl ? 2u : 0u);
    s.worldId = 0x52450000u | leon.room;
    s.collisionEpoch = g_epoch;
    s.posX = lx; s.posY = ly + 0.02; s.posZ = lz;   // 2 cm up: a hair below the floor made SkyCraft wait 6 s for ground
    s.yaw = yaw; s.pitch = pitch;
    s.teleportSeq = g_teleportSeq;
    s.viewportW = vw; s.viewportH = vh;
    s.gameHour = 20.0f;
    MemoryBarrier(); *(volatile uint32_t*)&s.seq = g_skySeq * 2;

    if (leon.valid && g_mcConnected) UpdateCollision(leon, puppet);

    static ULONGLONG lastLog = 0;
    if (haveMc && GetTickCount64() - lastLog > 3000) {
        lastLog = GetTickCount64();
        BridgeLog("bridge: MC (%.2f %.2f %.2f) flags 0x%X ack %u/%u | Leon (%.2f %.2f %.2f) look %.0f/%.0f puppet %d",
                  mc.x, mc.y, mc.z, mc.flags, mc.teleportAck, g_teleportSeq, lx, ly, lz, yaw, pitch, puppet);
    }
    return puppet;
}

} // namespace bridge
