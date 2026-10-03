// names.cpp - RE4's items get their Minecraft names (and short descriptions) while RECraft maps them.
//
// RE4 keeps every message file in memory as loaded from disk (core.udas: item names, one table per
// language; SS/<lang>/ss_item.dat: item descriptions). A table is u32 id, u32 count, u32 ofs[count]
// (relative to the table), then the texts as u16 words: control codes < 0x80, glyphs from 0x80
// (western font: space 0x80, '0' 0x83, 'A' 0xA7, 'a' 0xC1, é 0xF0, É 0xF6, è 0xE6, à 0xE5).
// A background thread finds the item tables (count 272) and points the entries of the mapped items at
// new texts of our own (offsets wrap around in 32 bits, so the texts can live anywhere).
#include "link.h"
namespace bridge { uint32_t CurrentRoom(); }
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>

namespace names {

static uint16_t Glyph(uint32_t c) {
    if (c >= 'a' && c <= 'z') return (uint16_t)(0xC1 + (c - 'a'));
    if (c >= 'A' && c <= 'Z') return (uint16_t)(0xA7 + (c - 'A'));
    if (c >= '0' && c <= '9') return (uint16_t)(0x83 + (c - '0'));
    switch (c) {
    case ' ': return 0x80; case '.': return 0x95; case ',': return 0x94; case '-': return 0x91;
    case '(': return 0x98; case ')': return 0x99; case '\'': return 0x123;
    case 0xE9: return 0xF0;   // é
    case 0xC9: return 0xF6;   // É
    case 0xE8: return 0xE6;   // è
    case 0xE0: return 0xE5;   // à
    case '\n': return 0x03;   // new line
    default: return 0x80;
    }
}
static std::vector<uint16_t> Enc(const char* utf8) {
    std::vector<uint16_t> out;
    const unsigned char* s = (const unsigned char*)utf8;
    while (*s) {
        uint32_t c = *s++;
        if (c >= 0xC0 && *s) { c = ((c & 0x1F) << 6) | (*s++ & 0x3F); }
        out.push_back(Glyph(c));
    }
    return out;
}

struct Entry { uint16_t id; const char* en; const char* fr; int desc; };
// desc: 0 none (keep RE4's), 1 sword, 2 axe, 3 bow, 4 protection, 5 golden apple, 6-8 armour cases
static const Entry kEntries[] = {
    {35, "Wooden Sword", "\xC3\x89p\xC3\xA9" "e en bois", 1},
    {33, "Stone Sword", "\xC3\x89p\xC3\xA9" "e en pierre", 1}, {64, "Stone Sword", "\xC3\x89p\xC3\xA9" "e en pierre", 1},
    {37, "Iron Sword", "\xC3\x89p\xC3\xA9" "e en fer", 1}, {38, "Iron Sword", "\xC3\x89p\xC3\xA9" "e en fer", 1},
    {39, "Diamond Sword", "\xC3\x89p\xC3\xA9" "e en diamant", 1},
    {3, "Netherite Sword", "\xC3\x89p\xC3\xA9" "e en netherite", 1}, {41, "Netherite Sword", "\xC3\x89p\xC3\xA9" "e en netherite", 1},
    {42, "Netherite Sword", "\xC3\x89p\xC3\xA9" "e en netherite", 1}, {55, "Netherite Sword", "\xC3\x89p\xC3\xA9" "e en netherite", 1},
    {44, "Stone Axe", "Hache en pierre", 2}, {148, "Iron Axe", "Hache en fer", 2}, {45, "Diamond Axe", "Hache en diamant", 2},
    {52, "Netherite Axe", "Hache en netherite", 2},
    {46, "Bow (Power I)", "Arc (Puissance I)", 3}, {107, "Bow (Power I)", "Arc (Puissance I)", 3},
    {47, "Bow (Power II)", "Arc (Puissance II)", 3}, {108, "Bow (Power II)", "Arc (Puissance II)", 3}, {81, "Bow (Power II)", "Arc (Puissance II)", 3},
    {48, "Protection Book", "Livre de Protection", 4}, {50, "Protection Book", "Livre de Protection", 4}, {62, "Protection Book", "Livre de Protection", 4},
    {125, "Leather Armour (M)", "Armure de cuir (M)", 6}, {126, "Iron Armour (L)", "Armure de fer (L)", 7},
    {127, "Diamond Armour (XL)", "Armure diamant (XL)", 8},
    {5, "Golden Apple", "Pomme dor\xC3\xA9" "e", 5},
    {53, "Firework Rocket", "Fus\xC3\xA9" "e d'artifice", 0}, {54, "Crossbow", "Arbal\xC3\xA8" "te", 0},
    {34, "Spyglass", "Longue-vue", 0}, {68, "Spyglass", "Longue-vue", 0}, {69, "Spyglass", "Longue-vue", 0},
    {170, "Spyglass", "Longue-vue", 0},
    {4, "Arrows", "Fl\xC3\xA8" "ches", 0}, {24, "Arrows", "Fl\xC3\xA8" "ches", 0}, {7, "Arrows", "Fl\xC3\xA8" "ches", 0},
    {0, "Arrows", "Fl\xC3\xA8" "ches", 0}, {26, "Arrows", "Fl\xC3\xA8" "ches", 0}, {32, "Arrows", "Fl\xC3\xA8" "ches", 0},
    {106, "Arrows", "Fl\xC3\xA8" "ches", 0},
    // grenades become Minecraft throwables
    {1, "TNT", "TNT", 0}, {14, "Ender Pearl", "Perle de l'Ender", 0}, {2, "Fire Charge", "Boule de feu", 0},
    // herbs become steak in Minecraft (one per herb)
    {6, "Steak", "Steak", 0}, {25, "Steak", "Steak", 0}, {28, "Steak", "Steak", 0},
    {18, "2 Steaks", "2 steaks", 0}, {20, "2 Steaks", "2 steaks", 0}, {22, "2 Steaks", "2 steaks", 0}, {168, "2 Steaks", "2 steaks", 0},
    {19, "3 Steaks", "3 steaks", 0}, {21, "3 Steaks", "3 steaks", 0},
    // pesetas lying around are emeralds
    {0x78, "Emeralds", "\xC3\x89meraudes", 0}, {0x79, "Emeralds", "\xC3\x89meraudes", 0},
};
static const char* kDescEn[9] = {"",
    "Your Minecraft sword and bow. Tune-ups add\nSharpness, Power, Punch, Flame and more.",
    "Your Minecraft axe. Tune-ups add Sharpness,\nFire Aspect and Knockback.",
    "Your Minecraft bow. Tune-ups add Power,\nPunch, Flame and Infinity.",
    "Puts Protection on your Minecraft armour.\nFirepower tune-ups raise it.",
    "Heals you completely, in both games.",
    "Leather armour for you in Minecraft,\nand a bigger attache case.",
    "Iron armour for you in Minecraft,\nand a bigger attache case.",
    "Diamond armour for you in Minecraft,\nand the biggest attache case."};
static const char* kDescFr[9] = {"",
    "Votre \xC3\xA9p\xC3\xA9" "e et votre arc Minecraft. Les am\xC3\xA9liorations\ndonnent Tranchant, Puissance, Frappe, Flamme...",
    "Votre hache Minecraft. Les am\xC3\xA9liorations donnent\nTranchant, Aura de feu et Recul.",
    "Votre arc Minecraft. Les am\xC3\xA9liorations donnent\nPuissance, Frappe, Flamme et Infinit\xC3\xA9.",
    "Met Protection sur votre armure Minecraft.\nLa puissance de feu l'augmente.",
    "Vous soigne compl\xC3\xA8" "tement, dans les deux jeux.",
    "Une armure de cuir pour vous dans Minecraft,\net une mallette plus grande.",
    "Une armure de fer pour vous dans Minecraft,\net une mallette plus grande.",
    "Une armure en diamant dans Minecraft,\net la plus grande mallette."};

static const uint32_t kCount = 272, kOfs0 = 8 + 4 * 272;
static uint8_t* g_buf = nullptr; static size_t g_used = 0;
// our texts: [lang 0 en, 1 fr][kind 0 name, 1 description][entry]
static uint8_t* g_text[2][2][sizeof kEntries / sizeof kEntries[0]];

static uint8_t* Put(const std::vector<uint16_t>& w) {
    size_t n = w.size() * 2;
    if (g_used + n > 0x10000) return nullptr;
    uint8_t* p = g_buf + g_used; memcpy(p, w.data(), n); g_used += (n + 3) & ~3u;
    return p;
}
static void Build() {
    g_buf = (uint8_t*)VirtualAlloc(nullptr, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_buf) return;
    for (int l = 0; l < 2; l++)
        for (size_t i = 0; i < sizeof kEntries / sizeof kEntries[0]; i++) {
            const Entry& e = kEntries[i];
            std::vector<uint16_t> w{0x0000};
            auto t = Enc(l ? e.fr : e.en); w.insert(w.end(), t.begin(), t.end());
            w.insert(w.end(), {0x80, 0x0E, 0x08, 0x01});
            g_text[l][0][i] = Put(w);
            g_text[l][1][i] = nullptr;
            if (e.desc) {
                std::vector<uint16_t> d{0x0000, 0x80, 0x80, 0x11, e.id, 0x03};
                auto u = Enc(l ? kDescFr[e.desc] : kDescEn[e.desc]); d.insert(d.end(), u.begin(), u.end());
                d.insert(d.end(), {0x80, 0x08, 0x01});
                g_text[l][1][i] = Put(d);
            }
        }
}

// The game can free memory while we look, so every read and write goes through Read/WriteProcessMemory
// (which fail instead of faulting).
static bool Rd(const uint8_t* p, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
}
static bool InOurs(uint32_t ofs, uint8_t* t) { uint8_t* q = t + ofs; return q >= g_buf && q < g_buf + 0x10000; }

// What kind of table this is: -1 not ours to touch, else lang*2 + kind (kind 0 names, 1 descriptions)
static int Classify(uint8_t* t, const uint32_t* ofs) {
    if (InOurs(ofs[35], t)) return -1;                              // already done
    if (ofs[35] < kOfs0 || ofs[35] > 0x80000) return -1;
    uint16_t m[8];
    if (!Rd(t + ofs[35], m, sizeof m)) return -1;
    static const uint16_t kPistolet[] = {0xB6, 0xC9, 0xD3, 0xD4, 0xCF, 0xCC, 0xC5, 0xD4};   // "Pistolet"
    if (m[0] != 0) return -1;
    if (m[1] == 0x80 && m[2] == 0x80 && m[3] == 0x11 && m[4] == 35 && m[5] == 0x03) {   // a description
        if (m[6] >= 0x200) return -1;
        return m[6] == 0xB6 ? 3 : 1;                                // "Pistolet de 9mm..." French
    }
    if (m[1] > 0x80 && m[1] < 0x200) {                              // a name
        uint16_t f[8]; memcpy(f, m + 1, 7 * 2); Rd(t + ofs[35] + 16, f + 7, 2);
        if (!memcmp(f, kPistolet, sizeof kPistolet)) return 2;      // French
        return 0;                                                   // English (and other western languages)
    }
    return -1;
}

static void Patch(uint8_t* t, int cls) {
    int lang = cls / 2, kind = cls % 2;
    int n = 0;
    for (size_t i = 0; i < sizeof kEntries / sizeof kEntries[0]; i++) {
        uint8_t* txt = g_text[lang][kind][i];
        if (!txt) continue;
        uint32_t o = (uint32_t)(txt - t); SIZE_T put = 0;
        if (WriteProcessMemory(GetCurrentProcess(), t + 8 + 4 * kEntries[i].id, &o, 4, &put)) n++;
    }
    BridgeLog("names: %s %s table at %p - %d items renamed", lang ? "French" : "English", kind ? "description" : "item name", t, n);
}

// Areas (allocations) where tables turned up: the game keeps loading its message files into the same heap,
// so most scans only look there; the whole address space is swept rarely (it costs a moment of CPU).
static std::vector<void*> g_hot;
static int Scan(bool full) {
    int found = 0, chunks = 0;
    static std::vector<uint32_t> chunk(0x40000 + 2);   // 1 MB (+8 bytes overlap)
    SYSTEM_INFO si; GetSystemInfo(&si);
    uint8_t* p = (uint8_t*)si.lpMinimumApplicationAddress;
    MEMORY_BASIC_INFORMATION mi;
    while (p < (uint8_t*)si.lpMaximumApplicationAddress && VirtualQuery(p, &mi, sizeof mi)) {
        uint8_t* base = (uint8_t*)mi.BaseAddress; size_t size = mi.RegionSize;
        p = base + size;
        if (mi.State != MEM_COMMIT || !(mi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) || (mi.Protect & PAGE_GUARD)) continue;
        if (base == g_buf) continue;
        bool hot = false; for (void* h : g_hot) hot |= h == mi.AllocationBase;
        if (!full && !hot) continue;
        for (size_t off = 0; off < size; off += 0x100000) {
            size_t n = size - off; if (n > 0x100000 + 8) n = 0x100000 + 8;
            if (full && ++chunks % 16 == 0) Sleep(1);   // stay out of the game's way
            if (!Rd(base + off, chunk.data(), n)) continue;
            size_t nw = n / 4;
            for (size_t i = 1; i + 1 < nw; i++) {
                if (chunk[i] != kCount || chunk[i + 1] != kOfs0) continue;
                uint8_t* t = base + off + (i - 1) * 4;
                if (t >= (uint8_t*)chunk.data() && t < (uint8_t*)(chunk.data() + chunk.size())) continue;   // our own read buffer
                static uint32_t ofs[272];
                if (!Rd(t + 8, ofs, sizeof ofs)) continue;
                int cls = Classify(t, ofs);
                if (cls < 0) continue;
                Patch(t, cls); found++;
                if (!hot) { g_hot.push_back(mi.AllocationBase); hot = true; }
            }
        }
    }
    return found;
}

static DWORD WINAPI Thread(LPVOID) {
    Build();
    if (!g_buf) return 0;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    Sleep(8000);
    bool wasBusy = false; ULONGLONG busySince = 0, last = 0, lastFull = 0, roomAt = 0; int quick = 0, roomLooks = 0;
    uint32_t lastRoom = 0xFFFFFFFF;
    for (;;) {
        Sleep(250);
        ULONGLONG now = GetTickCount64();
        bool busy = bridge::S().busy;
        if (busy && !wasBusy) { busySince = now; quick = 0; }
        wasBusy = busy;
        uint32_t room = bridge::CurrentRoom();
        if (room != lastRoom) { lastRoom = room; roomAt = now; roomLooks = 0; }
        // Tables are (re)loaded with a room and when a menu (shop, case) opens: look right after those -
        // twice after a room change, up to three times after a menu - and otherwise only once a minute.
        // Quick looks cover the areas where tables were found before; a full sweep of the game's memory
        // (a moment of CPU and memory bandwidth) only runs while nothing has been found yet, or every 5 minutes.
        // (0.30 looked every 20 seconds and swept everything every 2 minutes, in the middle of fights too.)
        bool due = now - last > 60000;
        if (busy && quick < 3 && now - busySince > (ULONGLONG)(400 + quick * 900)) { due = true; quick++; }
        if (roomLooks < 2 && now - roomAt > (ULONGLONG)(1500 + roomLooks * 4500)) { due = true; roomLooks++; }
        if (g_hot.empty() && now - last > 20000) due = true;
        if (!due) continue;
        last = now;
        bool full = g_hot.empty() || now - lastFull > 300000;
        if (full) lastFull = now;
        Scan(full);
    }
}

void Start() { CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr); }

} // namespace names
