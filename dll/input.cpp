// input.cpp - takes keyboard and mouse away from RE4 while Minecraft drives the player.
//
// RE4 reads input through DirectInput 8. We hook IDirectInputDevice8::GetDeviceState/GetDeviceData
// (found through a throwaway device of our own: every DirectInput device shares these functions),
// forward what we see to Minecraft's input ring, and hand RE4 an empty state.
//
// Keys:  F6 = switch control between Minecraft and RE4 (cutscenes, doors, ladders are handed over automatically)
//        Esc = goes to RE4 (skips cutscenes, pause menu), or closes an open Minecraft screen
//        O = Minecraft's pause/options menu
//        F = RE4's action button (its keyboard "A"/check key, E by default): doors, windows, ladders,
//            items, the merchant... While Minecraft has Leon, F goes to RE4 as E instead of to Minecraft.
// DIK -> SDL table from SkyCraft's Input.cpp (MIT License, (c) 2026 chasmlol).

#define DIRECTINPUT_VERSION 0x0800
#include "link.h"
#include <dinput.h>
#include <cstring>
#include "MinHook.h"

namespace combat { void LogAim(uint8_t* view); }
namespace input {

volatile bool g_re4Control = false;   // F6 toggles

static uint16_t kDikToSdl[256];
static void InitTable() {
    uint16_t* t = kDikToSdl; memset(t, 0, sizeof kDikToSdl);
    t[0x01] = 41;
    for (int i = 0; i < 9; ++i) t[0x02 + i] = (uint16_t)(30 + i);
    t[0x0B] = 39;
    t[0x0C] = 45, t[0x0D] = 46, t[0x0E] = 42, t[0x0F] = 43;
    t[0x10] = 20, t[0x11] = 26, t[0x12] = 8, t[0x13] = 21, t[0x14] = 23;
    t[0x15] = 28, t[0x16] = 24, t[0x17] = 12, t[0x18] = 18, t[0x19] = 19;
    t[0x1A] = 47, t[0x1B] = 48, t[0x1C] = 40, t[0x1D] = 224;
    t[0x1E] = 4, t[0x1F] = 22, t[0x20] = 7, t[0x21] = 9, t[0x22] = 10;
    t[0x23] = 11, t[0x24] = 13, t[0x25] = 14, t[0x26] = 15;
    t[0x27] = 51, t[0x28] = 52, t[0x29] = 53, t[0x2A] = 225, t[0x2B] = 49;
    t[0x2C] = 29, t[0x2D] = 27, t[0x2E] = 6, t[0x2F] = 25, t[0x30] = 5;
    t[0x31] = 17, t[0x32] = 16, t[0x33] = 54, t[0x34] = 55, t[0x35] = 56;
    t[0x36] = 229, t[0x37] = 85, t[0x38] = 226, t[0x39] = 44, t[0x3A] = 57;
    for (int i = 0; i < 10; ++i) t[0x3B + i] = (uint16_t)(58 + i);
    t[0x45] = 83, t[0x46] = 71;
    t[0x47] = 95, t[0x48] = 96, t[0x49] = 97, t[0x4A] = 86;
    t[0x4B] = 92, t[0x4C] = 93, t[0x4D] = 94, t[0x4E] = 87;
    t[0x4F] = 89, t[0x50] = 90, t[0x51] = 91, t[0x52] = 98, t[0x53] = 99;
    t[0x56] = 100, t[0x57] = 68, t[0x58] = 69;
    t[0x9C] = 88, t[0x9D] = 228, t[0xB5] = 84, t[0xB7] = 70, t[0xB8] = 230;
    t[0xC5] = 72, t[0xC7] = 74, t[0xC8] = 82, t[0xC9] = 75, t[0xCB] = 80;
    t[0xCD] = 79, t[0xCF] = 77, t[0xD0] = 81, t[0xD1] = 78, t[0xD2] = 73;
    t[0xD3] = 76, t[0xDB] = 227, t[0xDC] = 231, t[0xDD] = 101;
}

constexpr uint32_t DIK_ESC = 0x01, DIK_O_KEY = 0x18, DIK_F6_KEY = 0x40, DIK_F_KEY = 0x21, DIK_E_KEY = 0x12;

static bool g_keyDown[256];       // what Minecraft has been told
static bool g_btnDown[8];
static bool g_rawKey[256];        // what the keyboard really shows (for edge detection)
static volatile bool g_mouseBuffered = false;


static void SendText(uint32_t dik) {
    UINT vk = MapVirtualKeyA(dik, MAPVK_VSC_TO_VK_EX);
    if (!vk) return;
    BYTE ks[256]; GetKeyboardState(ks);
    ks[VK_SHIFT] = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 0x80 : 0;
    ks[VK_CAPITAL] = (GetKeyState(VK_CAPITAL) & 1) ? 1 : 0;
    WCHAR buf[4]; int n = ToUnicode(vk, dik, ks, buf, 4, 0);
    for (int i = 0; i < n; i++) if (buf[i] >= 32) bridge::PushInput(bridge::kInText, 0, buf[i]);
}

static void ReleaseAllToMc() {
    memset(g_keyDown, 0, sizeof g_keyDown); memset(g_btnDown, 0, sizeof g_btnDown);
    bridge::PushInput(bridge::kInReleaseAll, 0);
}

void SetRe4Control(bool re4) {
    if (re4 == g_re4Control) return;
    g_re4Control = re4;
    ReleaseAllToMc();
    BridgeLog("input: control -> %s", re4 ? "RE4" : "Minecraft");
}

// A keyboard key changed (from either state polling or buffered data). Returns true to let RE4 see it.
static void KeyEdge(uint32_t dik, bool down) {
    if (dik >= 256 || g_rawKey[dik] == down) return;
    g_rawKey[dik] = down;
    auto& S = bridge::S();
    if (dik == DIK_F6_KEY) { if (down) SetRe4Control(!g_re4Control); return; }
    if (!S.puppet) return;
    if (!S.mcScreenOpen) {
        if (dik == DIK_ESC) { if (down) BridgeLog("input: Esc -> RE4 (skip / pause menu)"); return; }   // passed to RE4 (AfterGet*)
        if (dik == DIK_O_KEY) { if (down) { ReleaseAllToMc(); bridge::PushInput(bridge::kInOpenMenu, 0); } return; }
        if (dik == DIK_F_KEY) {   // passed to RE4 as E (AfterGet*)
            if (down) BridgeLog("input: F -> RE4 action (E)");
            return;
        }
    }
    uint16_t sdl = kDikToSdl[dik & 0xFF];
    if (!sdl) return;
    if (g_keyDown[dik] != down) {
        g_keyDown[dik] = down;
        bridge::PushInput(bridge::kInKey, sdl, down ? 1 : 0);
        if (down && S.mcScreenOpen) SendText(dik);
    }
}

// Is the player holding a movement key (W/A/S/D)? Leon's position is predicted a tick ahead while moving.
bool MovementHeld() { return g_keyDown[0x11] || g_keyDown[0x1E] || g_keyDown[0x1F] || g_keyDown[0x20]; }

static void MouseButton(int i, bool down) {
    static const uint16_t kSdl[8] = {1, 3, 2, 4, 5, 0, 0, 0};
    if (i < 0 || i >= 8 || g_btnDown[i] == down || !kSdl[i]) return;
    g_btnDown[i] = down;
    bridge::PushInput(bridge::kInMouseButton, kSdl[i], down ? 1 : 0);
    if (i == 0 && down && !bridge::S().mcScreenOpen && bridge::McAlive()) combat::LogAim(bridge::View());
}

static void MouseMove(long dx, long dy) {
    auto& S = bridge::S();
    if (S.mcScreenOpen) {
        if (cfg::overlayHalf) {   // the cursor lives in the overlay's pixels: half as many when it's half size
            static long rx = 0, ry = 0; rx += dx; ry += dy; dx = rx / 2; dy = ry / 2; rx -= dx * 2; ry -= dy * 2;
        }
        int x = S.cursorX + dx, y = S.cursorY + dy;
        if (x < 0) x = 0; if (y < 0) y = 0;
        if (x >= (int)S.viewportW) x = S.viewportW - 1; if (y >= (int)S.viewportH) y = S.viewportH - 1;
        S.cursorX = x; S.cursorY = y;
        bridge::PushInput(bridge::kInCursor, 0, x, y);
    } else {
        bridge::AddLook((float)dx, (float)dy);
    }
}

// ------------------------------------------------------------------ device type cache
static DWORD DevType(IDirectInputDevice8A* dev) {
    static IDirectInputDevice8A* lastDev[8]; static DWORD lastType[8]; static int n = 0;
    for (int i = 0; i < n; i++) if (lastDev[i] == dev) return lastType[i];
    DIDEVCAPS caps; caps.dwSize = sizeof caps;
    DWORD type = SUCCEEDED(dev->GetCapabilities(&caps)) ? GET_DIDEVICE_TYPE(caps.dwDevType) : 0;
    if (n < 8) { lastDev[n] = dev; lastType[n] = type; n++; }
    return type;
}

// ------------------------------------------------------------------ hooks
typedef HRESULT (WINAPI *GetStateFn)(IDirectInputDevice8A*, DWORD, LPVOID);
typedef HRESULT (WINAPI *GetDataFn)(IDirectInputDevice8A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
static GetStateFn g_getState[2] = {};
static GetDataFn  g_getData[2] = {};

static HRESULT AfterGetState(HRESULT hr, DWORD cb, LPVOID data);
static HRESULT AfterGetData(HRESULT hr, IDirectInputDevice8A* dev, DWORD cb, LPDIDEVICEOBJECTDATA od, LPDWORD inOut, DWORD flags);
template<int W> static HRESULT WINAPI GetStateHook(IDirectInputDevice8A* dev, DWORD cb, LPVOID data) {
    return AfterGetState(g_getState[W](dev, cb, data), cb, data);
}
template<int W> static HRESULT WINAPI GetDataHook(IDirectInputDevice8A* dev, DWORD cb, LPDIDEVICEOBJECTDATA od, LPDWORD inOut, DWORD flags) {
    return AfterGetData(g_getData[W](dev, cb, od, inOut, flags), dev, cb, od, inOut, flags);
}

static HRESULT AfterGetState(HRESULT hr, DWORD cb, LPVOID data) {
    if (FAILED(hr) || !data) return hr;
    if (cb == 256) {                                   // keyboard
        BYTE* k = (BYTE*)data;
        for (int i = 0; i < 256; i++) KeyEdge(i, (k[i] & 0x80) != 0);
        if (bridge::S().puppet) {
            bool f = (k[DIK_F_KEY] & 0x80) && !bridge::S().mcScreenOpen;
            bool esc = (k[DIK_ESC] & 0x80) && !bridge::S().mcScreenOpen;
            memset(k, 0, 256);
            if (f) k[DIK_E_KEY] = 0x80;   // RE4's action key
            if (esc) k[DIK_ESC] = 0x80;   // RE4's skip / pause menu
        }
    } else if (cb == sizeof(DIMOUSESTATE) || cb == sizeof(DIMOUSESTATE2)) {   // mouse
        DIMOUSESTATE2* m = (DIMOUSESTATE2*)data;
        int nb = cb == sizeof(DIMOUSESTATE) ? 4 : 8;
        if (bridge::S().puppet) {
            if (!g_mouseBuffered) {
                if (m->lX || m->lY) MouseMove(m->lX, m->lY);
                if (m->lZ) bridge::PushInput(bridge::kInScroll, 0, m->lZ > 0 ? 120 : -120);
            }
            for (int i = 0; i < nb; i++) MouseButton(i, (m->rgbButtons[i] & 0x80) != 0);
            memset(data, 0, cb);
        } else {
            for (int i = 0; i < nb; i++) g_btnDown[i] = false;
        }
    }
    return hr;
}

static HRESULT AfterGetData(HRESULT hr, IDirectInputDevice8A* dev, DWORD cb, LPDIDEVICEOBJECTDATA od, LPDWORD inOut, DWORD flags) {
    if (FAILED(hr) || !inOut || !od || (flags & DIGDD_PEEK)) return hr;
    DWORD type = DevType(dev);
    DWORD n = *inOut;
    for (DWORD i = 0; i < n; i++) {
        DIDEVICEOBJECTDATA* e = (DIDEVICEOBJECTDATA*)((BYTE*)od + i * cb);
        if (type == DI8DEVTYPE_KEYBOARD) {
            KeyEdge(e->dwOfs & 0xFF, (e->dwData & 0x80) != 0);
        } else if (type == DI8DEVTYPE_MOUSE && bridge::S().puppet) {
            if (e->dwOfs == DIMOFS_X) { g_mouseBuffered = true; MouseMove((LONG)e->dwData, 0); }
            else if (e->dwOfs == DIMOFS_Y) { g_mouseBuffered = true; MouseMove(0, (LONG)e->dwData); }
            else if (e->dwOfs == DIMOFS_Z) bridge::PushInput(bridge::kInScroll, 0, (LONG)e->dwData > 0 ? 120 : -120);
            else if (e->dwOfs >= DIMOFS_BUTTON0 && e->dwOfs <= DIMOFS_BUTTON7)
                MouseButton(e->dwOfs - DIMOFS_BUTTON0, (e->dwData & 0x80) != 0);
        }
    }
    if (bridge::S().puppet && type == DI8DEVTYPE_KEYBOARD) {   // keep only F, renamed to RE4's action key
        DWORD kept = 0;
        for (DWORD i = 0; i < n; i++) {
            DIDEVICEOBJECTDATA* e = (DIDEVICEOBJECTDATA*)((BYTE*)od + i * cb);
            DWORD k = e->dwOfs & 0xFF;
            if ((k != DIK_F_KEY && k != DIK_ESC) || bridge::S().mcScreenOpen) continue;
            DIDEVICEOBJECTDATA* dst = (DIDEVICEOBJECTDATA*)((BYTE*)od + kept * cb);
            if (dst != e) memcpy(dst, e, cb);
            if (k == DIK_F_KEY) dst->dwOfs = DIK_E_KEY;
            kept++;
        }
        *inOut = kept;
    } else if (bridge::S().puppet && type == DI8DEVTYPE_MOUSE) *inOut = 0;
    return hr;
}

static LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

bool Install() {
    InitTable();
    char path[MAX_PATH]; GetSystemDirectoryA(path, MAX_PATH); strcat(path, "\\dinput8.dll");
    HMODULE di = LoadLibraryA(path);   // the real one (RE4's folder has re4_tweaks' dinput8 wrapper)
    auto create = di ? (HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN))GetProcAddress(di, "DirectInput8Create") : nullptr;
    if (!create) { BridgeLog("input: no DirectInput8Create"); return false; }
    WNDCLASSA wc{}; wc.lpfnWndProc = DummyProc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "RECraft_di";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("RECraft_di", "", 0, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    bool ok = false;
    for (int wide = 0; wide < 2; wide++) {
        void* dinput = nullptr;
        if (FAILED(create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, wide ? IID_IDirectInput8W : IID_IDirectInput8A, &dinput, nullptr)) || !dinput) continue;
        IDirectInputDevice8A* dev = nullptr;
        if (SUCCEEDED(((IDirectInput8A*)dinput)->CreateDevice(GUID_SysKeyboard, &dev, nullptr)) && dev) {
            void** vt = *(void***)dev;
            void* gs = vt[9]; void* gd = vt[10];
            MH_STATUS a = MH_CreateHook(gs, wide ? (void*)&GetStateHook<1> : (void*)&GetStateHook<0>, (void**)&g_getState[wide]);
            MH_STATUS b = MH_CreateHook(gd, wide ? (void*)&GetDataHook<1> : (void*)&GetDataHook<0>, (void**)&g_getData[wide]);
            if (a == MH_OK) MH_EnableHook(gs);
            if (b == MH_OK) MH_EnableHook(gd);
            BridgeLog("input: %s device hooks %s/%s", wide ? "W" : "A", MH_StatusToString(a), MH_StatusToString(b));
            ok |= (a == MH_OK || a == MH_ERROR_ALREADY_CREATED);
            dev->Release();
        }
        ((IDirectInput8A*)dinput)->Release();
    }
    if (hwnd) DestroyWindow(hwnd);
    return ok;
}

} // namespace input
