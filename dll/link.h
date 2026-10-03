// link.h - shared declarations for the RE4 side of the RECraft Minecraft link.
#pragma once
#include <windows.h>
#include <cstdint>

void BridgeLog(const char* fmt, ...);

namespace bridge {

constexpr double kUnitsPerBlock = 1000.0;   // RE4 units (~mm) per Minecraft block

// link protocol enums used on our side
enum : uint16_t { kInKey = 1, kInMouseButton = 2, kInScroll = 3, kInCursor = 4, kInText = 5, kInReleaseAll = 6, kInOpenMenu = 8 };
enum : uint32_t { kMcInWorld = 1, kMcScreenOpen = 2, kMcOnGround = 4, kMcSneaking = 8, kMcDead = 32 };

#pragma pack(push, 1)
struct McState {
    uint32_t seq, flags;
    double   x, y, z;
    float    yaw, pitch, eyeHeight, sensitivity;
    uint32_t teleportAck, guiScale;
    uint64_t frameCounter;
    float    fovDeg, bobPhase, bobAmount;
    uint32_t pad4C;
    double   eyeX, eyeY, eyeZ;
    int64_t  tickQpc;
    double   prevX, prevY, prevZ, curX, curY, curZ;
    float    tickEyeO, tickEye, walkDistO, walkDist, bobO, bob, tickMs;
    uint32_t tickPad;
    uint32_t cameraMode;
    float    cameraDistance;
};
#pragma pack(pop)
static_assert(sizeof(McState) == 0xC8, "McState layout");

struct Leon { bool valid; float x, y, z, yawRad; uint16_t room; };

bool Open();
// Per-frame update. Returns true while Minecraft drives the player ("puppet" mode).
// gameBusy: RE4's gameplay camera isn't running (cutscene, door, ladder, pause...): RE4 keeps Leon,
// Minecraft holds still, and when gameplay comes back Minecraft is re-synced to wherever Leon ended up.
bool Frame(const Leon& leon, bool re4HasControl, bool gameBusy, uint32_t viewportW, uint32_t viewportH);
bool McAlive();
uint8_t* View();   // control area of the shared memory
bool ReadMc(McState& out);

// look (authoritative, MC degrees); mouse counts are added by the input hook
void AddLook(float dx, float dy);
void GetLook(float& yaw, float& pitch);

// input ring (RE4 -> Minecraft)
void PushInput(uint16_t type, uint16_t code, int32_t a = 0, int32_t b = 0, int32_t c = 0);

// overlay (Minecraft -> RE4): returns pixels of the newest frame or nullptr. bottomUp set from slot flags.
const uint8_t* AcquireOverlay(uint32_t& w, uint32_t& h, bool& bottomUp, uint64_t& frameId);

// shared state for the hooks
struct Shared {
    volatile bool puppet = false;        // Minecraft drives Leon
    volatile bool mcScreenOpen = false;  // inventory / chat / options open
    volatile int  cursorX = 0, cursorY = 0;
    volatile uint32_t viewportW = 1280, viewportH = 720;
    float sensitivity = 0.5f;
    // latest interpolated Minecraft eye (blocks) and fov for the camera hook
    double eyeX = 0, eyeY = 0, eyeZ = 0, feetX = 0, feetY = 0, feetZ = 0;
    float fov = 70.0f;
    bool haveEye = false;
    volatile bool busy = false;          // RE4's gameplay camera isn't running (cutscene, door, menu...)
    volatile bool mcReady = false;       // Minecraft player in the world and alive
    volatile bool everPuppet = false;    // Minecraft has driven Leon at least once
};
Shared& S();

} // namespace bridge

// RECraft.ini (next to bio4.exe)
namespace cfg {
extern bool  hideLeon;            // hide Leon's model while Minecraft is the player
extern bool  hideLeonInCutscenes; // ...and during cutscenes
extern bool  hudInCutscenes;      // keep Minecraft's HUD on screen during cutscenes
extern bool  blocksBehindWalls;   // hide Minecraft blocks behind RE4's walls
extern bool  evenRefresh;         // fullscreen at 120/60 Hz instead of e.g. 144 (60 fps judder)
extern bool  mcLowPriority;       // Minecraft below normal priority
extern bool  doorCollision;       // RE4's doors block Minecraft's player while shut
extern bool  lowLatency;          // keep at most one frame queued (less input lag)
extern bool  overlayHalf;         // Minecraft's HUD drawn at half resolution and shown 2x (a quarter of the copying)
extern bool  vsyncHalfRate;       // 120 Hz fullscreen: present every 2nd refresh (an even 60 fps, no 1-3 refresh beat)
extern bool  predictMovement;     // show Leon a Minecraft tick ahead while walking
extern bool  avatarInCutscenes;   // draw the Minecraft player's body in Leon's place during cutscenes
extern float damageScale;         // RE4 hp per point of Minecraft damage
extern float hurtScale;           // Minecraft damage per RE4 hp Leon would lose (LinkHealth=0)
extern bool  linkHealth;          // one shared health (RE4's): hearts follow it, death in either kills both
extern bool  hitReactions;        // put hit enemies into RE4's damage routine (experimental)
extern int   staggerCooldownMs;   // an enemy flinches from Minecraft hits at most this often
extern int   staggerChance;       // % chance a weaker hit still makes it flinch (when not cooling down)
extern float staggerMinDamage;    // Minecraft damage a hit needs to make an enemy flinch (crits always can)
extern float enemyDamage;         // RE4's hits on Leon x this (before armour)
extern float axeDamage;          // the axe hits this many times harder (and always knocks down)
extern int   axeCooldownSec;     // (Minecraft side: seconds between axe swings)
extern int   minEnemyRank;       // RE4's adaptive difficulty never goes below this (1..10, 0 = off)
extern float arrowsPerAmmo;       // arrows per round of RE4 ammo Leon picks up (x the ammo type's rate)
}
