#include "plugin.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include "mm/imgui/imgui.h"
#include <mm/game/go/character.h>
#include "mm/game/go/vehicle.h"
#include "mm/game/cameramanager.h"
#include "mm/core/input.h"
// DbgHelp.h defines its own ADDRESS macro, which would clobber the SDK's
// ADDRESS(gog, steam); keep ours.
#pragma push_macro("ADDRESS")
#undef ADDRESS
#include <DbgHelp.h>
#undef ADDRESS
#pragma pop_macro("ADDRESS")
#pragma comment(lib, "dbghelp.lib")

// ---- Build mode (2026-09-17) ----
// This file doubles as (a) the distributable "Convoy Respawn Mod" -- the
// only thing a normal user installing this .asi should get: the convoy fix
// below, on by default, plus a small status overlay -- and (b) our own dev
// sandbox for whatever we're reverse-engineering next (storm/vehicle test
// hooks, airbrake, position readout). Real end users never asked for any of
// that and shouldn't have random hotkeys doing unrelated things, so it's
// gated behind this flag instead of living in a separate project/build.
// Set to 1 for our own testing; a distributed build must ship with 0.
#define MM_DEV_TOOLS 0

// World repopulation (2026-10-14..16): liberated camps, scavenging locations and sniper towers get their enemies back after a
// cooldown, and road encounters ignore the region's threat level (camp_repop.inc, threat_diag.inc). Uses fixed addresses of the
// GOG build, so it switches itself off on any other executable. Can ship without MM_DEV_TOOLS (no keys, no overlay lines).
#define MM_WORLD_REPOP 1

// Storm presets moved to their own plugin, Wasteland Storms (WastelandStorms.asi,
// 2026-09-23). Kept here, switched off, so both can be installed side by side
// without two plugins answering F5/F11 and firing storms.
#define MM_STORM_TOOLS 0

// Unattended camp test (2026-11-10): only the autotest mode of MM_DEV_TOOLS (scripts\autotest.txt, tools\repop\autotest.ps1), with its
// camp commands, without the dev keys and overlay. Test builds only; a distributed build ships with 0.
#define MM_AUTOTEST 0
#if MM_AUTOTEST
#define MM_AUTOTEST_TAG " autotest"
#else
#define MM_AUTOTEST_TAG ""
#endif

// User-requested (2026-09-18): always show a visible tag for whatever
// build is currently installed, so it's never ambiguous which test is
// running. Bump this string every time a new test build goes out.
#define MM_BUILD_TAG "v0.9.5-beta2 (2026-11-27)" MM_AUTOTEST_TAG

// ---- Session event log (2026-09-21) ----
// Replaces screenshot-driven debugging: every notable event (wreck seen,
// respawn armed/fired, composition patch, crash) is appended with a
// timestamp to scripts\convoy_respawn_log.txt, plus a snapshot of all 14
// convoys every 60 s and at game exit (DLL_PROCESS_DETACH). Append+close per
// line so the file survives a crash. Truncated at each game start.
static DWORD g_logSessionStart = 0;
static char g_modDir[MAX_PATH] = "";   // folder of this .asi; set in DllMain (the CWD at attach time is not the game folder)
static char g_logPath[MAX_PATH] = "convoy_respawn_log.txt";
static char g_crashPath[MAX_PATH] = "convoy_crash_stack.txt";
static char g_dumpPath[MAX_PATH] = "convoy_composition_dump.txt";
static void InitModPaths(HMODULE self) {
    if (GetModuleFileNameA(self, g_modDir, MAX_PATH)) {
        char* slash = strrchr(g_modDir, '\\');
        if (slash) slash[1] = 0;
        snprintf(g_logPath, MAX_PATH, "%sconvoy_respawn_log.txt", g_modDir);
        snprintf(g_crashPath, MAX_PATH, "%sconvoy_crash_stack.txt", g_modDir);
        snprintf(g_dumpPath, MAX_PATH, "%sconvoy_composition_dump.txt", g_modDir);
    }
}
static void LogLine(const char* fmt, ...) {
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "a") != 0 || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    DWORD t = GetTickCount() - g_logSessionStart;
    fprintf(f, "[%02d:%02d:%02d +%5lu.%01lus] ", st.wHour, st.wMinute, st.wSecond, (unsigned long)(t / 1000), (unsigned long)((t % 1000) / 100));
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}
static void LogSessionStart() {
    g_logSessionStart = GetTickCount();
    // keep the previous session's log (a quick relaunch used to wipe it)
    char prev[MAX_PATH];
    snprintf(prev, MAX_PATH, "%sconvoy_respawn_log.previous.txt", g_modDir);
    MoveFileExA(g_logPath, prev, MOVEFILE_REPLACE_EXISTING);
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "w") == 0 && f) fclose(f);
}

// ---- Game address resolution by byte signature (v0.9.1, 2026-09-21) ----
// beta1 hard-coded every address and refused any executable but the GOG
// 2015-12-07 build. Steam users reported that immediately. The Steam exe is
// a different build (the SDK's own GOG/Steam address pairs differ by
// non-constant deltas), so instead of fixed addresses every function this
// mod hooks or calls is now located at startup by scanning the loaded
// executable's .text section for a byte pattern taken from the GOG build
// (relative operands wildcarded; each pattern verified to match exactly
// once there, see gen_sigs.py). A pattern that matches zero or several
// times fails the check. All required patterns resolved -> the mod runs;
// otherwise it stays loaded but inert and tells the user what failed.
//
// Untested on Steam at the time of writing: the code bytes are expected to
// match (same compiler, same 1.0.3 source), and the struct offsets used
// elsewhere in this file (CGraphScriptGameObject +0xE0/+0xF0/+0xF8,
// CConvoyDataContainer +0xD8/+0x118, CProcessor +0xA0, CGameObject vtable
// slot 0x138) are assumed identical between builds.
static bool g_gameVersionOk = false;
static void LogConvoySnapshot(const char* why);
typedef void (*SendEventMsgFn)(const char* msg);   // NEvent::CSendEvent<...>::SendMsg; defined in the dev block below
extern SendEventMsgFn SendEventMsg;
static void DetectConvoyMapPoints();
extern int g_convoyMapPoints;
extern char g_convoyMapStatus[128];
static char g_gameVersionStatus[240] = "not checked";

struct GameSignature {
    const char* name;
    const char* pattern;    // "48 8B ?? ..." -- '??' is a wildcard byte
    bool required;          // release-critical; optional ones only gate dev features
    uintptr_t resolved;
    int matches;
    bool alreadyHooked;     // found behind another mod's hook (E9 jmp at the start)
};
static GameSignature g_sigs[] = {
    { "NGSONodes::ConvoyDataSetWrecked",       "40 57 48 83 EC 40 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 50 48 89 6C 24 58 48 89 74 24 60 48 8B FA 48 8B F1 E8 ?? ?? ?? ?? 48 8B E8", true, 0, 0, false },
    { "CGameObject::FindOptional",             "48 8B C4 57 48 83 EC 70 48 C7 44 24 48 FE FF FF FF 48 89 58 08 48 89 70 10", true, 0, 0, false },
    { "CPlayer::UpdateController",             "48 8B C4 41 54 48 81 EC 90 00 00 00 48 C7 44 24 20 FE FF FF FF 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 0F 29 70 E8 0F 28 F1 48 8B E9 F3 0F 10 81 00 03 00 00", true, 0, 0, false },
    { "CGameObject::AddToUpdate",              "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 01 80 A1 8C 00 00 00 FE", true, 0, 0, false },
    { "CGraphScriptGameObject::UpdatePostSim", "40 57 48 83 EC 40 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 58 48 89 74 24 60 48 8B F2 48 8B D9 48 81 C1 C0 00 00 00", true, 0, 0, false },
    { "CAIConstantsProfilesManager::ResolveConvoysCompositionCritical", "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 49 60 49 8B F0 8B FA", false, 0, 0, false },
    { "composition table accessor",            "0F B7 01 4C 8D 04 80 48 8B 41 08 48 8B 40 58 4A 8B 04 C0 48 89 02 33 C0", false, 0, 0, false },
    { "AI constants manager singleton (mov rcx,[rip] in IterateGuards)", "48 8B 0D ?? ?? ?? ?? 4C 8D 44 24 20 8B D3 48 89 6C 24 20 E8", false, 0, 0, false },
    { "NGSONodes::SpawnStorm",                 "48 8B C4 55 41 54 41 55 41 56 41 57 48 8D 68 C8 48 81 EC 10 01 00 00 48 C7 45 D8 FE FF FF FF", false, 0, 0, false },
    { "CCharacter::GetVehiclePtr",             "48 89 5C 24 10 57 48 83 EC 30 48 81 C1 C0 01 00 00 48 8D 54 24 20 48 8B 01 FF 50 60 48 8B 5C 24 28 48 8B 38", false, 0, 0, false },
    { "NGraphScript::CProcessor::FireStart",   "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 40 8B FA 48 8B D9", false, 0, 0, false },
};
enum { SIG_SETWRECKED = 0, SIG_FINDOPTIONAL, SIG_UPDATECONTROLLER, SIG_ADDTOUPDATE, SIG_UPDATEPOSTSIM,
       SIG_RESOLVECOMPOSITION, SIG_TABLEACCESSOR, SIG_MANAGERPTR, SIG_SPAWNSTORM, SIG_GETVEHICLEPTR, SIG_FIRESTART, SIG_COUNT };

// The Steam build (exe timestamp 0x5628A448) runs its code from the
// protector's .xpdata section, re-encoded: register-to-register instructions
// use the other ModRM direction (GOG `33 C0` xor eax,eax is `31 C0` there,
// GOG `4C 8B E1` mov r12,rcx is `49 89 CC`); the composition table accessor
// was found there byte-identical but for that. The empty REX prefix of a
// prologue push (`40 57` push rdi) is dropped there as well (steamtest4). These are the GOG patterns with
// every reg-reg instruction rewritten that way (analysis/steam/steam_forms.py,
// 2026-11-24), tried when the GOG form is not found. nullptr = the pattern has
// no reg-reg instruction, its GOG form is the Steam form.
static const char* g_steamPatterns[] = {
    "57 48 83 EC 40 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 50 48 89 6C 24 58 48 89 74 24 60 48 89 D7 48 89 CE E8 ?? ?? ?? ?? 48 89 C5",
    "48 89 E0 57 48 83 EC 70 48 C7 44 24 48 FE FF FF FF 48 89 58 08 48 89 70 10",
    "48 89 E0 41 54 48 81 EC 90 00 00 00 48 C7 44 24 20 FE FF FF FF 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 0F 29 70 E8 0F 28 F1 48 89 CD F3 0F 10 81 00 03 00 00",
    nullptr,
    "57 48 83 EC 40 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 58 48 89 74 24 60 48 89 D6 48 89 CB 48 81 C1 C0 00 00 00",
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 49 60 4C 89 C6 89 D7",
    "0F B7 01 4C 8D 04 80 48 8B 41 08 48 8B 40 58 4A 8B 04 C0 48 89 02 31 C0",
    "48 8B 0D ?? ?? ?? ?? 4C 8D 44 24 20 89 DA 48 89 6C 24 20 E8",
    "48 89 E0 55 41 54 41 55 41 56 41 57 48 8D 68 C8 48 81 EC 10 01 00 00 48 C7 45 D8 FE FF FF FF",
    nullptr,
    "57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 40 89 D7 48 89 CB",
};
static_assert(sizeof(g_steamPatterns) / sizeof(g_steamPatterns[0]) == SIG_COUNT, "one Steam form per signature");
static bool g_sigSteamForm[SIG_COUNT];

typedef void* (*CharacterGetVehicleFn)(void* character);
static CharacterGetVehicleFn CharacterGetVehiclePtr = nullptr; // from g_sigs[SIG_GETVEHICLEPTR]
typedef bool (*FireStartFn)(void* processor, unsigned int startNameHash);
static FireStartFn ProcessorFireStart = nullptr;   // from g_sigs[SIG_FIRESTART]
static bool g_overlayAvailable = false;   // the SDK's own hard-coded addresses (overlay) only fit two known builds

static uintptr_t g_exeBase = 0;
static uintptr_t g_scanStart[4], g_scanEnd[4];      // the code section(s) the signatures are looked for in
static int g_scanRegions = 0;
static unsigned int g_exeTimeStamp = 0, g_exeImageSize = 0;

static bool ScanBytes(const unsigned char* bytes, const bool* wild, int len, GameSignature& sig) {
    sig.matches = 0; sig.resolved = 0;
    for (int r = 0; r < g_scanRegions && sig.matches < 2; r++) {
        const unsigned char* p = (const unsigned char*)g_scanStart[r];
        const unsigned char* end = (const unsigned char*)g_scanEnd[r] - len;
        __try {                                          // the pages were checked readable; a protector may still change them
            for (; p <= end; p++) {
                if (!wild[0] && p[0] != bytes[0]) continue;
                int i = 1;
                for (; i < len; i++) if (!wild[i] && p[i] != bytes[i]) break;
                if (i == len) {
                    if (++sig.matches == 1) sig.resolved = (uintptr_t)p;
                    else break;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { sig.matches = 0; sig.resolved = 0; break; }
    }
    return sig.matches == 1;
}

// Another ASI may already have hooked the same function (Enhanced Convoys and
// Wasteland Storms both hook CPlayer::UpdateController). MinHook replaces the
// first 5 bytes with `jmp rel32` (E9 xx xx xx xx) and leaves every byte after
// them untouched, so if the plain pattern finds nothing, look for exactly
// that: E9 + 4 wildcards + the original pattern from byte 5 on.
static bool ScanPatternText(GameSignature& sig, const char* pattern) {
    unsigned char bytes[128]; bool wild[128]; int len = 0;
    for (const char* c = pattern; *c && len < 128; ) {
        while (*c == ' ') c++;
        if (!*c) break;
        if (c[0] == '?') { wild[len] = true; bytes[len] = 0; len++; c += 2; continue; }
        unsigned int v = 0; sscanf_s(c, "%2x", &v); bytes[len] = (unsigned char)v; wild[len] = false; len++; c += 2;
    }
    sig.alreadyHooked = false;
    if (ScanBytes(bytes, wild, len, sig)) return true;
    if (sig.matches != 0 || len < 12) return false;
    bytes[0] = 0xE9; wild[0] = false;
    for (int i = 1; i < 5; i++) wild[i] = true;
    if (!ScanBytes(bytes, wild, len, sig)) return false;
    sig.alreadyHooked = true;
    return true;
}

// GOG form first, then the Steam form (see g_steamPatterns).
static bool ScanPattern(GameSignature& sig) {
    int i = (int)(&sig - g_sigs);
    g_sigSteamForm[i] = false;
    if (ScanPatternText(sig, sig.pattern) || sig.matches || !g_steamPatterns[i]) return sig.matches == 1;
    g_sigSteamForm[i] = ScanPatternText(sig, g_steamPatterns[i]);
    return g_sigSteamForm[i];
}

// Signatures found in their Steam form are used as they are on the Steam build
// they were tested on (Pablo's own copy, 2026-11-26); on any other build only
// when <game>\scripts\steam_try.txt exists (the opt-in of the Steam test).
static const unsigned STEAM_EXE_TIMESTAMP = 0x5628A448u;
static bool SteamTryEnabled() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* slash = strrchr(path, '\\'); if (slash) slash[1] = 0;
    strcat_s(path, "scripts\\steam_try.txt");
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static int SteamFormsUsed() {
    int n = 0;
    for (int i = 0; i < SIG_COUNT; i++) if (g_sigs[i].matches == 1 && g_sigSteamForm[i]) n++;
    return n;
}

// ----------------------------------------------------------- code section --
// Which section holds the game's code. The GOG build names it .text. The Steam
// build a tester ran on 2026-11-23 (exe timestamp 0x5628A448) is wrapped by a
// protector that renames every section (.bss .data1 .data ... .xpdata .xtext):
// the game's code is the first one, ".bss" (BaseOfCode, the size of GOG's
// .text), decrypted a few seconds after start; the protector's ".xpdata" holds
// code too (some game functions matched there). So without a .text the code
// section is the one holding BaseOfCode, else the first executable one, and
// the scan waits until it is decrypted, then also covers every other big
// section full of code. Nothing is read before VirtualQuery says the pages
// are readable.
static char g_codeSection[96] = "none";
static bool g_codeReady = false;     // renamed sections: the code section is decrypted and was scanned
static int g_lastCodeRuns = -1, g_stableChecks = 0;
static bool g_deferred = false;      // installing from the waiting thread: no message box (it would sit behind the starting game)

static bool RangeReadable(uintptr_t p, size_t n) {
    uintptr_t end = p + n;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
        if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            || (mbi.Protect & PAGE_GUARD)) return false;
        p = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}

// Compiled code is full of int3 (CC) padding between functions; encrypted code
// has almost none. Counts CC CC CC CC runs; -1 = not readable.
static int CountPadding(const uint8_t* p, size_t n) {
    int runs = 0;
    __try {
        for (size_t k = 0; k + 4 <= n; k += 4)
            if (*(const uint32_t*)(p + k) == 0xCCCCCCCCu) runs++;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return runs;
}

static int SectionPaddingRuns(const IMAGE_SECTION_HEADER* sec, uintptr_t base) {
    size_t n = sec->Misc.VirtualSize < 0x1000000 ? sec->Misc.VirtualSize : 0x1000000;   // first 16 MB
    if (!RangeReadable(base + sec->VirtualAddress, n)) return -1;
    return CountPadding((const uint8_t*)(base + sec->VirtualAddress), n);
}

static int PickCodeSection(const IMAGE_SECTION_HEADER* sec, int i, const char* why) {
    char name[9] = { 0 }; memcpy(name, sec->Name, 8);
    snprintf(g_codeSection, sizeof(g_codeSection), "%s (%s)", name, why);
    return i;
}

static int CodeSectionIndex(const IMAGE_NT_HEADERS64* nt, uintptr_t base) {
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int n = nt->FileHeader.NumberOfSections;
    for (int i = 0; i < n; i++)
        if (memcmp(sec[i].Name, ".text", 5) == 0) return PickCodeSection(sec + i, i, "named .text");
    DWORD boc = nt->OptionalHeader.BaseOfCode;
    for (int i = 0; i < n; i++)
        if (boc >= sec[i].VirtualAddress && boc < sec[i].VirtualAddress + sec[i].Misc.VirtualSize)
            return PickCodeSection(sec + i, i, "holds BaseOfCode");
    for (int i = 0; i < n; i++)
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) return PickCodeSection(sec + i, i, "first executable");
    return -1;
}

static void AddScanRegion(uintptr_t base, const IMAGE_SECTION_HEADER* sec) {
    if (g_scanRegions >= 4) return;
    g_scanStart[g_scanRegions] = base + sec->VirtualAddress;
    g_scanEnd[g_scanRegions] = g_scanStart[g_scanRegions] + sec->Misc.VirtualSize;
    if (g_scanRegions++) {
        char name[9] = { 0 }; memcpy(name, sec->Name, 8);
        strcat_s(g_codeSection, " + ");
        strcat_s(g_codeSection, name);
    }
}

// Name of the section an address is in, for the signature table.
static const char* SectionNameOf(uintptr_t a) {
    static char name[9];
    auto base = (uintptr_t)GetModuleHandleA(nullptr);
    auto nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (a >= base + sec[i].VirtualAddress && a < base + sec[i].VirtualAddress + sec[i].Misc.VirtualSize) {
            memset(name, 0, sizeof(name)); memcpy(name, sec[i].Name, 8);
            return name;
        }
    return "-";
}

// Padding runs of every section of 1 MB or more, for the log ("-" = not readable).
static const char* SectionPaddingText() {
    static char text[400];
    auto base = (uintptr_t)GetModuleHandleA(nullptr);
    auto nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    text[0] = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (sec[i].Misc.VirtualSize < 0x100000) continue;
        char name[9] = { 0 }; memcpy(name, sec[i].Name, 8);
        int runs = SectionPaddingRuns(sec + i, base);
        char item[48];
        if (runs < 0) snprintf(item, sizeof(item), "%s%s -", text[0] ? ", " : "", name);
        else snprintf(item, sizeof(item), "%s%s %d", text[0] ? ", " : "", name, runs);
        strcat_s(text, item);
    }
    return text;
}

static bool ResolveGameAddresses() {
    HMODULE exe = GetModuleHandleA(NULL);
    g_exeBase = (uintptr_t)exe;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)exe;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "no DOS header"); return false; }
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(g_exeBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "no PE header"); return false; }
    g_exeTimeStamp = nt->FileHeader.TimeDateStamp;
    g_exeImageSize = nt->OptionalHeader.SizeOfImage;
    int cs = CodeSectionIndex(nt, g_exeBase);
    if (cs < 0) { snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "no code section"); return false; }
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt) + cs;
    g_scanRegions = 0;
    if (!RangeReadable(g_exeBase + sec->VirtualAddress, sec->Misc.VirtualSize)) {
        snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "code section %s not readable yet", g_codeSection);
        return false;
    }
    if (memcmp(sec->Name, ".text", 5) != 0) {
        // renamed sections: scan once the code section is decrypted (thousands of
        // int3 runs, the same count for 1 s), together with every other big
        // section full of code
        int runs = SectionPaddingRuns(sec, g_exeBase);
        g_stableChecks = (runs >= 1000 && runs == g_lastCodeRuns) ? g_stableChecks + 1 : 0;
        g_lastCodeRuns = runs;
        if (g_stableChecks < 4) {
            snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "code section %s not decrypted yet (%d int3 runs)", g_codeSection, runs);
            return false;
        }
        g_codeReady = true;
        AddScanRegion(g_exeBase, sec);
        const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
            if (i != cs && s[i].Misc.VirtualSize >= 0x100000 && SectionPaddingRuns(s + i, g_exeBase) >= 1000) AddScanRegion(g_exeBase, s + i);
    } else AddScanRegion(g_exeBase, sec);

    int failedRequired = 0; char failList[160] = "";
    for (int i = 0; i < SIG_COUNT; i++) {
        bool ok = ScanPattern(g_sigs[i]);
        if (!ok && g_sigs[i].required) {
            failedRequired++;
            size_t l = strlen(failList);
            snprintf(failList + l, sizeof(failList) - l, "%s%s(x%d)", l ? ", " : "", g_sigs[i].name, g_sigs[i].matches);
        }
    }
    // the singleton pattern points at `mov rcx,[rip+disp32]`: target = next instruction + disp32
    if (g_sigs[SIG_MANAGERPTR].matches == 1) {
        uintptr_t insn = g_sigs[SIG_MANAGERPTR].resolved;
        int disp = *(const int*)(insn + 3);
        g_sigs[SIG_MANAGERPTR].resolved = insn + 7 + (intptr_t)disp;
    }
    if (failedRequired) {
        snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "signature not found: %s (exe timestamp 0x%08X, code section %s)", failList, g_exeTimeStamp, g_codeSection);
        return false;
    }
    if (SteamFormsUsed() && g_exeTimeStamp != STEAM_EXE_TIMESTAMP && !SteamTryEnabled()) {
        snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "all required signatures found, %d of them in their Steam form; not enabled on this unknown build: no scripts\\steam_try.txt (exe timestamp 0x%08X)",
            SteamFormsUsed(), g_exeTimeStamp);
        return false;
    }
    snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "OK, %d/%d signatures resolved (exe timestamp 0x%08X, %s, code section %s)",
        SIG_COUNT - (int)(!g_sigs[SIG_RESOLVECOMPOSITION].matches) - (int)(g_sigs[SIG_TABLEACCESSOR].matches != 1) - (int)(g_sigs[SIG_MANAGERPTR].matches != 1) - (int)(g_sigs[SIG_SPAWNSTORM].matches != 1),
        SIG_COUNT, g_exeTimeStamp, g_exeTimeStamp == 0x566520B2u ? "the GOG build this was made on" : g_exeTimeStamp == STEAM_EXE_TIMESTAMP ? "the Steam build" : "a build not seen by the author -- please report", g_codeSection);
    return true;
}

// ------------------------------------------------------------- startup --
// Test switch: MADMAX_MODS_DEFER=1 in the environment, or a file
// scripts\force_steam_path.txt, forces the deferred (Steam) startup path on a
// build whose code is readable at load time.
// A marker file whose text starts with "poll" also skips the GetStartupInfoW
// trigger, to exercise the polling fallback alone.
static bool g_forcePollOnly = false;
static bool ForceDefer() {
    char v[8];
    if (GetEnvironmentVariableA("MADMAX_MODS_DEFER", v, sizeof(v)) > 0 && v[0] == '1') return true;
    char path[MAX_PATH];                             // or a marker file: <game>\scripts\force_steam_path.txt
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* slash = strrchr(path, '\\'); if (slash) slash[1] = 0;
    strcat_s(path, "scripts\\force_steam_path.txt");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return false;
    char text[8] = { 0 };
    fread(text, 1, 4, f); fclose(f);
    g_forcePollOnly = strncmp(text, "poll", 4) == 0;
    return true;
}

// Padding runs of the code section (see CodeSectionIndex); -1 = not readable.
static int TextPaddingRuns() {
    auto base = (uintptr_t)GetModuleHandleA(nullptr);
    auto nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    int i = CodeSectionIndex(nt, base);
    return i < 0 ? -1 : SectionPaddingRuns(IMAGE_FIRST_SECTION(nt) + i, base);
}

// What kind of executable this is: Steam's DRM wraps the game in a ".bind"
// section, the protector of the Steam build seen on 2026-11-23 renames them
// all; a different build shows a different timestamp and size.
static void LogExeInfo() {
    auto base = (const uint8_t*)GetModuleHandleA(nullptr);
    auto nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    char sections[768] = { 0 };
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        char name[9] = { 0 }; memcpy(name, sec[i].Name, 8);
        DWORD c = sec[i].Characteristics;
        char item[64]; snprintf(item, sizeof(item), "%s%s(%X at +%X %c%c%c)", i ? " " : "", name, (unsigned)sec[i].Misc.VirtualSize,
            (unsigned)sec[i].VirtualAddress, c & IMAGE_SCN_MEM_READ ? 'R' : '-', c & IMAGE_SCN_MEM_WRITE ? 'W' : '-', c & IMAGE_SCN_MEM_EXECUTE ? 'X' : '-');
        strcat_s(sections, item);
    }
    LogLine("executable: timestamp 0x%08X, image size 0x%X, entry +0x%X, code base +0x%X size 0x%X, sections: %s", (unsigned)nt->FileHeader.TimeDateStamp,
        (unsigned)nt->OptionalHeader.SizeOfImage, (unsigned)nt->OptionalHeader.AddressOfEntryPoint,
        (unsigned)nt->OptionalHeader.BaseOfCode, (unsigned)nt->OptionalHeader.SizeOfCode, sections);
    LogLine("code padding runs: %d (thousands = readable code; near 0 = still encrypted)", TextPaddingRuns());
    LogLine("code section: %s; int3 padding runs per section: %s", g_codeSection, SectionPaddingText());
}

static void InstallMod(HMODULE hModule);
static HMODULE g_attachModule = nullptr;
static volatile LONG g_installed = 0;
typedef void (WINAPI* GetStartupInfoWFn)(LPSTARTUPINFOW info, uintptr_t chain);
static GetStartupInfoWFn GetStartupInfoW_orig = nullptr;

// Not one signature matched: the code is not readable yet (Steam's DRM keeps
// it encrypted until the game starts), as opposed to a build that differs.
static bool NoSignatureMatched() {
    for (int i = 0; i < SIG_COUNT; i++) if (g_sigs[i].matches) return false;
    return true;
}

// The second argument is not part of GetStartupInfoW; mm_sdk-based plugins
// pass a marker through it to each other, so it is forwarded untouched.
// Both triggers below resolve and install at most once.
static CRITICAL_SECTION g_installLock;
// ------------------------------------------------------------ steam probe --
// Diagnostics for a protected build whose signatures did not all match (Steam
// tester, 2026-11-23: only 2 of 11 found, both in .xpdata, 2 s after start).
// The mod stays off; a background thread scans again right away, at 30 s and
// at 120 s (the code section may still be being decrypted) and logs, for each
// signature not found, how many times its first 16 bytes and the rest of it
// occur, where, and 32 bytes from there -- enough to tell a late decryption,
// a moved function and changed code apart, and to write signatures for it.
static void HexAt(uintptr_t a, char* out, size_t size) {
    out[0] = 0;
    __try {
        for (int k = 0; k < 32 && (size_t)(k * 3 + 3) < size; k++)
            snprintf(out + k * 3, size - k * 3, "%02X ", ((const unsigned char*)a)[k]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, size, "(unreadable)"); }
}

static int CountWindow(const unsigned char* bytes, const bool* wild, int len, uintptr_t* first) {
    int matches = 0; *first = 0;
    for (int r = 0; r < g_scanRegions && matches < 99; r++) {
        const unsigned char* p = (const unsigned char*)g_scanStart[r];
        const unsigned char* end = (const unsigned char*)g_scanEnd[r] - len;
        __try {
            for (; p <= end; p++) {
                if (!wild[0] && p[0] != bytes[0]) continue;
                int i = 1;
                for (; i < len; i++) if (!wild[i] && p[i] != bytes[i]) break;
                if (i == len) {
                    if (!matches++) *first = (uintptr_t)p;
                    if (matches >= 99) break;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
    }
    return matches;
}

static void ProbeSignature(const GameSignature& sig, const char* pattern, const char* form, uintptr_t base) {
    unsigned char bytes[128]; bool wild[128]; int len = 0;
    for (const char* c = pattern; *c && len < 128; ) {
        while (*c == ' ') c++;
        if (!*c) break;
        if (c[0] == '?') { wild[len] = true; bytes[len] = 0; len++; c += 2; continue; }
        unsigned int v = 0; sscanf_s(c, "%2x", &v); bytes[len] = (unsigned char)v; wild[len] = false; len++; c += 2;
    }
    uintptr_t headAt = 0, tailAt = 0;
    int head = CountWindow(bytes, wild, len < 16 ? len : 16, &headAt);
    int tail = len >= 24 ? CountWindow(bytes + 16, wild + 16, len - 16, &tailAt) : -1;
    char headHex[100], tailHex[100];
    HexAt(headAt, headHex, sizeof(headHex));
    if (tailAt) HexAt(tailAt - 16, tailHex, sizeof(tailHex));
    LogLine("    %s, %s form: first 16 bytes %d time(s)%s%s +%X: %s; the rest (%d bytes) %d time(s)%s%s +%X: %s",
        sig.name, form, head, headAt ? ", first in " : "", headAt ? SectionNameOf(headAt) : "", headAt ? (unsigned)(headAt - base) : 0,
        headAt ? headHex : "-", len - 16, tail, tailAt ? ", it would start in " : "", tailAt ? SectionNameOf(tailAt - 16) : "",
        tailAt ? (unsigned)(tailAt - 16 - base) : 0, tailAt ? tailHex : "-");
}

static void SteamProbeRound(const char* when) {
    auto base = (uintptr_t)GetModuleHandleA(nullptr);
    auto nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    int cs = CodeSectionIndex(nt, base);
    g_scanRegions = 0;
    if (cs < 0 || !RangeReadable(base + s[cs].VirtualAddress, s[cs].Misc.VirtualSize)) { LogLine("steam probe (%s): code section not readable", when); return; }
    AddScanRegion(base, s + cs);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (i != cs && s[i].Misc.VirtualSize >= 0x100000 && SectionPaddingRuns(s + i, base) >= 1000) AddScanRegion(base, s + i);
    LogLine("steam probe (%s): scanning %s; int3 padding runs per section: %s", when, g_codeSection, SectionPaddingText());
    for (int i = 0; i < SIG_COUNT; i++) {
        GameSignature& sig = g_sigs[i];
        if (ScanPattern(sig)) LogLine("    %s: OK in %s +%X (%s form)", sig.name, SectionNameOf(sig.resolved), (unsigned)(sig.resolved - base),
            g_sigSteamForm[i] ? "Steam" : "GOG");
        else {
            LogLine("    %s: not found (matches %d)", sig.name, sig.matches);
            ProbeSignature(sig, sig.pattern, "GOG", base);
            if (g_steamPatterns[i]) ProbeSignature(sig, g_steamPatterns[i], "Steam", base);
        }
    }
}

static DWORD WINAPI SteamProbe(LPVOID) {
    SteamProbeRound("at install");
    Sleep(30000);
    SteamProbeRound("30 s later");
    Sleep(90000);
    SteamProbeRound("120 s later");
    LogLine("steam probe done");
    return 0;
}

static void StartSteamProbe() {
    HANDLE th = CreateThread(nullptr, 0, SteamProbe, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
}

static bool TryInstall(const char* how, bool wait) {
    // The game's main thread (GetStartupInfoW) never waits here: while the
    // other trigger scans or installs, the game goes on starting.
    if (!wait) { if (!TryEnterCriticalSection(&g_installLock)) return false; }
    else EnterCriticalSection(&g_installLock);
    bool done = g_installed != 0;
    if (!done) {
        g_gameVersionOk = ResolveGameAddresses();
        if (g_gameVersionOk || g_codeReady || !NoSignatureMatched()) {      // readable now (a build that differs still ends here)
            g_installed = 1;
            LogLine("game code ready (%s), installing", how);
            InstallMod(g_attachModule);
            done = true;
        }
    }
    LeaveCriticalSection(&g_installLock);
    return done;
}

static void WINAPI GetStartupInfoW_hook(LPSTARTUPINFOW info, uintptr_t chain) {
    GetStartupInfoW_orig(info, chain);
    if (!g_installed) TryInstall("C runtime startup", false);
}

// Fallback for when GetStartupInfoW is never called after this plugin loads
// (the plugin loader may come in after the game's C runtime startup): look
// again every 250 ms for up to 5 minutes, logging what the sections look like
// every 10 s.
static DWORD WINAPI WaitForGameCode(LPVOID) {
    for (int i = 0; i < 1200 && !g_installed; i++) {
        Sleep(250);
        if (TryInstall("found while waiting", true)) return 0;
        if (i % 40 == 39)
            LogLine("waiting for game code (%d s): %s; code section %s; int3 padding runs per section: %s",
                (i + 1) / 4, g_gameVersionStatus, g_codeSection, SectionPaddingText());
    }
    if (!g_installed)
        LogLine("code padding runs now: %d (thousands = the code is readable, so the functions really are not there)", TextPaddingRuns());
    if (!g_installed)
        LogLine("gave up after 5 minutes: none of the game functions were found -- please send this log to the mod author.");
    return 0;
}

#if MM_DEV_TOOLS
static void TdDump(const char* why);
#endif
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
    if (dwReason == DLL_PROCESS_DETACH) {
        LogConvoySnapshot("game exit");
#if MM_DEV_TOOLS
        TdDump("game exit");
#endif
        LogLine("session end");
        return TRUE;
    }
    if (dwReason == DLL_PROCESS_ATTACH) {
        InitModPaths(hModule);
        LogSessionStart();
        DetectConvoyMapPoints();
        MH_Initialize();
        InitializeCriticalSection(&g_installLock);
        bool defer = ForceDefer();
        g_gameVersionOk = ResolveGameAddresses();
        LogLine("session start, build %s", MM_BUILD_TAG);
        LogExeInfo();
        if ((!g_gameVersionOk && NoSignatureMatched() && !g_codeReady) || defer) {
            // Steam: the executable is wrapped by Steam's DRM and its code is
            // still encrypted while plugins load, so nothing can match yet. The
            // game's C runtime startup calls GetStartupInfoW once the real code
            // runs; resolve and install from there (what mm_sdk's
            // HookMgr::Initialize does on Steam, minus its hard-coded address).
            g_attachModule = hModule;
            g_deferred = true;
            MH_STATUS e = MH_ERROR_DISABLED;
            if (!g_forcePollOnly) {
                LPVOID gsi = (LPVOID)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetStartupInfoW");
                MH_STATUS c = gsi ? MH_CreateHook(gsi, (LPVOID)GetStartupInfoW_hook, (LPVOID*)&GetStartupInfoW_orig) : MH_ERROR_FUNCTION_NOT_FOUND;
                e = (c == MH_OK) ? MH_EnableHook(gsi) : c;
            }
            LogLine("game code not readable yet (Steam DRM?), installing at game startup: %s; also checking again every 250 ms", MH_StatusToString(e));
            HANDLE th = CreateThread(nullptr, 0, WaitForGameCode, nullptr, 0, nullptr);
            if (th) CloseHandle(th);
            return TRUE;
        }
        InstallMod(hModule);
    }
    return TRUE;
}

static void InstallMod(HMODULE hModule) {
    {
        LogLine("game version check: %s", g_gameVersionStatus);
        LogLine("convoy formation map: %s", g_convoyMapStatus);
        for (int i = 0; i < SIG_COUNT; i++)
            LogLine("  signature %-70s %s %p in %s (matches %d)%s%s", g_sigs[i].name, g_sigs[i].matches == 1 ? "OK" : "--", (void*)g_sigs[i].resolved,
                g_sigs[i].resolved ? SectionNameOf(g_sigs[i].resolved) : "-", g_sigs[i].matches,
                g_sigs[i].alreadyHooked ? " -- already hooked by another mod, chaining behind it" : "",
                g_sigSteamForm[i] ? " (Steam form)" : "");
        if (!g_gameVersionOk) {
            char msg[512];
            snprintf(msg, sizeof(msg),
                "Mad Max - Enhanced Convoys is DISABLED: it could not locate the game functions it needs in this executable.\n\n"
                "Reason: %s\n\n"
                "The mod stays loaded but does nothing, so the game is safe to play. "
                "Please report your game version (store + patch) to the mod author.", g_gameVersionStatus);
            if (!g_deferred) MessageBoxA(NULL, msg, "Mad Max - Enhanced Convoys", MB_OK | MB_ICONWARNING);
            else if (g_codeReady) StartSteamProbe();
            return;
        }
        // Not HookMgr::Initialize(): its Steam path defers installation from a
        // hook whose trigger is a hard-coded return address. Everything this
        // mod hooks is signature-resolved; on Steam the deferral is done above.
        HookMgr::isGOG = HookMgr::IsKnownGogBuild();
        HookMgr::isSteam = !HookMgr::isGOG;
        LogLine("SDK build detection: %s", HookMgr::isGOG ? "known GOG build" : (HookMgr::SdkAddressesUsable() ? "not the known GOG build" : "unknown build (SDK addresses unusable)"));
        PluginHooks();
        PluginAttach(hModule, DLL_PROCESS_ATTACH, nullptr);
    }
}


void PluginAttach(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
}

#if MM_DEV_TOOLS
bool rightShiftPressed = false;
bool enabledAirBrake = false;
float speedBrake = 20.0;
bool f1Pressed = false;
bool enabledInvincibility = false;
bool f2Pressed = false;
#endif
bool f3Pressed = false;
bool g_showDiagnostics = false;

// Diagnostics. g_convoy*HookInstallStatus captures MH_CreateHook/MH_EnableHook's
// real result instead of ignoring it like HookMgr::Install normally does.
int g_convoySetWreckedCalls = 0;
int g_convoySetWreckedCleared = 0;
int g_convoySetWreckedResolveFailed = 0;
const char* g_convoySetWreckedHookInstallStatus = "not attempted yet";

// NExternalObjectResolvers::GetGameObjectTyped<CConvoyDataContainer> -- takes
// the exact same (CProcessor&, SGSNode*, pin) shape our own hook receives and
// resolves which CConvoyDataContainer instance the node is operating on. Not
// hooked, just called directly (it's the game's own helper, address only).
typedef void* (*GetConvoyContainerFn)(void* processor, const void* node, unsigned int pinIndex);
// (address no longer resolved: the typed resolver is a template instantiated for several types with
// byte-identical code, so it has no unique signature; the diagnostic that used it was dropped in v0.9.1)

// NGSONodes::ConvoyDataSetWrecked -- history of this hook:
// v1 (2026-09-15): unconditionally skipped the real call (no-op). Did stop
//   the permanent lock, but broke the spawn cycle -- dust-cloud route marker
//   kept animating with no actual vehicles ever spawning. This function
//   evidently also does bookkeeping (e.g. clearing SpawnedObjectsList) that a
//   fresh spawn needs, so skipping it left that cleanup undone.
// v2 (2026-09-16): moved the override to CConvoyDataContainer::IsWrecked(), a
//   tiny native getter (`return (m_Flags & 1) != 0`, confirmed byte-for-byte
//   against the user's exe). Live test: hook installed fine but was called
//   ZERO times even after destroying a convoy -- the function is small enough
//   that the compiler almost certainly inlined it at every call site that
//   matters, so hooking the one out-of-line copy hits nothing.
// v3 (this version): stop trying to intercept a *read* of the flag (inlining
//   makes that unreliable) and instead correct the underlying *data* right
//   after the real write happens. Call the real SetWrecked first (so its
//   cleanup/bookkeeping runs normally -- this is what v1 broke), then use the
//   engine's own GetGameObjectTyped<CConvoyDataContainer> helper (same
//   processor/node the node call already has) to resolve the actual
//   container instance, and clear EFlags_Wrecked (bit 0x1) of m_Flags
//   (this+0x118, verified via DIA2Dump) directly. Since this corrects the
//   real memory location, it doesn't matter how many places read it or
//   whether those reads are inlined -- they'll all see it cleared.
// The third argument to GetGameObjectTyped is NOT a pin index -- it's the
// Jenkins hash of the node's variable-pin NAME. Found 2026-09-20 by
// disassembling the real NGSONodes::ConvoyDataSetWrecked (0x1402BDB20): it
// calls GetGameObject(processor, node, 0xBAFB74B7), and 0xBAFB74B7 ==
// 3137041591 == the name of the first variable_pin on the ConvoyDataSetWrecked
// node (Node[43] in convoy_navigation_handler.gsrc) and on ConvoyDataGetWrecked
// (Node[2] in convoy_choreographer.gsrc). The previous hardcoded `0` never
// resolved anything: every live test that had the resolve counters visible
// showed `resolve failed`, and the earlier "confirmed correct 2026-09-17" note
// actually described the F4 manual reset working, not this hook.
const unsigned int CONVOY_NODE_CONTAINER_PIN_HASH = 0xBAFB74B7;
//
// Always on: this is the mod's entire purpose, not an experimental option --
// an end user installing this expects convoys to always respawn, with no
// hotkey required. (MM_DEV_TOOLS builds keep an F2 kill-switch for our own
// A/B testing against vanilla behavior.)
#if MM_DEV_TOOLS
bool enabledConvoyNeverWrecked = true;
#else
static const bool enabledConvoyNeverWrecked = true;
#endif

// ---- Graph-object capture diagnostics (2026-09-19/20) ----
// processor+0xA0 (CProcessor::m_GameObject, inherited by sub-processors via
// CGameObjectProcessor::BuildSubProcessor -- disassembled) is the top-level
// CGraphScriptGameObject running convoy_choreographer.gsrc. Layout via
// Dia2Dump: +0xE0 m_State (EGraphState: 0 = (re)build, 1 = init/fire default
// Start, 2 = running, 4 = exited/done), +0xE8 m_Graph, +0xF0
// m_GraphProcessor, +0xF8 m_GraphPathHash, +0x100 m_GraphPathName, +0x1C8
// m_TurnTaker. Only the path-hash check below is load-bearing now; the
// FireStart approach that this capture was built for is retired -- tracing
// the graph showed its target start node (Name 127520085, Node[100]) is the
// "GRAPH EXIT ON LOAD" entry, not the spawn entry, and the v2 sweep uses the
// engine's own m_State=0 rebuild path instead.
//
// m_GraphPathHash is the hash of the SOURCE path ("...convoy_choreographer
// .gsrc"), not the compiled ".gsr" the RTPC references (live-confirmed).
const unsigned int CONVOY_CHOREOGRAPHER_PATH_HASH = 2496881151; // Jenkins("graphs/convoys/convoy_choreographer.gsrc")

void* g_lastConvoyGraphObject = nullptr;
unsigned int g_lastConvoyGraphPathHash = 0;
int g_convoyGraphObjectCaptures = 0;
int g_convoyGraphObjectCaptureMismatches = 0;

bool f9Pressed = false;
int g_forcedSweeps = 0; // dev F9 presses (v2 sweep with delay/distance bypassed)

// Diagnostic (2026-09-20): the `pin` our hook receives is the hash of the
// TRIGGER pin that fired the node (observed live: 3377178442 == "in"), which
// is a different thing from the variable-pin name hash the resolver wants --
// logged so the two never get confused again.
unsigned int g_lastConvoySetWreckedRealPin = 0xFFFFFFFF;

DEFHOOK(uint32_t, ConvoyDataSetWrecked, (void* processor, const void* node, unsigned int pin)) {
    g_convoySetWreckedCalls++;
    LogLine("EVENT ConvoyDataSetWrecked called (#%d) -- a convoy was just destroyed", g_convoySetWreckedCalls);
    g_lastConvoySetWreckedRealPin = pin;
    uint32_t result = ConvoyDataSetWrecked_orig(processor, node, pin);

    // v2 (2026-09-20): the flag is deliberately NOT cleared here any more.
    // Tracing convoy_wreck_handler.gsrc (gsrc_trace.py) showed it starts with
    // ConvoyDataGetWrecked and, if the container is NOT wrecked, hits an Error
    // node ("ConvoyData is not wrecked") and dies -- taking the hood-ornament
    // relic loop (load/unload at 300 m, RelicIsCollected) and the wreck
    // despawn loop with it. Clearing the flag at this moment therefore breaks
    // vanilla wreck handling. The respawn is now driven by the state-4 sweep
    // in ConvoyRespawnTick() below; this resolve is kept purely as a
    // diagnostic that the pin-hash resolution keeps working.


    // Diagnostic capture of the owning graph object (shown in the overlay),
    // gated strictly on the path-hash matching -- never trust the pointer
    // otherwise. The v2 sweep does not use it.
    void* graphObj = *(void**)((uintptr_t)processor + 0xA0);
    if (graphObj) {
        unsigned int pathHash = *(unsigned int*)((uintptr_t)graphObj + 0xF8);
        g_lastConvoyGraphPathHash = pathHash;
        if (pathHash == CONVOY_CHOREOGRAPHER_PATH_HASH) {
            g_lastConvoyGraphObject = graphObj;
            g_convoyGraphObjectCaptures++;
        } else {
            g_convoyGraphObjectCaptureMismatches++;
        }
    }
    return result;
}

// ---- "Reset all convoys" cheat (2026-09-16) ----
// Testing bottleneck: after enough live tests, every convoy route on every
// save except the user's real save (never to be touched for testing) and the
// dedicated test save (now also exhausted) is already permanently wrecked --
// no fresh route left to test the hook above with. Rather than needing a new
// save, this resets EFlags_Wrecked directly on all 14 known convoys by ID, on
// demand, in the current session.
//
// CGameObject::FindOptional(uint64_t id, boost::shared_ptr<CGameObject>&) is
// `public: static`, so callable with no existing instance. boost::shared_ptr
// is not linked here; its layout (16 bytes: {T* px; shared_count pn;}, a
// default-constructed/zeroed instance being a valid empty shared_ptr) is
// stable enough to fake with a raw byte buffer for this one call -- we read
// px (offset 0) as the resolved CGameObject* and never touch pn again, which
// leaks one refcount per successful call. Harmless for a rarely-pressed debug
// key on objects that live for the whole session anyway, not something to do
// in a hot path.
//
// The 14 IDs are CFFF8405 ("<hex>,0" pairs) read from every CConvoyDataContainer
// in global/convoys.blo during the static analysis phase of this session
// (see README.md's Convoys section) -- UNVERIFIED assumption that this RTPC
// "objectid" field equals the runtime CGameObject::GetObjectID() FindOptional
// looks up by. Guarded with a sanity check (m_Flags must be 0-3, the only
// combination of known EFlags_Wrecked|EFlags_Discovered bits) before writing,
// so a wrong/stale ID is far more likely to be skipped than to corrupt an
// unrelated live object.
typedef bool (*FindOptionalFn)(uint64_t id, void* outSharedPtr /* 16 bytes */);
static FindOptionalFn CGameObject_FindOptional = nullptr; // set from g_sigs[SIG_FINDOPTIONAL] in PluginHooks

// The per-ID names originally attached here (e.g. "gutgash_convoy3",
// "mm3030_convoy") were retracted 2026-09-18: they came from matching each
// ID against the nearest *sibling* object in convoys.blo.xml, which turned
// out to be unreliable -- re-parsing the real object tree showed every one
// of the 14 CConvoyDataContainer entries is structurally identical (same
// class, same generic tag, no mission-ID field), and only 2 of 14 names
// happened to line up by file-proximity coincidence. No evidence anywhere
// in the game data (checked every mission file and every .gsrc) ties any of
// these 14 to a specific mission -- the "mm3030_convoy" label specifically
// could not be substantiated and was likely just wrong.
//
// A user reported seeing only 13 convoy routes on their in-game map despite
// this list having 14 entries. Best current explanation: EFlags_Discovered
// (bit 0x2 of the same m_Flags byte EFlags_Wrecked lives in, this+0x118) is
// a separate flag from EFlags_Wrecked -- a route a player hasn't physically
// encountered yet likely doesn't get a map icon regardless of this mod,
// and ResetAllKnownConvoys() below only ever touches EFlags_Wrecked, never
// EFlags_Discovered. Not confirmed live, but doesn't need to be for this
// mod to be safe: the sanity check in ResetAllKnownConvoys() (m_Flags must
// be 0-3) means clearing the wrecked bit on an undiscovered/inactive convoy
// is a no-op either way, not a source of instability.
static const uint64_t g_knownConvoyIds[] = {
    0x7E90E3F6, // convoy 1
    0x38A45D73, // convoy 2
    0x8F1728CD, // convoy 3
    0x59501178, // convoy 4
    0x7D6BB232, // convoy 5
    0x132E3492, // convoy 6
    0x42D456AE, // convoy 7
    0x7C5903DF, // convoy 8
    0x6FDA7EF0, // convoy 9
    0x337019D7, // convoy 10
    0x1FA21EA1, // convoy 11
    0xB6418D01, // convoy 12
    0x74C87945, // convoy 13
    0x35762FBA, // convoy 14
};
const int g_knownConvoyCount = sizeof(g_knownConvoyIds) / sizeof(g_knownConvoyIds[0]);

int g_resetConvoysLastFound = -1;
int g_resetConvoysLastCleared = -1;
int g_resetConvoysLastSkippedSanity = -1;

#if MM_DEV_TOOLS

// ---- Spawn-flow logging (2026-09-21, dev only) ----
// "Invisible convoy" reports (dust cloud, no cars) with the +2 escorts. The
// navigation graph already routes spawn failures into metric nodes
// (ConvoyMetricIsSpawnPossibleFailed / InProgressSpawningFailed) and gates
// the batch on SpawnMultipleEntitiesAvailable / SpawnHoldAfterResourcesLoaded.
// These hooks wrap those node handlers plus RequestSpawn and, through a hook
// on CProcessor::FireConnections, log WHICH output pin each one fired
// (RequestSpawn: 1747528269 = "requested"; Available/Hold: true/false pins).
// Loop nodes are logged only on pin transitions to keep the log readable.
// Read-only: every hook just calls the original.
static const char* g_logNodeName = nullptr;
static unsigned int g_lastPinByNode[4] = { 0, 0, 0, 0 };
static int g_pinRepeatByNode[4] = { 0, 0, 0, 0 };
static bool g_convoyCompositionKnown = false;
static int g_logNodeIndex = -1;

DEFHOOK(void, FireConnectionsLog, (void* processor, const void* node, unsigned int pin)) {
    if (g_logNodeName) {
        if (g_logNodeIndex >= 0) {
            if (g_lastPinByNode[g_logNodeIndex] == pin) { g_pinRepeatByNode[g_logNodeIndex]++; }
            else {
                if (g_pinRepeatByNode[g_logNodeIndex]) LogLine("    (%s -> pin %u repeated %d more times)", g_logNodeName, g_lastPinByNode[g_logNodeIndex], g_pinRepeatByNode[g_logNodeIndex]);
                LogLine("  %s -> pin %u", g_logNodeName, pin);
                g_lastPinByNode[g_logNodeIndex] = pin; g_pinRepeatByNode[g_logNodeIndex] = 0;
            }
        } else {
            LogLine("  %s -> pin %u", g_logNodeName, pin);
        }
    }
    FireConnectionsLog_orig(processor, node, pin);
}

// Only graphs owned by a convoy choreographer object are logged: processor+0xA0
// is the owning CGraphScriptGameObject even for ExternalGraph sub-processors
// (BuildSubProcessor copies it), and +0xF8 is its graph path hash.
static bool IsConvoyProcessor(void* processor) {
    void* obj = *(void**)((uintptr_t)processor + 0xA0);
    return obj && *(unsigned int*)((uintptr_t)obj + 0xF8) == CONVOY_CHOREOGRAPHER_PATH_HASH;
}
#define LOGGED_NODE_HOOK(NAME, LABEL, IDX) \
    DEFHOOK(uint32_t, NAME, (void* processor, const void* node, unsigned int pin)) { \
        if (!IsConvoyProcessor(processor)) return NAME##_orig(processor, node, pin); \
        const char* prev = g_logNodeName; int prevIdx = g_logNodeIndex; \
        g_logNodeName = LABEL; g_logNodeIndex = IDX; \
        uint32_t r = NAME##_orig(processor, node, pin); \
        g_logNodeName = prev; g_logNodeIndex = prevIdx; \
        return r; \
    }
LOGGED_NODE_HOOK(RequestSpawnLog, "RequestSpawn (1747528269=requested, 3838669375=not possible)", -1)
LOGGED_NODE_HOOK(IterateGuardsLog, "ConvoysCompositionIterateGuards (172537D0=finished, ACF45D32=guard)", -1)
LOGGED_NODE_HOOK(CCMapIsBlockedLog, "ConvoyDataCCMapIsBlocked (706834940=blocked, 3855993015=free)", -1)

LOGGED_NODE_HOOK(SpawnPossibleFailedLog, "ConvoyMetricIsSpawnPossibleFailed", -1)
LOGGED_NODE_HOOK(SpawningFailedLog, "ConvoyMetricInProgressSpawningFailed (batch resource load FAILED -> despawn + retry)", -1)
LOGGED_NODE_HOOK(SpawnAvailableLog, "SpawnMultipleEntitiesAvailable (706834940=true, 3855993015=false)", 0)
LOGGED_NODE_HOOK(SpawnHoldLog, "SpawnHoldAfterResourcesLoaded", 1)
LOGGED_NODE_HOOK(SpawnResourcesLoadedLog, "SpawnResourcesLoaded (706834940=loaded, 3855993015=loading, 3714460898=FAILED)", 3)

static void InstallLoggedNodeHook(uintptr_t addr, LPVOID hook, LPVOID* orig, const char* label) {
    MH_STATUS c = MH_CreateHook((LPVOID)addr, hook, orig);
    MH_STATUS e = (c == MH_OK) ? MH_EnableHook((LPVOID)addr) : c;
    LogLine("spawn-flow hook %s @ %p: %s", label, (void*)addr, MH_StatusToString(e));
}

// ---- Mod A test hook (2026-09-17): NGSONodes::SpawnStorm ----
// Found by cross-referencing a third-party Cheat Engine table
// (MadMax_v1.03_AOB_steam_and_GoG_v1.5.CT) against our own AVAMain_F.pdb:
// its "storm.trigger" console command goes through
// NEvent::CSendEvent<...>::SendMsg(const char*), a generic engine-wide named
// -event dispatcher. Grepping the PDB for Storm/Weather symbols from there
// surfaced the real graph-script node that actually spawns a storm.
//
// Two separate, independent probes, both read-only/additive (neither skips
// or alters the real call, unlike the early convoy hook mistakes):
//   1. An instrumentation hook on SpawnStorm itself -- just counts how often
//      the game calls it on its own, to see the real baseline frequency
//      before touching anything.
//   2. A one-shot F5 key that calls SendMsg("storm.trigger") directly --
//      this is the *same* engine entry point the CT table's own working
//      hotkey uses, just resolved through our PDB to its real name/address
//      instead of a raw AOB scan. No CProcessor/SGSNode context is needed
//      for this path (unlike SpawnStorm, which only makes sense mid-graph),
//      so it's the practical way to test "can we force a storm on demand".
//
// UNVERIFIED: whether calling SendMsg with a plain C++ call (which already
// keeps 16-byte stack alignment per the x64 ABI) is equivalent to how the CE
// script invokes it (which manually reserves 0x220 bytes of scratch stack
// before the call) -- if SendMsg's internals expect that scratch space to
// already be there rather than allocating their own, this could misbehave.
// Test on save 4 only.
SendEventMsgFn SendEventMsg = (SendEventMsgFn)0x140007ca0;

bool f5Pressed = false;
extern bool g_fastStormsOn;
static void OnStormSpawned();
int g_spawnStormCalls = 0;
const char* g_spawnStormHookInstallStatus = "not attempted yet";
int g_forceStormPresses = 0;

// ---- Mod A: fast storms, using the game's own switch (2026-09-22) ----
// Storms are rare by design, not by accident: graphs/open_world/
// encounter_storm_generate_interval.gsrc picks the delay until the next storm
// with SetRandomFloat. The node's own literals read 1800/3600, but its
// variable pins are wired to VariableFloat 7200 and 9000 -- and the pins win,
// so the real interval is 2 to 2.5 hours of play. That matches the SpawnStorm
// instrumentation hook above, which logged zero natural storms in every test
// session so far.
//
// The same script has a second path: if its input bool is true the interval
// is a flat 60 seconds. Inside encounter_system.gsrc that bool is a local
// variable (Node[355]) written by two SetVariable nodes, each fired by its
// own Start node -- Node[311] writes 1.0, Node[294] writes 0.0. Start nodes
// are exactly what CProcessor::FireStart(nameHash) triggers, so the toggle is
// a pair of FireStart calls on the object that runs that graph: the
// CGraphScriptGameObject "gsr.encounter_system" from global/global.blo,
// objectid 4252045013. No file is modified and it is reversible in place.
//
// UNVERIFIED: whether the new interval applies to the storm already being
// timed or only to the next one scheduled after the toggle (the script runs
// when the previous interval expires), and whether 60 s storms upset anything
// that assumes they are rare. Test on save 4 only.
static void* FindGameObjectByIdAndRelease(uint64_t id);   // defined with the respawn sweep below

const uint64_t ENCOUNTER_SYSTEM_OBJECT_ID = 4252045013ull;                  // gsr.encounter_system in global.blo
const unsigned int ENCOUNTER_SYSTEM_PATH_HASH = 0x99BD2CEAu;                // Jenkins("graphs/open_world/encounter_system.gsrc")
// The decoded graph prints these Name fields as signed decimals
// (-2005534086 and 1857822523); converting them by hand the first time gave
// two wrong constants and FireStart simply found no such Start node.
const unsigned int STORM_START_FAST = 0x8875FA7Au;                          // Start Node[311], Name -2005534086 -> flag = 1 -> 60 s interval
const unsigned int STORM_START_NORMAL = 0x6EBC1F3Bu;                        // Start Node[294], Name  1857822523 -> flag = 0 -> 7200-9000 s
// Setting the flag alone changes nothing that is already running: the storm
// is fired by a Timer node (Node[325]) whose "done" output goes straight to
// SpawnStorm, and a Timer only samples its time pin when it is started or
// restarted. Three live pulses in a row timed out for exactly this reason --
// the interval variable was updated while the old 2-hour countdown kept
// running. Start Node[291] is the cycle entry the game itself uses: through
// OrderedExecute Node[198] it calls the interval script AND then reaches
// Node[203], which fires the Timer's "restart" pin. So each pulse now sets
// the flag first and immediately restarts the cycle.
const unsigned int STORM_START_RESTART_CYCLE = 0x4121B2DAu;                 // Start Node[291], Name 1092727514

// Storm cadence presets.
//
// First attempt drove the game's own scheduler: encounter_system.gsrc keeps a
// flag that switches the interval between 60 s and 7200-9000 s, and Start
// Node[291] recomputes the interval and restarts the timer. Both Starts were
// accepted every time, yet five pulses across two sessions produced no storm.
// Reading the graph again explains it: the storm is fired by Timer Node[325],
// which is authored with paused = true and is un-paused by a pin fed from
// four separate conditions (InStormArea "no", IsCharacterInSequence,
// a CompareVariable, and a WaitForEnvironmentTagCallback). Restarting a timer
// that the environment keeps parked achieves nothing.
//
// So the cadence is now driven the short way: NEvent::CSendEvent::SendMsg
// with the engine's own "storm.trigger" command -- the same entry point the
// Cheat Engine table uses, verified in game to raise a storm immediately and
// without side effects. The game's own 2-hour cycle is left completely
// untouched underneath; this only adds storms on top of it.
enum StormPreset { STORM_OFF = 0, STORM_FREQUENT, STORM_UNCOMMON, STORM_RANDOM, STORM_PRESET_COUNT };
struct StormPresetInfo { const char* name; float minSeconds; float maxSeconds; };
// Gaps are measured from the END of one storm (the weather really clearing,
// read from the weather manager below) to the next trigger, counted only in
// game time and only while the sky is clear.
static const StormPresetInfo g_stormPresets[STORM_PRESET_COUNT] = {
    { "off (vanilla only)",                        0.0f,    0.0f },
    { "frequent (4-9 min between storms)",       240.0f,  540.0f },
    { "uncommon (10-30 min between storms)",     600.0f, 1800.0f },
    { "random (0-60 min, can be back to back)",    0.0f, 3600.0f },
};
// Fallback only, for when the weather signal is unavailable: measured
// 2026-09-23, a storm reaches a waiting player ~40 s after the trigger, stays
// at full strength 300 s and fades out in 30 s.
const float STORM_MEASURED_TOTAL_S = 370.0f;
// A trigger whose storm has not reached the player after this long is counted
// as missed (outrun, or the player is somewhere storms cannot happen). The
// next one is then scheduled normally -- never re-fired at once, so storm
// fronts do not pile up out on the map.
const float STORM_ARRIVAL_TIMEOUT_S = 120.0f;

int g_stormPreset = STORM_OFF;
float g_stormNextDelay = 0.0f;
int g_stormTriggers = 0, g_stormArrived = 0, g_stormMissed = 0;
char g_stormToggleStatus[200] = "not started";
bool f11Pressed = false;
bool g_fastStormsOn = false;   // kept only so the SpawnStorm log line can say who asked

static float RollStormDelay() {
    const StormPresetInfo& p = g_stormPresets[g_stormPreset];
    float t = (float)rand() / (float)RAND_MAX;
    return p.minSeconds + t * (p.maxSeconds - p.minSeconds);
}

// ---- The weather signal (2026-09-23, confirmed live) ----
// CEnvironmentPresetTimeOfDayManager keeps the active storm-class preset at
//     int   index  = *(int*)(mgr + 0x270)        (-1 = none)
//     state = mgr + 0x278 + index * 0x48         (SSandstormState)
// and the state's +0x3C is the preset's weather id, from
// global/environment_presets.blo:
//     -1 Sandstorm, -2 ThunderStorm      real storms (CStormConfig defines
//                                        Sandstorm, Thunderstorm, Infinitestorm)
//     -3 SulfurStorm, -4 Gastown         local area atmospheres (sulfur pits,
//                                        Gastown) holding the same slot; a
//                                        storm cannot start while they are on
// Live test: near Gastown the -4 preset was on and F11 produced nothing;
// waiting in the open, the index switched to the Sandstorm 39 s after F11,
// stayed 300 s, faded 30 s; driving away, it never changed at all.
// The manager comes from its own per-frame InternalUpdateRender (GOG, exact
// `this`) or, failing that, from the object lookup: FindOptional on objectid
// 4080819380 returns the manager + 0x10 (live: ...360 vs ...350, a secondary
// base), accepted only if the index reads as a sane -1..7.
extern CVector3f g_PlayerPos;   // defined with the overlay below
const uint64_t TIME_OF_DAY_MANAGER_OBJECT_ID = 4080819380ull;
const int STORM_STATE_BASE = 0x278, STORM_STATE_STRIDE = 0x48, STORM_INDEX_OFF = 0x270, STORM_SLOTS = 8;

void* volatile g_timeOfDayMgr = nullptr;
char g_todHookStatus[64] = "not installed";
DEFHOOK(void, TodInternalUpdateRender, (void* self, float dt)) {
    g_timeOfDayMgr = self;
    TodInternalUpdateRender_orig(self, dt);
}

enum WeatherKind { WEATHER_UNKNOWN = 0, WEATHER_CLEAR, WEATHER_STORM, WEATHER_LOCAL };
struct WeatherReading { bool ok; int index; int weatherId; float fadeIn, fadeOut, timer; int flags; };
static WeatherReading g_weather = { false, -1, 0, 0.0f, 0.0f, 0.0f, 0 };
static WeatherKind g_weatherKind = WEATHER_UNKNOWN;
static float g_weatherClock = 0.0f, g_gameTime = 0.0f, g_stormStartedAt = -1.0f;
char g_stormProbeStatus[200] = "weather: waiting for the manager";

static const char* WeatherName(int id) {
    switch (id) {
    case -1: return "Sandstorm";
    case -2: return "ThunderStorm";
    case -3: return "SulfurStorm (local)";
    case -4: return "Gastown (local)";
    }
    return "unknown";
}

static bool ReadWeather(void* mgr, WeatherReading* out) {
    __try {
        int idx = *(int*)((uintptr_t)mgr + STORM_INDEX_OFF);
        if (idx < -1 || idx >= STORM_SLOTS) return false;
        out->index = idx; out->weatherId = 0; out->fadeIn = out->fadeOut = out->timer = 0.0f; out->flags = 0;
        if (idx >= 0) {
            uintptr_t st = (uintptr_t)mgr + STORM_STATE_BASE + idx * STORM_STATE_STRIDE;
            out->fadeIn = *(float*)(st + 0x30); out->fadeOut = *(float*)(st + 0x34); out->timer = *(float*)(st + 0x38);
            out->weatherId = *(int*)(st + 0x3C); out->flags = *(uint8_t*)(st + 0x40);
        }
        out->ok = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void* FindWeatherManager() {
    if (g_timeOfDayMgr) return g_timeOfDayMgr;
    if (!CGameObject_FindOptional) return nullptr;
    void* byId = FindGameObjectByIdAndRelease(TIME_OF_DAY_MANAGER_OBJECT_ID);
    return byId ? (void*)((uintptr_t)byId - 0x10) : nullptr;
}

static void OnWeatherChanged(const WeatherReading& prev, WeatherKind prevKind, const WeatherReading& now, WeatherKind kind) {
    if (kind == WEATHER_STORM && prevKind != WEATHER_STORM) {
        g_stormStartedAt = g_gameTime;
        LogLine("STORM ARRIVED: %s (index %d) | player %.0f %.0f %.0f", WeatherName(now.weatherId), now.index, g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z);
    } else if (prevKind == WEATHER_STORM && kind != WEATHER_STORM) {
        LogLine("STORM ENDED: %s after %.0f s | player %.0f %.0f %.0f", WeatherName(prev.weatherId),
            g_stormStartedAt >= 0.0f ? g_gameTime - g_stormStartedAt : -1.0f, g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z);
        g_stormStartedAt = -1.0f;
    }
    if (kind == WEATHER_LOCAL && prevKind != WEATHER_LOCAL)
        LogLine("WEATHER: entered local atmosphere %s -- storms cannot start here | player %.0f %.0f %.0f", WeatherName(now.weatherId), g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z);
    else if (prevKind == WEATHER_LOCAL && kind != WEATHER_LOCAL)
        LogLine("WEATHER: left local atmosphere %s | player %.0f %.0f %.0f", WeatherName(prev.weatherId), g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z);
}

static void WeatherTick(float dt) {
    g_gameTime += dt;
    g_weatherClock += dt;
    if (g_weatherClock < 1.0f) return;
    g_weatherClock = 0.0f;
    void* mgr = FindWeatherManager();
    WeatherReading r = { false, -1, 0, 0.0f, 0.0f, 0.0f, 0 };
    if (!mgr || !ReadWeather(mgr, &r)) {
        if (g_weather.ok) LogLine("WEATHER: signal lost");
        g_weather.ok = false; g_weatherKind = WEATHER_UNKNOWN;
        snprintf(g_stormProbeStatus, sizeof(g_stormProbeStatus), "weather: no signal (render hook %s)", g_todHookStatus);
        return;
    }
    WeatherKind kind = r.index < 0 ? WEATHER_CLEAR : (r.weatherId == -1 || r.weatherId == -2) ? WEATHER_STORM : WEATHER_LOCAL;
    if (!g_weather.ok) {
        LogLine("WEATHER: signal acquired (%s), index %d id %d", g_timeOfDayMgr ? "render hook" : "object lookup - 0x10", r.index, r.weatherId);
        if (kind == WEATHER_STORM) g_stormStartedAt = g_gameTime;
    } else if (kind != g_weatherKind || r.index != g_weather.index) {
        OnWeatherChanged(g_weather, g_weatherKind, r, kind);
    }
    g_weather = r; g_weatherKind = kind;
    snprintf(g_stormProbeStatus, sizeof(g_stormProbeStatus), "weather: %s (index %d, id %d, timer %.1f)",
        kind == WEATHER_CLEAR ? "clear" : WeatherName(r.weatherId), r.index, r.weatherId, r.timer);
}

// ---- Scheduler ----
enum StormSchedState { SCHED_GAP = 0, SCHED_AWAITING_ARRIVAL, SCHED_IN_STORM };
static StormSchedState g_schedState = SCHED_GAP;
static float g_awaitingFor = 0.0f;

static void TriggerStormNow(const char* why) {
    if (!SendEventMsg) return;
    g_stormTriggers++;
    g_fastStormsOn = true;
    SendEventMsg("storm.trigger");
    LogLine("STORM: storm.trigger sent (%s, #%d) | player %.0f %.0f %.0f", why, g_stormTriggers, g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z);
}

// Called from the SpawnStorm hook (which has never fired so far).
static void OnStormSpawned() { g_fastStormsOn = false; }

static void StartGap(const char* why) {
    g_schedState = SCHED_GAP;
    g_stormNextDelay = RollStormDelay();
    snprintf(g_stormToggleStatus, sizeof(g_stormToggleStatus), "%s; next storm after %.0f s of clear sky", why, g_stormNextDelay);
    LogLine("STORM: %s", g_stormToggleStatus);
}

static void StormPresetTick(float dt) {
    if (g_stormPreset == STORM_OFF) return;
    bool haveSignal = g_weather.ok;
    bool storm = haveSignal && g_weatherKind == WEATHER_STORM;
    bool local = haveSignal && g_weatherKind == WEATHER_LOCAL;

    if (!haveSignal) {
        // No weather signal: previous behaviour, a plain timer that assumes
        // the measured storm length.
        g_stormNextDelay -= dt;
        if (g_stormNextDelay > 0.0f) return;
        TriggerStormNow(g_stormPresets[g_stormPreset].name);
        g_stormNextDelay = RollStormDelay() + STORM_MEASURED_TOTAL_S;
        snprintf(g_stormToggleStatus, sizeof(g_stormToggleStatus), "no weather signal -- timed mode; next in %.0f s", g_stormNextDelay);
        return;
    }

    switch (g_schedState) {
    case SCHED_GAP:
        if (storm) {   // a late arrival of ours, or the game's own storm
            g_schedState = SCHED_IN_STORM;
            snprintf(g_stormToggleStatus, sizeof(g_stormToggleStatus), "storm in progress (the game's own or a late arrival)");
            LogLine("STORM: a storm is on while waiting for the next one; the gap restarts when it ends");
            return;
        }
        if (local) {
            snprintf(g_stormToggleStatus, sizeof(g_stormToggleStatus), "paused: inside %s; %.0f s left", WeatherName(g_weather.weatherId), g_stormNextDelay);
            return;
        }
        g_stormNextDelay -= dt;
        if (g_stormNextDelay > 0.0f) {
            snprintf(g_stormToggleStatus, sizeof(g_stormToggleStatus), "clear sky; next storm in %.0f s", g_stormNextDelay);
            return;
        }
        TriggerStormNow(g_stormPresets[g_stormPreset].name);
        g_schedState = SCHED_AWAITING_ARRIVAL;
        g_awaitingFor = 0.0f;
        snprintf(g_stormToggleStatus, sizeof(g_stormToggleStatus), "storm triggered, waiting for it to reach you");
        return;

    case SCHED_AWAITING_ARRIVAL:
        if (storm) {
            g_stormArrived++;
            g_schedState = SCHED_IN_STORM;
            LogLine("STORM: triggered storm #%d reached the player after %.0f s", g_stormTriggers, g_awaitingFor);
            snprintf(g_stormToggleStatus, sizeof(g_stormToggleStatus), "storm in progress");
            return;
        }
        g_awaitingFor += dt;
        if (g_awaitingFor >= STORM_ARRIVAL_TIMEOUT_S) {
            g_stormMissed++;
            char why[140];
            snprintf(why, sizeof(why), "storm #%d never reached the player in %.0f s (%s)", g_stormTriggers, STORM_ARRIVAL_TIMEOUT_S,
                local ? "inside a local atmosphere" : "outrun, or a no-storm area");
            StartGap(why);
        }
        return;

    case SCHED_IN_STORM:
        if (storm) return;
        StartGap("storm over");
        return;
    }
}

static void CycleStormPreset() {
    g_stormPreset = (g_stormPreset + 1) % STORM_PRESET_COUNT;
    if (g_stormPreset == STORM_OFF) {
        snprintf(g_stormToggleStatus, sizeof(g_stormToggleStatus), "off; the game's own cadence is untouched");
        LogLine("STORM: %s", g_stormToggleStatus);
        return;
    }
    char why[140];
    snprintf(why, sizeof(why), "preset %s", g_stormPresets[g_stormPreset].name);
    if (g_weather.ok && g_weatherKind == WEATHER_STORM) {
        g_schedState = SCHED_IN_STORM;
        snprintf(g_stormToggleStatus, sizeof(g_stormToggleStatus), "%s; a storm is on, the gap starts when it ends", why);
        LogLine("STORM: %s", g_stormToggleStatus);
    } else {
        StartGap(why);
    }
}

// ---- Storm observation hooks (2026-09-22) ----
// The SpawnStorm node hook has never logged a single line, not even in the
// session where a storm demonstrably happened, so it is not on the path the
// game actually takes. These two are: CStormConfig::FindStormDefinition is
// how a storm is resolved by name before it starts, and NGSONodes::InStormArea
// is what the graphs ask to know whether the player is inside one. Together
// they answer the open question -- does "storm.trigger" really create a
// storm, and how long does it take to arrive? Both are read-only.
int g_findStormDefCalls = 0;
int g_inStormAreaTrue = 0;
bool g_wasInStorm = false;

// The real signal. Neither SpawnStorm nor FindStormDefinition ever fired,
// not even while a storm raised by "storm.trigger" was visibly on screen, so
// the command works through the weather system rather than by spawning a
// storm object. NEnvironmentPreset::CEnvironmentPresetManager::SetCurrentWeatherId
// (0x1401CCD80) is where a weather change actually lands; logging its
// argument gives a timestamped record of every storm starting and ending.
DEFHOOK(void, SetCurrentWeatherId, (void* mgr, int weatherId)) {
    static int last = -12345;
    if (weatherId != last) {
        last = weatherId;
        LogLine("WEATHER: id -> %d", weatherId);
    }
    SetCurrentWeatherId_orig(mgr, weatherId);
}

DEFHOOK(void*, FindStormDefinition, (void* stormConfig, unsigned int nameHash)) {
    void* def = FindStormDefinition_orig(stormConfig, nameHash);
    g_findStormDefCalls++;
    if (g_findStormDefCalls <= 40)
        LogLine("STORM: FindStormDefinition(%u) -> %s (#%d)", nameHash, def ? "found" : "NOT found", g_findStormDefCalls);
    return def;
}

DEFHOOK(uint32_t, InStormArea, (void* processor, const void* node, unsigned int pin)) {
    uint32_t r = InStormArea_orig(processor, node, pin);
    // The node reports through its pins, so infer from how often it is asked:
    // log only the transitions, driven by the graph's own answer frequency.
    g_inStormAreaTrue++;
    if (g_inStormAreaTrue == 1) LogLine("STORM: InStormArea node started being evaluated");
    return r;
}

DEFHOOK(uint32_t, SpawnStorm, (void* processor, const void* node, unsigned int pin)) {
    g_spawnStormCalls++;
    // Logged, not just counted: the overlay counter is useless once the game
    // is closed, and whether storms actually start is the whole question the
    // fast-storm toggle is meant to answer.
    LogLine("STORM spawned (SpawnStorm call #%d)%s", g_spawnStormCalls, g_fastStormsOn ? " [asked for by the preset]" : " [the game's own schedule]");
    OnStormSpawned();
    return SpawnStorm_orig(processor, node, pin);
}


// ---- Captured/hijacked car test (2026-09-17) ----
// Every vehicle-upgrade path in the game data (InstallArchetype, all of
// lib_vehicleupgrades.xvmc's Change*/Get*UpgradeLevel functions) hardcodes
// CVehicleUpgradeManager::GetSignatureVehicle() as the target -- Max's own
// persistent car. Nothing resolves "whatever CVehicle the player currently
// occupies", which is what a hijacked enemy car would be. Found the native,
// per-part functions that take a plain CVehicle* instead of resolving the
// signature vehicle internally, so they should work on any vehicle:
//   CVehicle::SetMass(float) @ 0x140575C50 (plain member fn, not virtual --
//   confirmed by the PDB demangling showing no "virtual" keyword, unlike its
//   neighbors like CPfxRigidBody::SetMass -- so a direct address call with
//   `this` as first arg is safe, same convention already used in this file
//   for GetConvoyContainer/CGameObject_FindOptional).
// Two probes, both read-only/additive:
//   1. Every frame, compare ch->GetVehiclePtr() (whatever car Max currently
//      occupies) against CVehicleUpgradeManager::GetSignatureVehicle() (his
//      persistent car) -- if they're ever different pointers, that PROVES a
//      hijacked car is a distinct CVehicle instance, confirming the premise
//      before even testing a write.
//   2. F6 one-shot: call CVehicle::SetMass on the CURRENT vehicle (not the
//      signature one) with a deliberately light test value -- if it's a
//      hijacked car and it suddenly handles much lighter, that's a live
//      confirmation the function isn't restricted to the signature vehicle.
// Mass is NOT restored automatically when toggled/pressed again -- this is
// a one-shot diagnostic press, not a toggle. Test on save 4 only.
typedef void* (*GetSignatureVehicleFn)();
static GetSignatureVehicleFn GetSignatureVehicleNative = (GetSignatureVehicleFn)0x140051870;

typedef void (*VehicleSetMassFn)(void* thiz, float mass);
static VehicleSetMassFn VehicleSetMass = (VehicleSetMassFn)0x140575C50;

const float g_testVehicleMass = 200.0f; // deliberately light vs a normal car, to make the effect obvious

bool f6Pressed = false;
int g_setMassPresses = 0;
void* g_lastCurrentVehicle = nullptr;
void* g_lastSignatureVehicle = nullptr;

// ---- Chassis-level access (2026-09-17) ----
// CVehicle::GetChassis() isn't a clean separate function -- disassembling
// the CVehicleModule::GetChassis XVM bridge (the only working reference we
// had) showed it's inlined: a virtual call through vtable slot 2 (type
// check), then a `this - 8` adjustment (multiple-inheritance thunk), then a
// raw field read at [thatadjusted pointer + 0x1648]. Too fragile to
// replicate by hand (offset/adjustment could differ across CVehicle
// subtypes) when there's a clean alternative: CVehicle::GetPfxVehicle()
// (plain member fn, confirmed via PDB) returns a CPfxVehicle*, which itself
// has a clean CPfxVehicle::GetChassis() (also plain, already used to
// confirm CPhysicsVehicleChassis::SetEngineTorqueScale/
// SetEngineResistanceScale/ClearAllTireParameters/SetMass earlier this
// session by disassembling the other three XVM bridges). Chain:
//   CVehicle::GetPfxVehicle() -> CPfxVehicle::GetChassis() -> CPhysicsVehicleChassis*
// then call any of its plain setters directly. Untested past this comment
// -- F7 below is the first live test, same one-shot/no-restore pattern as F6.
typedef void* (*GetPfxVehicleFn)(void* thiz);
static GetPfxVehicleFn VehicleGetPfxVehicle = (GetPfxVehicleFn)0x140501A60;

typedef void* (*PfxGetChassisFn)(void* thiz);
static PfxGetChassisFn PfxVehicleGetChassis = (PfxGetChassisFn)0x140425500;

typedef void (*ChassisSetEngineTorqueScaleFn)(void* thiz, float scale);
static ChassisSetEngineTorqueScaleFn ChassisSetEngineTorqueScale = (ChassisSetEngineTorqueScaleFn)0x140837940;

const float g_testTorqueScale = 3.0f; // 3x normal engine torque, should feel like a very obvious acceleration boost

bool f7Pressed = false;
int g_setTorquePresses = 0;
void* g_lastChassis = nullptr;

// ---- Convoy respawn without an area transition (2026-09-18) ----
// The real reason the mod needs an area change today: each convoy's graph
// script only runs its spawn-decision logic (checks EFlags_Wrecked, spawns
// vehicles if clear) once, when the containing world region streams in --
// it isn't a loop that re-checks periodically. Our SetWrecked hook already
// clears the flag in memory the instant a convoy is destroyed, but nothing
// tells the (already-loaded, not re-streaming) graph to run again.
//
// SpawnSystemReset(bool) -- the function the third-party CT table calls
// "Nuke All Spawned Entities" -- is a thin wrapper (verified by disassembly)
// that stores the bool and tail-jumps straight into
// CEncounterSpawning::Reset(void) (0x1404f51d0, found via that jump target
// in the PDB). Disassembled Reset() in full: two loops over the encounter
// system's own two internal arrays (stride 0x48 and stride 8), each just
// writing a "needs re-evaluation" flag byte -- ZERO call instructions
// anywhere in the function body. It cannot touch anything outside its own
// bookkeeping, which includes the region Threat system (a separate global,
// NRegionInfo::g_ThreatsInRegions, never referenced here) -- so this cannot
// be the thing that pushes a region's Threat back up. Not yet empirically
// confirmed in-game; that's what F8 below is for.
//
// Plan: F8 does ResetAllKnownConvoys() (clear EFlags_Wrecked on all 14,
// same as F4) immediately followed by SpawnSystemReset(true) (flag the
// encounter system to re-evaluate everything nearby) -- testing whether
// this alone is enough to make a convoy actually respawn without leaving
// the area, and whether the region's Threat level visibly moves at all
// (watch the map/HUD threat display before and after pressing F8).
typedef void (*SpawnSystemResetFn)(bool force);
static SpawnSystemResetFn SpawnSystemReset = (SpawnSystemResetFn)0x1404f7410;

bool f8Pressed = false;
int g_spawnSystemResetPresses = 0;
#endif // MM_DEV_TOOLS

void ResetAllKnownConvoys() {
    int found = 0, cleared = 0, skipped = 0;
    for (int i = 0; i < g_knownConvoyCount; i++) {
        uint8_t sharedPtrBuf[16] = { 0 };
        bool ok = CGameObject_FindOptional(g_knownConvoyIds[i], sharedPtrBuf);
        if (!ok) continue;
        void* obj = *(void**)sharedPtrBuf; // px
        if (!obj) continue;
        found++;
        uint8_t* flags = (uint8_t*)((uintptr_t)obj + 0x118);
        if ((*flags & ~0x3) != 0) {
            skipped++; // doesn't look like a real EFlags byte, don't touch it
            continue;
        }
        *flags &= ~0x1; // clear EFlags_Wrecked
        cleared++;
    }
    g_resetConvoysLastFound = found;
    g_resetConvoysLastCleared = cleared;
    g_resetConvoysLastSkippedSanity = skipped;
}

CVector3f g_PlayerPos(0.f, 0.f, 0.f);

// ---- Convoy composition: resolver plumbing (release, 2026-09-22) ----
// Addresses come from the signature table; see the composition patch below.
typedef bool (*ResolveConvoysCompositionFn)(void* thiz, unsigned int id, void** outProfile);
typedef int (*ResourceTableAccessorFn)(void* handle, void** outArray);
static ResourceTableAccessorFn CompositionTableAccessor = nullptr; // from g_sigs[SIG_TABLEACCESSOR]
static void** g_aiConstantsManagerPtr = nullptr;                   // from g_sigs[SIG_MANAGERPTR]

int g_compositionResolveCalls = 0;
unsigned int g_compositionLastId = 0;
bool g_compositionLastOk = false;
int g_compositionLastGuardEntries = -1;
const char* g_compositionResolveHookInstallStatus = "not attempted yet";
bool g_compositionDumpDone = false;
char g_compositionDumpStatus[160] = "not dumped yet";
bool f10Pressed = false;
#if MM_DEV_TOOLS
// ---- Convoy composition profile dump (2026-09-21, read-only) ----
// Goal: see the real convoy compositions ("add more cars" request).
// NGSONodes::ConvoysCompositionIterateGuards (0x1402C3B30, disassembled)
// calls CAIConstantsProfilesManager::ResolveConvoysCompositionCritical
// (0x1400491A0) with the convoy's ConvoyCompositionId and iterates the
// resolved SAIProfile's m_KeyValuesHash entries -- one RequestSpawn per
// entry (minus the one whose key matches the leader). So the escort count
// is literally that array's m_Count. Resolve() looks the id up in a table
// of SAIProfile (stride 0x38) reached through manager+0x60 -> a tiny pure
// accessor (0x140A003B0: out = handle->owner->table[handle->index]) ->
// { SAIProfile* data; uint count }. The manager singleton pointer lives at
// 0x141711AF8 (rip-relative operand in IterateGuards).
//
// This build only READS: a hook on Resolve counts calls and remembers the
// last id/profile, and DumpCompositionProfiles() walks the whole table and
// writes every profile (id, float/hash/string key-values) to a text file
// next to the game. Runs automatically once, right after the first
// successful Resolve (the table is guaranteed loaded then), and on F10.
// Layouts (Dia2Dump): SAIProfile { u64 m_ID; {SKeyValueFloat* p; u32 n} @+0x08;
// {SKeyValueHash* p; u32 n} @+0x18; {SKeyValueString* p; u32 n} @+0x28 };
// SKeyValueHash { u64 m_Key; u64 m_Value }; SKeyValueFloat { u64 m_Key;
// float m_Value }; SKeyValueString { u64 m_Key; const char* m_Value }.
void DumpCompositionProfiles() {
    if (!g_aiConstantsManagerPtr || !CompositionTableAccessor) { snprintf(g_compositionDumpStatus, sizeof(g_compositionDumpStatus), "composition signatures not resolved on this exe"); return; }
    void* manager = *g_aiConstantsManagerPtr;
    if (!manager) { snprintf(g_compositionDumpStatus, sizeof(g_compositionDumpStatus), "manager singleton is null"); return; }
    void* handle = *(void**)((uintptr_t)manager + 0x60);
    if (!handle) { snprintf(g_compositionDumpStatus, sizeof(g_compositionDumpStatus), "composition resource handle (manager+0x60) is null"); return; }
    void* arr = nullptr;
    CompositionTableAccessor(handle, &arr);
    if (!arr) { snprintf(g_compositionDumpStatus, sizeof(g_compositionDumpStatus), "accessor returned null array"); return; }
    const uint8_t* data = *(const uint8_t**)arr;
    unsigned int count = *(const unsigned int*)((uintptr_t)arr + 8);
    if (!data || count > 4096) { snprintf(g_compositionDumpStatus, sizeof(g_compositionDumpStatus), "implausible table (data=%p count=%u)", (void*)data, count); return; }

    FILE* f = nullptr;
    fopen_s(&f, g_dumpPath, "w");
    if (!f) { snprintf(g_compositionDumpStatus, sizeof(g_compositionDumpStatus), "could not open convoy_composition_dump.txt"); return; }
    fprintf(f, "CAIConstantsProfilesManager convoy composition table: %u profiles (SAIProfile stride 0x38)\n\n", count);
    for (unsigned int i = 0; i < count; i++) {
        const uint8_t* prof = data + (size_t)i * 0x38;
        unsigned long long id = *(const unsigned long long*)prof;
        const uint8_t* fData = *(const uint8_t* const*)(prof + 0x08); unsigned int fN = *(const unsigned int*)(prof + 0x10);
        const uint8_t* hData = *(const uint8_t* const*)(prof + 0x18); unsigned int hN = *(const unsigned int*)(prof + 0x20);
        const uint8_t* sData = *(const uint8_t* const*)(prof + 0x28); unsigned int sN = *(const unsigned int*)(prof + 0x30);
        fprintf(f, "profile[%u] m_ID=0x%016llX (low32 %u / %d)  floats=%u hashes=%u strings=%u\n", i, id, (unsigned)id, (int)(unsigned)id, fN, hN, sN);
        if (fData && fN < 1024) for (unsigned int k = 0; k < fN; k++) {
            unsigned long long key = *(const unsigned long long*)(fData + k * 16); float v = *(const float*)(fData + k * 16 + 8);
            fprintf(f, "  float  [%u] key=0x%016llX (%u)  value=%g\n", k, key, (unsigned)key, v);
        }
        if (hData && hN < 1024) for (unsigned int k = 0; k < hN; k++) {
            unsigned long long key = *(const unsigned long long*)(hData + k * 16); unsigned long long v = *(const unsigned long long*)(hData + k * 16 + 8);
            fprintf(f, "  hash   [%u] key=0x%016llX (%u)  value=0x%016llX (%u)\n", k, key, (unsigned)key, v, (unsigned)v);
        }
        if (sData && sN < 1024) for (unsigned int k = 0; k < sN; k++) {
            unsigned long long key = *(const unsigned long long*)(sData + k * 16); const char* v = *(const char* const*)(sData + k * 16 + 8);
            fprintf(f, "  string [%u] key=0x%016llX (%u)  value=\"%s\"\n", k, key, (unsigned)key, v ? v : "(null)");
        }
        fprintf(f, "\n");
    }
    fclose(f);
    g_compositionDumpDone = true;
    snprintf(g_compositionDumpStatus, sizeof(g_compositionDumpStatus), "wrote %u profiles to convoy_composition_dump.txt", count);
}
#else
static void DumpCompositionProfiles() {}
#endif

// ---- Convoy composition patch: extra escorts (2026-09-21, test) ----
// The dump (convoy_compositions_resolved.txt) confirmed the model: every
// profile's m_KeyValuesHash is { Leader -> leader vehicle, <slot> -> escort
// vehicle, ... }, 1 leader + 4..9 escorts, and IterateGuards simply walks
// that array. So "more cars" = a longer array. On the first successful
// Resolve (table guaranteed loaded), each profile gets a fresh, larger
// array: the original entries, then COMPOSITION_EXTRA_ESCORTS extra entries
// whose vehicle is copied round-robin from the profile's own escorts and
// whose slot key is a new unique hash (Jenkins of "ModGuard1".."ModGuard8",
// precomputed) -- the CCMap slot tracking is keyed on that hash, so the
// keys must be unique and must not be the Leader key (0x6FBB4E7F, which
// IterateGuards skips). The old arrays are left untouched (never freed;
// they belong to the loaded resource). The mm3030 mission convoy's profile
// (0xF6F655B0, landmover) is skipped: it is scripted mission content.
// UNVERIFIED: whether road formations / despawn / CCMap cope with more
// slots than any shipped profile has (max shipped is 10 entries). Start
// with +2. Test on save 4 only.
const int COMPOSITION_EXTRA_ESCORTS = 2;
const unsigned long long COMPOSITION_LEADER_KEY = 0x6FBB4E7Full; // "Leader"

// Vehicle-name hashes seen in the composition table (resolved by brute force,
// see convoy_composition_names.json). The spawn system draws vehicles from
// pre-allocated pools per class (spawning/spawn_types.spawnresourcesc):
// Scrotus light = 12 instances (spotter, spotter_armored, fire_raider),
// medium = 7 (rammerhead, metalgrinder), heavy = 3 -- shared with every
// other Scrotus car in the world. The first +2 build copied a profile's own
// first escorts, which on the big "_db" profiles meant 2 more MEDIUMS on top
// of 4-5 -> SpawnResourcesLoaded failed for one id -> the whole batch was
// dropped and retried forever (dust cloud, no cars). Extras are therefore
// always LIGHT-class now.
struct VehicleName { unsigned int hash; const char* name; };
static const VehicleName g_vehicleNames[] = {
    { 0x73989A77u, "scrotus_spotter" }, { 0x44E7989Bu, "scrotus_spotter_armored" }, { 0x0C15F832u, "scrotus_spotter_mm3030" },
    { 0xE12E9B14u, "scrotus_rammerhead" }, { 0x923BD84Fu, "scrotus_rammerhead_db" }, { 0x07D44A49u, "scrotus_rammerhead_mm3030" },
    { 0xD72C2F79u, "scrotus_metalgrinder" }, { 0x06F6D475u, "scrotus_metalgrinder_db" }, { 0x7A36581Du, "scrotus_metalgrinder_mm3030" },
    { 0x4180510Du, "scrotus_fire_raider" }, { 0x27515F9Fu, "scrotus_fire_raider_db" }, { 0x6FC80099u, "scrotus_fire_raider_mm3030" },
    { 0x71CA9B2Eu, "scrotus_convoy_leader_fueler" }, { 0x9E0099F3u, "scrotus_convoy_leader_topdog" }, { 0xA56980F4u, "scrotus_convoy_leader_truck" },
    { 0x66CBCEDBu, "scrotus_convoy_leader_landmover_mm3030" },
};
static const char* VehicleNameOf(unsigned long long v) {
    for (const VehicleName& n : g_vehicleNames) if (n.hash == (unsigned int)v) return n.name;
    return nullptr;
}
static const unsigned long long VEH_SPOTTER = 0x73989A77ull, VEH_FIRE_RAIDER = 0x4180510Dull, VEH_FIRE_RAIDER_DB = 0x27515F9Full,
                                VEH_RAMMERHEAD_DB = 0x923BD84Full, VEH_METALGRINDER_DB = 0x06F6D475ull;
const unsigned long long COMPOSITION_SKIP_PROFILE_ID = 0xF6F655B0ull; // mm3030 landmover convoy
// v2 of the patch (2026-09-21, after a live crash "when approaching the
// convoy" with invented slot keys): every shipped profile uses the SAME nine
// escort slot keys in the SAME order (a 4-escort profile uses the first
// four, the mm3030 profile all nine) -- almost certainly formation
// positions defined per slot elsewhere in the engine, so an unknown key has
// no position to take. Extra escorts therefore reuse the next unused keys
// of that canonical sequence, and a profile never exceeds nine escorts.
// Slots 1-9 are the game's own point names (convoy_map defines 1-7, the
// mm3030 landmover map 1-9). Slots 10-12 are ours ("ModSlot10".."ModSlot12",
// Jenkins) and only exist in the modded convoy_map shipped as
// dropzone/global/car_combat.blo (12 points: the 7 originals + 5 behind the
// convoy at z = 60..100 m). Without that dropzone file the engine cap is 7.
static const unsigned long long g_canonicalEscortSlotKeys[12] = {
    0xD89FB74Bull, 0xD950BF1Eull, 0xE4CE1F86ull, 0x4381CB33ull, 0xB1CBD58Bull,
    0xB49EC145ull, 0x4DA5C974ull, 0x23DDBD0Dull, 0x928325D0ull,
    0x2257A626ull, 0xBEF5FB0Eull, 0x73C7B796ull,
};
// How many formation points convoy_map actually has. The stock map defines 7;
// asking for an escort slot it does not define makes the game dereference a
// null map point and crash (confirmed 2026-09-21). The mod ships a modded
// dropzone/global/car_combat.blo with 12 points, so the cap is raised only
// when that file is present AND contains the new point name hashes.
int g_convoyMapPoints = 7;
char g_convoyMapStatus[128] = "stock map assumed (7 formation points)";
static void DetectConvoyMapPoints() {
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s..\\dropzone\\global\\car_combat.blo", g_modDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) { snprintf(g_convoyMapStatus, sizeof(g_convoyMapStatus), "no dropzone car_combat.blo -> 7 escort slots"); return; }
    static unsigned char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    const unsigned int wanted[3] = { 0x2257A626u, 0xBEF5FB0Eu, 0x73C7B796u }; // ModSlot10..12 (12-point map marker)
    int found = 0;
    for (unsigned int w : wanted)
        for (size_t i = 0; i + 4 <= n; i++)
            if (*(const unsigned int*)(buf + i) == w) { found++; break; }
    // Cap at 9 even with the 12-point map: the limit that actually bites is the
    // per-class vehicle pool in spawn_types, not formation points, and 9 is the
    // largest escort count the game itself ships (the mm3030 landmover convoy).
    if (found == 3) { g_convoyMapPoints = 9; snprintf(g_convoyMapStatus, sizeof(g_convoyMapStatus), "modded car_combat.blo found -> up to 9 escort slots"); }
    else snprintf(g_convoyMapStatus, sizeof(g_convoyMapStatus), "dropzone car_combat.blo present but not the modded one (%d/3 markers) -> 7 escort slots", found);
}

bool g_compositionPatchDone = false;
int g_compositionPatchedProfiles = 0;
int g_compositionPatchSkipped = 0;
char g_compositionPatchStatus[160] = "not applied yet";

static void PatchCompositionProfiles() {
    if (g_compositionPatchDone) return;
    if (!g_aiConstantsManagerPtr || !CompositionTableAccessor) { snprintf(g_compositionPatchStatus, sizeof(g_compositionPatchStatus), "composition signatures not resolved on this exe"); return; }
    void* manager = *g_aiConstantsManagerPtr;
    if (!manager) { snprintf(g_compositionPatchStatus, sizeof(g_compositionPatchStatus), "manager null"); return; }
    void* handle = *(void**)((uintptr_t)manager + 0x60);
    if (!handle) { snprintf(g_compositionPatchStatus, sizeof(g_compositionPatchStatus), "resource handle null"); return; }
    void* arr = nullptr;
    CompositionTableAccessor(handle, &arr);
    if (!arr) { snprintf(g_compositionPatchStatus, sizeof(g_compositionPatchStatus), "table null"); return; }
    uint8_t* data = *(uint8_t**)arr;
    unsigned int count = *(unsigned int*)((uintptr_t)arr + 8);
    if (!data || count > 4096) { snprintf(g_compositionPatchStatus, sizeof(g_compositionPatchStatus), "implausible table"); return; }

    const int extra = COMPOSITION_EXTRA_ESCORTS > 8 ? 8 : COMPOSITION_EXTRA_ESCORTS;
    for (unsigned int i = 0; i < count; i++) {
        uint8_t* prof = data + (size_t)i * 0x38;
        unsigned long long id = *(unsigned long long*)prof;
        unsigned long long* hData = *(unsigned long long**)(prof + 0x18);
        unsigned int hN = *(unsigned int*)(prof + 0x20);
        if (!hData || hN == 0 || hN > 64) { g_compositionPatchSkipped++; continue; }
        if ((id & 0xFFFFFFFFull) == COMPOSITION_SKIP_PROFILE_ID) { g_compositionPatchSkipped++; continue; }

        // collect escort entries (everything but the Leader) to copy vehicles from
        unsigned int escortIdx[64]; unsigned int escorts = 0;
        for (unsigned int k = 0; k < hN; k++) if (hData[k * 2] != COMPOSITION_LEADER_KEY) escortIdx[escorts++] = k;
        if (escorts == 0) { g_compositionPatchSkipped++; continue; }

        // which canonical slot keys does this profile already use?
        bool used[12] = { false };
        for (unsigned int k = 0; k < hN; k++)
            for (int c = 0; c < 12; c++) if (hData[k * 2] == g_canonicalEscortSlotKeys[c]) used[c] = true;
        // v3 (2026-09-21, after the captured crash): the slot keys are the
        // names of SCarCombatMapPosition points in global/car_combat.blo, and
        // regular convoys use "convoy_map", which defines exactly slots 1-7.
        // Slot 8 (0x23DDBD0D) exists only in the mm3030 landmover map -- the
        // captured crash was ResolveMapPointCritical(0x23DDBD0D) returning
        // null on a regular leader. So never go past the 7 points the map
        // has; more than that needs extra points added to convoy_map itself.
        const int CONVOY_MAP_POINTS = g_convoyMapPoints;
        unsigned long long freeKeys[12]; int freeCount = 0;
        for (int c = 0; c < CONVOY_MAP_POINTS; c++) if (!used[c]) freeKeys[freeCount++] = g_canonicalEscortSlotKeys[c];
        int add = extra < freeCount ? extra : freeCount;
        if (add <= 0) { g_compositionPatchSkipped++; continue; } // already using every point convoy_map defines

        unsigned long long* fresh = (unsigned long long*)malloc((size_t)(hN + add) * 16);
        if (!fresh) { g_compositionPatchSkipped++; continue; }
        memcpy(fresh, hData, (size_t)hN * 16);
        // The big "_db" profiles (convoys 3/8/10, 6-7 escorts already) are left
        // alone: with extra escorts their spawn batch never completes --
        // SpawnHoldAfterResourcesLoaded stops firing, so SpawnBegin/End never
        // pairs up and the global batch flag (CSpawnedEntityHandler+0x18E,
        // which SpawnMultipleEntitiesAvailable simply reads) stays set, making
        // every convoy retry forever: dust cloud, no cars. Enlarging the
        // per-class vehicle pools did not change this, so the cause is in the
        // resource-hold path, not the pools. Small and medium convoys take the
        // +2 happily (verified in game: a 5-car convoy became 7).
        bool dbProfile = false;
        for (unsigned int k = 0; k < hN; k++) {
            unsigned long long v = hData[k * 2 + 1];
            if (v == VEH_RAMMERHEAD_DB || v == VEH_METALGRINDER_DB || v == VEH_FIRE_RAIDER_DB) dbProfile = true;
        }
        if (dbProfile || hN >= 8) { g_compositionPatchSkipped++; continue; }
        for (int e = 0; e < add; e++) {
            fresh[(hN + e) * 2 + 0] = freeKeys[e];             // next unused canonical slot key
            // light-class vehicle only (12-instance pool): alternate spotter / fire raider;
            // "_db" profiles get the _db fire raider (the only light _db entity we know exists)
            fresh[(hN + e) * 2 + 1] = (e % 2) ? VEH_FIRE_RAIDER : VEH_SPOTTER;   // light class only
        }
        *(unsigned long long**)(prof + 0x18) = fresh;
        *(unsigned int*)(prof + 0x20) = hN + add;
        g_compositionPatchedProfiles++;
    }
    g_compositionPatchDone = true;
    LogLine("EVENT composition patch: +%d escorts on %d profiles (%d skipped)", extra, g_compositionPatchedProfiles, g_compositionPatchSkipped);
    snprintf(g_compositionPatchStatus, sizeof(g_compositionPatchStatus), "+%d escorts on %d convoy types (%d left stock: the big ones and the story convoy)", extra, g_compositionPatchedProfiles, g_compositionPatchSkipped);
}

DEFHOOK(bool, ResolveConvoysComposition, (void* thiz, unsigned int id, void** outProfile)) {
    bool ok = ResolveConvoysComposition_orig(thiz, id, outProfile);
    g_compositionResolveCalls++;
    g_compositionLastId = id;
    g_compositionLastOk = ok;
    {
        static unsigned int seen[32]; static int nseen = 0; bool isNew = true;
        for (int i = 0; i < nseen; i++) if (seen[i] == id) { isNew = false; break; }
        if (isNew && nseen < 32) seen[nseen++] = id;
        if (isNew) LogLine("EVENT composition resolved for id %u (%d): %s, %u hash entries", id, (int)id, ok ? "found" : "NOT FOUND",
            (ok && outProfile && *outProfile) ? *(const unsigned int*)((uintptr_t)*outProfile + 0x20) : 0u);
    }
    if (ok && outProfile && *outProfile) {
        if (!g_compositionDumpDone) DumpCompositionProfiles();   // dump the vanilla table first
        if (!g_compositionPatchDone) PatchCompositionProfiles();  // then extend it, before IterateGuards reads the count
        g_compositionLastGuardEntries = (int)*(const unsigned int*)((uintptr_t)*outProfile + 0x20);
        LogLine("EVENT convoy spawning with composition id %u: %d entries after patch (1 leader + %d escorts)", id, g_compositionLastGuardEntries, g_compositionLastGuardEntries - 1);
        {
            const unsigned long long* h = *(const unsigned long long* const*)((uintptr_t)*outProfile + 0x18);
            unsigned int n = *(const unsigned int*)((uintptr_t)*outProfile + 0x20);
            int light = 0, medium = 0;
            for (unsigned int k = 0; h && k < n && k < 32; k++) {
                const char* nm = VehicleNameOf(h[k * 2 + 1]);
                LogLine("    slot %08X -> %08X %s", (unsigned)h[k * 2], (unsigned)h[k * 2 + 1], nm ? nm : "(unresolved name)");
                if (nm && (strstr(nm, "spotter") || strstr(nm, "fire_raider"))) light++;
                if (nm && (strstr(nm, "rammerhead") || strstr(nm, "metalgrinder"))) medium++;
            }
            LogLine("    pool use (known names only): light %d of 12, medium %d of 7", light, medium);
        }
    }
    return ok;
}

// ---- Crash stack capture (2026-09-21, dev only) ----
// Two live crashes with the composition patch, both at AVAMain.exe+0x9E1277
// = CQuaternion::ToMatrix4 reading through a null `this` (per the Windows
// Application Error events). The fault site alone doesn't say WHO passed
// the null transform, and static analysis found 49 distinct callers. This
// vectored exception handler runs before the OS crash dialog: on the first
// access violation it writes the register state and a StackWalk64 of the
// faulting thread (module+offset per frame, resolved offline against the
// PDB) to scripts\convoy_crash_stack.txt, then lets the crash proceed
// unchanged (EXCEPTION_CONTINUE_SEARCH). Read-only, no game state touched.
static volatile long g_crashCaptured = 0;

static LONG CALLBACK CrashStackHandler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&g_crashCaptured, 1) != 0) return EXCEPTION_CONTINUE_SEARCH;

    LogLine("CRASH: exception 0x%08X at %p -- stack written to convoy_crash_stack.txt", (unsigned)code, ep->ExceptionRecord->ExceptionAddress);
    FILE* f = nullptr;
    fopen_s(&f, g_crashPath, "w");
    if (!f) return EXCEPTION_CONTINUE_SEARCH;

    CONTEXT ctx = *ep->ContextRecord;
    uintptr_t exeBase = (uintptr_t)GetModuleHandleA(NULL);
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&CrashStackHandler, &self);
    uintptr_t asiBase = (uintptr_t)self;

    fprintf(f, "build %s\n", MM_BUILD_TAG);
    fprintf(f, "exception 0x%08X at %p", (unsigned)code, ep->ExceptionRecord->ExceptionAddress);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        fprintf(f, "  (%s address %p)", ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read", (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    fprintf(f, "\nexe base %p   asi base %p\n", (void*)exeBase, (void*)asiBase);
    fprintf(f, "rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX\nrsi=%016llX rdi=%016llX rbp=%016llX rsp=%016llX\nr8 =%016llX r9 =%016llX r10=%016llX r11=%016llX\nr12=%016llX r13=%016llX r14=%016llX r15=%016llX\n",
        ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi, ctx.Rbp, ctx.Rsp, ctx.R8, ctx.R9, ctx.R10, ctx.R11, ctx.R12, ctx.R13, ctx.R14, ctx.R15);

    HANDLE proc = GetCurrentProcess();
    HANDLE thr = GetCurrentThread();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(proc, NULL, TRUE);
    STACKFRAME64 sf = {};
    sf.AddrPC.Offset = ctx.Rip;    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
    fprintf(f, "\nstack (StackWalk64):\n");
    for (int i = 0; i < 64; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thr, &sf, &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) break;
        uintptr_t pc = (uintptr_t)sf.AddrPC.Offset;
        if (!pc) break;
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &m);
        char modName[MAX_PATH] = "?";
        if (m) GetModuleFileNameA(m, modName, sizeof(modName));
        const char* base = strrchr(modName, '\\'); base = base ? base + 1 : modName;
        fprintf(f, "  #%02d %p  %s+0x%llX\n", i, (void*)pc, base, (unsigned long long)(m ? pc - (uintptr_t)m : pc));
    }
    // raw stack words as a fallback for the offline resolver
    fprintf(f, "\nraw stack (first 96 qwords from rsp):\n");
    const unsigned long long* sp = (const unsigned long long*)ep->ContextRecord->Rsp;
    for (int i = 0; i < 96; i++) {
        unsigned long long v = 0;
        __try { v = sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (v >= exeBase && v < exeBase + 0x01AA8000ull) fprintf(f, "  [%02d] %016llX  exe+0x%llX\n", i, v, v - exeBase);
        else if (asiBase && v >= asiBase && v < asiBase + 0x100000ull) fprintf(f, "  [%02d] %016llX  asi+0x%llX\n", i, v, v - asiBase);
    }
    fclose(f);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- Convoy respawn v2: automatic, gated on the game's own wreck lifecycle (2026-09-20) ----
// How the game actually handles a wrecked convoy (from tracing the .gsrc
// files with gsrc_trace.py, wiring format solved the same day): on wreck, the
// choreographer runs threat_transfer, then convoy_wreck_handler, which loops
// until (a) the player has collected the hood ornament (relic streamed
// in/out at 300 m) and (b) every wreck has been despawned by the spawn
// system's range priority. Only then does the graph Exit, the engine frees
// its processor, and the CGraphScriptGameObject parks in m_State == 4. There
// is no timer anywhere in that flow -- persistence is distance + collection.
//
// v2 rides on exactly that: each second, for all 14 known convoys (container
// and LogicGraph object IDs both come from convoys.blo, pairs verified), if
// the container is wrecked AND its graph object is in state 4, the convoy is
// "armed" with a random delay (the same [min,max] random-interval pattern the
// game's own encounter system uses, e.g. encounter_storm_generate_interval).
// When the delay has elapsed AND the player is farther than the convoy's own
// spawn radius (2000 m, LogicGraph field 1DD9C0E5 -> RequestSpawn) from the
// wreck spot, EFlags_Wrecked is cleared and m_State is set to 0, which makes
// CGraphScriptGameObject::UpdatePostSim rebuild the graph + processor and
// fire the default Start -- the same path a save load takes, scoped to one
// object. Live-confirmed on 2026-09-20 (manual F9 version).
//
// Because everything is re-resolved by object ID every sweep, this survives
// save reloads (no cached pointers across loads) and also picks up convoys
// that were wrecked before the mod was installed (they load straight into
// state 4 via the choreographer's "GRAPH EXIT ON LOAD" start).
//
// The wreck position (CConvoyDataContainer::m_WreckedTransform, this+0xD8,
// Dia2Dump-confirmed) is only written at wreck time; if it reads as zero
// (wreck from a previous session), the distance gate is skipped and the
// delay alone applies -- the worst case is then identical to vanilla
// behaviour on a fresh load near a route.
struct ConvoyPair { uint64_t containerId; uint64_t logicGraphId; unsigned routeId; };   // routeId: field 30E81847 of the logic graph, what GUIXAddConvoyRoute keys the map route by
static const ConvoyPair g_convoyPairs[] = {
    { 0x7E90E3F6, 0xE6D229A4, 0xFEF9954A },
    { 0x38A45D73, 0x65271E53, 0x00000000 },
    { 0x8F1728CD, 0x8902D2DC, 0x5FD12B2C },
    { 0x59501178, 0xEA9391A6, 0xE3CA191D },
    { 0x7D6BB232, 0xC52D952C, 0x94EC179D },
    { 0x132E3492, 0x12F7F772, 0x98AEFE84 },
    { 0x42D456AE, 0xA79A34DB, 0xF77B37E6 },
    { 0x7C5903DF, 0x02794167, 0x7EC00658 },
    { 0x6FDA7EF0, 0x7389B76D, 0x5350F446 },
    { 0x337019D7, 0x70F9DD52, 0x8B7A9F9C },
    { 0x1FA21EA1, 0xC415F9A8, 0xBFF0EB2B },
    { 0xB6418D01, 0x84FCD7AC, 0x9387CF7B },
    { 0x74C87945, 0x2C42382A, 0x610AD2CE },
    { 0x35762FBA, 0x204F397C, 0xE016B9F6 },
};
const int g_convoyPairCount = sizeof(g_convoyPairs) / sizeof(g_convoyPairs[0]);

// The wreck handler marks the map route wrecked (GUIXSetConvoyRouteWrecked always passes true) and the map draws such a route as completed. Nothing clears the mark, as a
// convoy never comes back in the vanilla game, so a respawned convoy kept a finished route until the next load. Fixed address, checked first: other builds keep the mark.
static void ConvoyRouteClearWrecked(int i) {
    static const uintptr_t A_GameGuiSetConvoyRouteWrecked = 0x14022FA10;
    static const uint8_t head[3] = { 0x4C, 0x8B, 0x05 };
    unsigned id = g_convoyPairs[i].routeId;
    if (!id) return;
    __try {
        const uint8_t* p = (const uint8_t*)A_GameGuiSetConvoyRouteWrecked;
        if (memcmp(p, head, sizeof head) || p[7] != 0x4C || p[8] != 0x8B || p[9] != 0x15) { LogLine("convoy %2d: map route left as it is (unknown code)", i + 1); return; }
        ((void (*)(const unsigned*, bool))A_GameGuiSetConvoyRouteWrecked)(&id, false);
        LogLine("convoy %2d: map route %08X no longer marked wrecked", i + 1, id);
    } __except (EXCEPTION_EXECUTE_HANDLER) { LogLine("convoy %2d: map route call FAULTED", i + 1); }
}

// Convoy 1 is Dim Rim's, the only convoy on the map that starts switched off (ConvoyActive 0): mission mm1038 sends the event behind Start 903D2DBA, which sets that flag, and a graph rebuilt by a load or a
// respawn starts off again. Once the mission is done nothing sends it any more, so the route stays on the map with no cars. Fired here the way the game does for the event (FireReceivedActionStartNodes); sent
// again every minute because a load rebuilds the graph unseen and a second Start only rewrites the flag (Node[8]). Before the mission is done this does nothing. Fixed addresses, checked first as above.
static const int CONVOY_DIM_RIM = 0;
static const unsigned CONVOY_ACTIVE_ENABLED_START = 0x903D2DBAu;
static const unsigned MISSION_DIM_RIM = 0x946AF1C1u;   // Jenkins("mm1038"), what the dialogue scripts pass to logbook.IsMissionCompleted
static const float CONVOY_DIM_RIM_RESEND_S = 60.0f;

static uintptr_t A_LogBookInstance = 0x141719778;
static uintptr_t A_LogBookIsMissionCompleted = 0x1403024B0;

static bool LogBookMissionDone(unsigned mission) {
    static const uint8_t head[7] = { 0x48, 0x8B, 0x81, 0xB8, 0x04, 0x00, 0x00 };
    __try {
        if (memcmp((const void*)A_LogBookIsMissionCompleted, head, sizeof head)) return false;
        void* book = *(void**)A_LogBookInstance;
        return book && ((bool (*)(void*, const unsigned*))A_LogBookIsMissionCompleted)(book, &mission);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void ConvoyDimRimEnable(void* graphObj, float elapsed) {
    static void* lastProcessor = nullptr;
    static float sinceSent = CONVOY_DIM_RIM_RESEND_S;
    void* processor = *(void**)((uintptr_t)graphObj + 0xF0);
    sinceSent += elapsed;
    if (!processor || !ProcessorFireStart || (processor == lastProcessor && sinceSent < CONVOY_DIM_RIM_RESEND_S) || !LogBookMissionDone(MISSION_DIM_RIM)) return;
    sinceSent = 0.0f;
    bool found = ProcessorFireStart(processor, CONVOY_ACTIVE_ENABLED_START);
    if (processor != lastProcessor) LogLine("convoy %2d: mission mm1038 done, switched on (start %s)", CONVOY_DIM_RIM + 1, found ? "sent" : "NOT FOUND");
    lastProcessor = processor;
}

const float CONVOY_RESPAWN_DELAY_MIN_S = 180.0f;
const float CONVOY_RESPAWN_DELAY_MAX_S = 480.0f;
const float CONVOY_RESPAWN_MIN_PLAYER_DISTANCE = 2000.0f; // == the convoy's own spawn radius
const float CONVOY_RESPAWN_SWEEP_INTERVAL_S = 1.0f;
const int CONVOY_GRAPH_STATE_DONE = 4;

struct TrackedConvoy {
    bool wrecked;
    int graphState;
    bool armed;
    float delayRemaining;
    float lastDistance;   // -1 = unknown (zero wreck transform)
    int respawns;
    bool foundThisSweep;
    int stateZeroSweeps;  // consecutive sweeps seen at state 0 with no processor
    bool prevWrecked, prevArmed, prevFound;
    int prevGraphState;
    float wreckPos[3];    // CConvoyDataContainer::m_WreckedTransform translation, read every sweep (diagnostic)
};
static TrackedConvoy g_tracked[sizeof(g_convoyPairs) / sizeof(g_convoyPairs[0])] = {};
static float g_sweepAccum = 0.0f;
static int g_sweepCount = 0;
static int g_totalAutoRespawns = 0;

// CGameObject::FindOptional hands back a boost::shared_ptr; ResetAllKnownConvoys
// above just drops it (one leaked refcount per call, fine for a hotkey). The
// sweep runs 28 lookups a second, so it releases properly: boost's
// sp_counted_base is { vptr, long use_count_, long weak_count_ } (stable
// layout since 1.33), and the object is always still owned by the world, so
// decrementing use_count_ can never reach zero here.
static void* FindGameObjectByIdAndRelease(uint64_t id) {
    uint8_t sharedPtrBuf[16] = { 0 };
    if (!CGameObject_FindOptional(id, sharedPtrBuf)) return nullptr;
    void* px = *(void**)sharedPtrBuf;
    void* pn = *(void**)(sharedPtrBuf + 8);
    if (pn) InterlockedDecrement((volatile long*)((uintptr_t)pn + 8));
    return px;
}

// When the choreographer graph Exits, CGraphScriptGameObject::UpdatePostSim
// sets CGameObject::m_RemoveFromUpdate (this+0x8C bit 0, seen as
// `or byte ptr [rbx+0x8c],1` in its disassembly) and the object drops out of
// the engine's update queues -- so a bare m_State=0 write on a state-4
// object is never noticed (live-confirmed 2026-09-21: convoy stuck at
// "graph state 0, processor null" after the first auto-respawn). The fix is
// the game's own re-registration routine, CGameObject::AddToUpdate()
// (virtual, vtable slot 0x138 -- the slot its own child-recursion uses;
// base impl 0x14068E290, disassembled): clears the remove bit, asks
// GetRequiredUpdates()/GetParallelizedUpdates(), calls
// RegisterUpdatePreSim/PostSim/Render as needed, recurses into children.
static int g_addToUpdateCalls = 0;
static void GameObjectAddToUpdate(void* obj) {
    void** vtable = *(void***)obj;
    typedef void (*AddToUpdateFn)(void*);
    ((AddToUpdateFn)vtable[0x138 / 8])(obj);
    g_addToUpdateCalls++;
}

static float RandomDelaySeconds() {
    float t = (float)rand() / (float)RAND_MAX;
    return CONVOY_RESPAWN_DELAY_MIN_S + t * (CONVOY_RESPAWN_DELAY_MAX_S - CONVOY_RESPAWN_DELAY_MIN_S);
}

// One full pass over the 14 convoys. `force` (dev F9) ignores delay+distance.
static void ConvoyRespawnSweep(float elapsed, bool force) {
    g_sweepCount++;
    for (int i = 0; i < g_convoyPairCount; i++) {
        TrackedConvoy& t = g_tracked[i];
        t.foundThisSweep = false;

        void* container = FindGameObjectByIdAndRelease(g_convoyPairs[i].containerId);
        void* graphObj = FindGameObjectByIdAndRelease(g_convoyPairs[i].logicGraphId);
        if (!container || !graphObj) { t.armed = false; continue; }
        if (*(unsigned int*)((uintptr_t)graphObj + 0xF8) != CONVOY_CHOREOGRAPHER_PATH_HASH) { t.armed = false; continue; }
        t.foundThisSweep = true;

        uint8_t* flags = (uint8_t*)((uintptr_t)container + 0x118);
        if ((*flags & ~0x3) != 0) { t.armed = false; continue; } // not a sane EFlags byte, leave it alone
        t.wrecked = (*flags & 0x1) != 0;
        t.graphState = *(int*)((uintptr_t)graphObj + 0xE0);
        // diagnostic (2026-10-12): a "hood ornament" map icon sat at the world origin with nothing there --
        // log every convoy's stored wreck position so a zero one can be matched to it
        const float* wreckMat = (const float*)((uintptr_t)container + 0xD8);
        float wx = wreckMat[12], wy = wreckMat[13], wz = wreckMat[14];
        if (t.prevFound && (wx != t.wreckPos[0] || wy != t.wreckPos[1] || wz != t.wreckPos[2]))
            LogLine("convoy %2d: wreck pos %.1f %.1f %.1f -> %.1f %.1f %.1f", i + 1, t.wreckPos[0], t.wreckPos[1], t.wreckPos[2], wx, wy, wz);
        t.wreckPos[0] = wx; t.wreckPos[1] = wy; t.wreckPos[2] = wz;
        if (!t.prevFound) { LogLine("convoy %2d: found (wrecked=%d, graph state %d, wreck pos %.1f %.1f %.1f)", i + 1, t.wrecked, t.graphState, wx, wy, wz); t.prevFound = true; t.prevWrecked = t.wrecked; t.prevGraphState = t.graphState; }
        if (t.wrecked != t.prevWrecked) { LogLine("convoy %2d: %s", i + 1, t.wrecked ? "WRECKED (flag set)" : "wrecked flag cleared"); t.prevWrecked = t.wrecked; }
        if (t.graphState != t.prevGraphState) { LogLine("convoy %2d: graph state %d -> %d", i + 1, t.prevGraphState, t.graphState); t.prevGraphState = t.graphState; }

        if (!t.wrecked) {
            t.armed = false;
            if (i == CONVOY_DIM_RIM && t.graphState == 2) ConvoyDimRimEnable(graphObj, elapsed);
            // Recovery: a rebuild we kicked off that the engine never picked up
            // (state 0, no processor) -- re-register it so UpdatePostSim runs.
            // Freshly created objects also pass through state 0 for a frame
            // (and are registered normally), so only act after it has sat
            // there across two consecutive sweeps (>= 1 s).
            if (t.graphState == 0 && *(void**)((uintptr_t)graphObj + 0xF0) == nullptr) {
                if (++t.stateZeroSweeps >= 2) { GameObjectAddToUpdate(graphObj); t.stateZeroSweeps = 0; LogLine("convoy %2d: stuck at graph state 0 with no processor -> AddToUpdate()", i + 1); }
            } else {
                t.stateZeroSweeps = 0;
            }
            continue;
        }
        if (t.graphState != CONVOY_GRAPH_STATE_DONE) { t.armed = false; continue; } // vanilla wreck flow still running

        if (!t.armed) {
            t.armed = true;
            t.delayRemaining = RandomDelaySeconds();
            LogLine("convoy %2d: wreck flow finished (graph state 4) -> respawn armed, delay %.0fs", i + 1, t.delayRemaining);
        } else {
            t.delayRemaining -= elapsed;
        }

        bool haveWreckPos = (wx != 0.0f || wy != 0.0f || wz != 0.0f);
        if (haveWreckPos) {
            float dx = g_PlayerPos.x - wx, dy = g_PlayerPos.y - wy, dz = g_PlayerPos.z - wz;
            t.lastDistance = sqrtf(dx * dx + dy * dy + dz * dz);
        } else {
            t.lastDistance = -1.0f;
        }

        bool delayOk = force || t.delayRemaining <= 0.0f;
        bool distanceOk = force || !haveWreckPos || t.lastDistance > CONVOY_RESPAWN_MIN_PLAYER_DISTANCE;
        if (!delayOk || !distanceOk) continue;

        *flags &= ~0x1;                                   // clear EFlags_Wrecked
        *(int*)((uintptr_t)graphObj + 0xE0) = 0;           // engine rebuilds graph + processor next tick...
        GameObjectAddToUpdate(graphObj);                   // ...but only once it is back in the update queue
        t.armed = false;
        t.respawns++;
        g_totalAutoRespawns++;
        LogLine("convoy %2d: RESPAWN triggered%s (player %.0fm from wreck, total this session %d)", i + 1, force ? " by F9" : "", t.lastDistance, g_totalAutoRespawns);
        ConvoyRouteClearWrecked(i);
    }
}

static void LogConvoySnapshot(const char* why) {
    LogLine("--- snapshot (%s): sweeps %d, auto-respawns %d, player at %.0f %.0f %.0f", why, g_sweepCount, g_totalAutoRespawns, g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z);
    for (int i = 0; i < g_convoyPairCount; i++) {
        const TrackedConvoy& t = g_tracked[i];
        if (!t.foundThisSweep) { LogLine("    convoy %2d: not found", i + 1); continue; }
        if (!t.wrecked) LogLine("    convoy %2d: active (graph state %d) respawns %d, wreck pos %.1f %.1f %.1f", i + 1, t.graphState, t.respawns, t.wreckPos[0], t.wreckPos[1], t.wreckPos[2]);
        else if (!t.armed) LogLine("    convoy %2d: WRECKED, wreck flow running (graph state %d), wreck pos %.1f %.1f %.1f", i + 1, t.graphState, t.wreckPos[0], t.wreckPos[1], t.wreckPos[2]);
        else LogLine("    convoy %2d: WRECKED, respawn in %.0fs, player %.0fm from wreck, wreck pos %.1f %.1f %.1f", i + 1, t.delayRemaining, t.lastDistance, t.wreckPos[0], t.wreckPos[1], t.wreckPos[2]);
    }
}

static void ConvoyRespawnTick(float dt) {
    if (!enabledConvoyNeverWrecked) return;
    g_sweepAccum += dt;
    if (g_sweepAccum < CONVOY_RESPAWN_SWEEP_INTERVAL_S) return;
    float elapsed = g_sweepAccum;
    g_sweepAccum = 0.0f;
    ConvoyRespawnSweep(elapsed, false);
    static float sinceSnapshot = 0.0f;
    sinceSnapshot += elapsed;
    if (sinceSnapshot >= 60.0f) { sinceSnapshot = 0.0f; LogConvoySnapshot("periodic"); }
}

// Dev overlay (MM_DEV_TOOLS only). The release panel (build, convoy counts, F3/F4 help; F4 itself went too) was removed on 2026-11-19 (user: "remova o
// overlay"): a release build draws only the camp notices (camp_notice.inc); the log keeps the status.
#if MM_DEV_TOOLS || MM_WORLD_REPOP
static char g_snOverlay[192];   // nearest sniper tower line (written once a second by SniperTick in camp_repop.inc)
#endif
#if MM_DEV_TOOLS
class ConvoyModOverlay : public ImGuiRenderer {
	void Render() override {
		// Dev builds (2026-09-24): Enhanced Convoys and Wasteland Storms are
		// released, so their status lines are gone from our own overlay -- only
		// what is still being worked on stays.
		ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
		ImGui::Begin("Mad Max dev tools", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
		ImGui::Text("Dev build: %s", MM_BUILD_TAG);
		ImGui::Text("Pos: X=%.1f Y=%.1f Z=%.1f", g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z);
		ImGui::Text("Invincibility (F1): %s", enabledInvincibility ? "ON" : "off");
		ImGui::Text("Vehicle: %s", (g_lastCurrentVehicle && g_lastCurrentVehicle == g_lastSignatureVehicle) ? "Magnum Opus" :
			(g_lastCurrentVehicle ? "another car" : "on foot"));
		if (g_snOverlay[0]) { char l[sizeof g_snOverlay]; memcpy(l, g_snOverlay, sizeof l); l[sizeof l - 1] = 0; ImGui::Text("%s", l); }
		if (g_showDiagnostics) {
			ImGui::Separator();
			ImGui::Text("Current vehicle: %p  |  Magnum Opus: %p  |  chassis: %p", g_lastCurrentVehicle, g_lastSignatureVehicle, g_lastChassis);
			ImGui::Text("F6: set current vehicle mass to %.0f (pressed %d)", g_testVehicleMass, g_setMassPresses);
			ImGui::Text("F7: set current vehicle engine torque scale to %.1fx (pressed %d)", g_testTorqueScale, g_setTorquePresses);
		}
		ImGui::Text("F3: %s details", g_showDiagnostics ? "hide" : "show");
		ImGui::End();
	}
};
#endif
#if MM_DEV_TOOLS || MM_WORLD_REPOP
// ---- camp repopulation test (2026-10-13) --------------------------------------------------------------------------------------
// Liberating a camp sends one event that switches off the camp's enemy location (world.bin entry *_gameplay / *_spawn, disable
// list 83DEBF1B) and switches on its civilian location (enable list 43D36548); the save keeps both flags. This test does the
// reverse for one camp the way the event handler does it: SLocationInfo::HandleEvent (0x14066C430) only sets or clears bit0 of
// +0x67 (enable list at +0x70, disable list at +0x78) and the streaming update loads or unloads by distance. (First try, 2026-10-13:
// GameLocationSystemInfoEnableLoading/DisableLoading are only the streaming block in +0x66, not the enabled flag -- the civilians
// went away, the enemies never came back.) Only these two locations change:
// the camp's main location (scrap crates, insignias, collectibles) is never touched. Table generated by
// tools/repopulate_table.py (scrap and fuel depots whose enemy location has no collectible).
// SLocationInfo (one per world.bin entry and file type): id +0x18 (hash of the path), type +0x65 (4 .nl, 8 .fl, 0x10 .bl),
// flags +0x67 (bit0 enabled, bit2 loaded). Context 0x14179F0D8: vector of SLocationInfo* at +8..+0x10. GOG addresses (dev only).
struct CampEntry { unsigned enemyId, civilianId; float x, z; const char* name; };
static const CampEntry g_camps[] = {
    { 0x01212177, 0x6B1CBF6E, -9502.f, -745.f, "a02_fd2030_gameplay" },
    { 0x0DC15ED8, 0xAD1D544C, -2082.f, 4968.f, "a01_sd1030_gameplay" },
    { 0x2BDD7106, 0x90889273, -3103.f, 2314.f, "a01_fd1110_spawn" },
    { 0x30335AF6, 0xEFBDDFE7, -3564.f, 3116.f, "a01_fd1090_gameplay" },
    { 0x464CEAF8, 0x9186B2A7, -6990.f, 5873.f, "a01_sd1330_spawn" },
    { 0x5243642C, 0xBC2151E7, -9026.f, 3482.f, "a01_fd1050_spawn" },
    { 0x5525AEE9, 0xE867CAA6, -3934.f, 2074.f, "a01_sd1130_gameplay" },
    { 0x5C876D19, 0xBE817E88, -5630.f, 6115.f, "a01_sd1010_gameplay" },
    { 0x683BCC6A, 0x8F2F9582, -4486.f, 2881.f, "a01_fd1080_logic" },
    { 0x68532541, 0xA049E4A5, -8454.f, 5507.f, "a01_sd1080_gameplay" },
    { 0x6E78B7E6, 0x67B9BD7C, -3755.f, 4807.f, "a01_sd1190_gameplay" },
    { 0x70E37637, 0x26247AF0, -7220.f, 2192.f, "a01_sd1090_gameplay" },
    { 0x72A2D8E4, 0x96ABF98C, -3898.f, 6167.f, "a01_sd1290_spawn" },
    { 0x7AD28246, 0xDA0501D7, -4856.f, 336.f, "a02_sd2090_gameplay" },
    { 0x83F6A0F7, 0x0978DE1F, -5968.f, 667.f, "a02_sd2010_gameplay" },
    { 0x8E91C5F7, 0xF5441C8D, -7897.f, -2616.f, "a02_sd2030_gameplay" },
    { 0x8EAECF51, 0x79B173C4, -7915.f, 585.f, "a02_fd2040_gameplay" },
    { 0xAD6FDB7E, 0x5862E92E, -2887.f, 4011.f, "a01_sd1170_gameplay" },
    { 0xB4D17B50, 0xACE2E1DA, -1676.f, 4138.f, "a01_sd1300_gameplay" },
    { 0xB8788A79, 0x2CF6703D, -4514.f, 4169.f, "a01_sd1200_gameplay" },
    { 0xD70F6C49, 0x68F0AC34, -3471.f, 5693.f, "a01_sd1270_gameplay" },
    { 0xE46FA8A7, 0x5D82EC11, -8036.f, 2348.f, "a01_sd1240_gameplay" },
    { 0xE4BE075D, 0xBF77A52A, -5872.f, 3511.f, "a01_sd1210_spawn" },
    { 0xF8FBEA29, 0xBD19C67B, -8611.f, -407.f, "a02_sd2060_gameplay" },
    // r43 (user 2026-11-17: "pode incluir os 7 acampamentos"): the valuable_vehicle and food_depot camps, same layout and the same
    // completion graph as the ones above; tools/repopulate_table.py had read only scrap_depot / fuel_depot / warchief_camp. Their centre
    // is the middle of their character spawn points, not the location's origin (r43 autotests: Black Cove's origin is 124 m from its
    // camp, 7 points 150-176 m away never spawned; Overlook kept one at 123 m; Jugular Canyon's origin is under the camp, the player fell
    // through the world)
    { 0x213A5DA6, 0x910C6C96, -5420.f, 4244.f, "a01_vv1050_gameplay" },
    { 0x3B84A609, 0x7FEEFD62, -7639.f, -1083.f, "a02_fo2040_gameplay" },
    { 0x81CFB36E, 0x2CEAF401, -6887.f, 884.f, "a02_fo2020_gameplay" },
    { 0x9FBD2D25, 0xE8988FE0, -5660.f, -1263.f, "a02_vv2020_spawn" },
    { 0xB7A6D39A, 0x22AC50F1, -9285.f, 4827.f, "a01_vv1110_gameplay" },
    { 0xE630E231, 0x7EDE3DDB, -6857.f, 3541.f, "a01_vv1060_gameplay" },
    { 0xE762AD41, 0xD3DE5E6A, -6237.f, 4771.f, "a01_vv1080_gameplay" },
    // warchief camps (Top Dog strongholds, 2026-10-16 test): same layout, enemy part without collectibles (repopulate_table.py).
    // r47 (user 2026-11-19: "pode fazer o r47"): in the release too, the warchief back (camp_repop.inc g_warchiefGates). The Drop's
    // centre is the middle of its character spawn points like the r43 camps (the location's origin, -9987 -1834, is 136 m east of them;
    // the other three are within 25 m: tools/aibt/camp_points.py)
    { 0x0E77AB96, 0xF23C43C9, -8026.f, 4138.f, "a01_wc1050_gameplay" },
    { 0x27ECF4B1, 0xCBCAFC3A, -5384.f, -513.f, "a02_wc2040_spawn" },
    { 0xC72615C6, 0x2E5ADAAF, -4498.f, 5468.f, "a01_wc1020_spawn" },
    { 0xE1C95370, 0x2E7E4843, -10123.f, -1831.f, "a02_wc2020_gameplay" },
};
static uintptr_t A_LocContext = 0x14179F0D8;

static int FindLocations(unsigned id, uint8_t** out, int max) {
    int n = 0;
    __try {
        const uint8_t* ctx = *(const uint8_t* const*)A_LocContext;
        if (!ctx) return 0;
        uint8_t* const* b = *(uint8_t* const* const*)(ctx + 8);
        uint8_t* const* e = *(uint8_t* const* const*)(ctx + 0x10);
        if (!b || e < b || e - b > 20000) return 0;
        for (uint8_t* const* it = b; it != e && n < max; ++it)
            if (*it && *(const unsigned*)(*it + 0x18) == id) out[n++] = *it;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return n; }
    return n;
}

// enabled flag of a camp part: 1 if every file of it (.nl/.fl) is enabled, 0 if none, -1 not found, 2 mixed
static int LocEnabled(unsigned id, int* loaded) {
    uint8_t* l[4]; int n = FindLocations(id, l, 4), on = 0;
    *loaded = 0;
    for (int i = 0; i < n; i++) { if (l[i][0x67] & 1) on++; if (l[i][0x67] & 4) (*loaded)++; }
    return n == 0 ? -1 : on == 0 ? 0 : on == n ? 1 : 2;
}

static volatile LONG g_saveLoads;      // saves loaded this session (camp_repop.inc LocLoadStatus; the game reloads one when the player dies)

static float CampDistance(const CampEntry& c) {
    float dx = g_PlayerPos.x - c.x, dz = g_PlayerPos.z - c.z;
    return sqrtf(dx * dx + dz * dz);
}

static void CampLogAll(const char* why) {
    int liberated = 0;
    LogLine("CAMPS (%s): player at %.0f %.0f %.0f", why, g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z);
    for (const CampEntry& c : g_camps) {
        int el, cl; int e = LocEnabled(c.enemyId, &el), v = LocEnabled(c.civilianId, &cl);
        if (e == 0) liberated++;
        LogLine("    %-26s enemies %s (loaded %d)  civilians %s (loaded %d)  %.0f m", c.name,
            e == 1 ? "ON " : e == 0 ? "off" : e == 2 ? "mix" : "n/a", el, v == 1 ? "ON " : v == 0 ? "off" : v == 2 ? "mix" : "n/a", cl, CampDistance(c));
    }
    LogLine("CAMPS: %d of %d have their enemies switched off (liberated)", liberated, (int)(sizeof g_camps / sizeof g_camps[0]));
}

// Repair after the camprepop/camprepop2 tests (2026-10-13): in the first test all 24 table camps were liberated (enemies off,
// civilians on); camprepop2 left some with enemies on / civilians off, and the save kept it. This puts every table camp back
// the way the liberation event leaves it: enemy entries bit0 cleared, civilian entries bit0 set.
static void CampRestoreLiberated() {
    int fixedE = 0, fixedC = 0;
    for (const CampEntry& c : g_camps) {
        uint8_t* en[4]; uint8_t* cv[4];
        int ne = FindLocations(c.enemyId, en, 4), nc = FindLocations(c.civilianId, cv, 4);
        for (int i = 0; i < ne; i++) if (en[i][0x67] & 1) {
            int b = en[i][0x67]; en[i][0x67] &= ~1; fixedE++;
            LogLine("CAMP RESTORE: %s enemy entry type %d f67 %02X -> %02X", c.name, en[i][0x65], b, en[i][0x67]);
        }
        for (int i = 0; i < nc; i++) if (!(cv[i][0x67] & 1)) {
            int b = cv[i][0x67]; cv[i][0x67] |= 1; fixedC++;
            LogLine("CAMP RESTORE: %s civilian entry type %d f67 %02X -> %02X", c.name, cv[i][0x65], b, cv[i][0x67]);
        }
    }
    LogLine("CAMP RESTORE: %d enemy entries switched off, %d civilian entries switched on", fixedE, fixedC);
}

static int g_campsRepopulated = 0;
static void CampRepopulateNearest() {
    const CampEntry* best = nullptr; float bestD = 1e30f;
    for (const CampEntry& c : g_camps) {
        int el; if (LocEnabled(c.enemyId, &el) != 0) continue;           // only camps whose enemies are switched off
        float d = CampDistance(c);
        if (d < bestD) { bestD = d; best = &c; }
    }
    if (!best) { LogLine("CAMP REPOPULATE: no liberated camp in the table"); return; }
    uint8_t* en[4]; uint8_t* cv[4];
    int ne = FindLocations(best->enemyId, en, 4), nc = FindLocations(best->civilianId, cv, 4);
    LogLine("CAMP REPOPULATE: %s at %.0f m: enabling %d enemy location(s), disabling %d civilian location(s)", best->name, bestD, ne, nc);
    for (int i = 0; i < ne; i++) {
        int before = en[i][0x67];
        en[i][0x67] |= 1;                                                   // what an enable event does
        LogLine("    enemy    %p type %d f66 %02X f67 %02X -> %02X container %p", en[i], en[i][0x65], en[i][0x66], before, en[i][0x67],
            *(void**)(en[i] + 0x20));
    }
    for (int i = 0; i < nc; i++) {
        int before = cv[i][0x67];
        cv[i][0x67] &= ~1;                                                  // what a disable event does
        LogLine("    civilian %p type %d f66 %02X f67 %02X -> %02X container %p", cv[i], cv[i][0x65], cv[i][0x66], before, cv[i][0x67],
            *(void**)(cv[i] + 0x20));
    }
    g_campsRepopulated++;
}
#endif // MM_DEV_TOOLS || MM_WORLD_REPOP

#if MM_DEV_TOOLS || MM_AUTOTEST
// ---- unattended test mode (2026-10-13; camp commands 2026-11-10) ------------------------------------------------------------------------
// Active only when <game>\scripts\autotest.txt exists: one waypoint per line (up to 24), "x y z seconds [command]" or "@camp dx dz seconds
// [command]": a camp of g_camps by part of its name (@sd2090), its centre moved by dx dz, on the ground (terrain height + 2.5 m, then the first
// static surface below it, recomputed while the player is held there: the ground may not be streamed in at first). Once the player has been
// in the world for 20 s of game time, the player (or their car) is moved to each waypoint in turn and kept there for its seconds,
// invulnerable; the position is logged every 5 s; the command runs once, 8 s after arriving (camp_repop.inc AutotestCampCommand: "repop
// <camp>", "kill", "log"). After the last one the log says AUTOTEST done (the test driver then closes the game). Used to test location
// changes and camp repopulation without the user (e.g. camp -> far away -> camp again, to force a reload).
static void AutotestCampCommand(const char* cmd);
static void AutotestDie(CCharacter* ch);
struct AutoWp { float p[3], secs; bool ground; char cmd[40]; };
static int g_autotest = -1;                      // -1 unknown, 0 off, 1 waiting, 2 running, 3 done
static AutoWp g_autoWp[24]; static int g_autoN = 0, g_autoI = 0;
static float g_autoClock = 0.f, g_autoLog = 0.f; static bool g_autoCmdDone;
static LONG g_autoDiedAt = -1;                   // "die": g_saveLoads when the player was killed; nothing runs until the next load
typedef float (*TerrainHeightFn)(const void* terrain, const float* p);
static const TerrainHeightFn TerrainBilinearHeight = (TerrainHeightFn)0x14078BCF0;   // CTerrainMapBase::GetBilinearHeight(const CVector3f&)
typedef float (*GroundHeightFn)(const float* p);
static const GroundHeightFn GroundHeightBelow = (GroundHeightFn)0x14057FD40;       // CAirVehicle::GetGroundHeight_Expensive: a static-only ray from y + 1 down to the terrain, 200 at least
static const uintptr_t A_TerrainMap = 0x141711D10;                                // CAvaSingle<CTerrainMapBase>::Instance
static bool AutotestGround(float* p) {
    __try {
        const void* t = *(const void* const*)A_TerrainMap;
        if (!t) return false;
        float q[3] = { p[0], 0.f, p[2] };
        q[1] = TerrainBilinearHeight(t, q) + 2.5f;
        p[1] = GroundHeightBelow(q) + 0.3f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void AutotestTeleport(CCharacter* ch, CVehicle* veh, AutoWp& w) {
    if (w.ground) AutotestGround(w.p);
    CMatrix4f m;
    if (veh) { veh->SetVelocity(CVector3f()); veh->GetTransform(&m); }
    else { ch->ForceNeutralState(); ch->GetTransform(&m); }
    m.SetPosition(CVector3f(w.p[0], w.p[1], w.p[2]));
    if (veh) veh->SetTransform(&m); else ch->SetTransform(&m);
    LogLine("AUTOTEST waypoint %d/%d (%s): from %.0f %.0f %.0f to %.1f %.1f %.1f%s for %.0f s%s%s", g_autoI + 1, g_autoN, veh ? "car" : "on foot",
        g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z, w.p[0], w.p[1], w.p[2], w.ground ? " (ground)" : "", w.secs, w.cmd[0] ? ", then " : "", w.cmd);
    g_autoCmdDone = false;
}
// the first seconds after a teleport the player is put back on the waypoint every frame: the ground there may not be streamed in
// yet (autotest DRMENU1 2026-10-15: Max fell from a Death Run platform and died)
static void AutotestHold(CCharacter* ch, CVehicle* veh, AutoWp& w) {
    if (w.ground) AutotestGround(w.p);
    CMatrix4f m;
    if (veh) { veh->SetVelocity(CVector3f()); veh->GetTransform(&m); } else ch->GetTransform(&m);
    m.SetPosition(CVector3f(w.p[0], w.p[1], w.p[2]));
    if (veh) veh->SetTransform(&m); else ch->SetTransform(&m);
}
static void AutotestRead(FILE* f) {
    char line[128];
    while (g_autoN < 24 && fgets(line, sizeof line, f)) {
        AutoWp& w = g_autoWp[g_autoN];
        memset(&w, 0, sizeof w);
        int used = 0;
        if (line[0] == '@') {
            char camp[24]; float dx, dz;
            if (sscanf_s(line + 1, "%23s %f %f %f %n", camp, (unsigned)sizeof camp, &dx, &dz, &w.secs, &used) < 4) continue;
            const CampEntry* c = nullptr;
            for (const CampEntry& e : g_camps) if (strstr(e.name, camp)) c = &e;
            if (!c) { LogLine("AUTOTEST: no camp \"%s\", line skipped", camp); continue; }
            w.p[0] = c->x + dx; w.p[2] = c->z + dz; w.ground = true; used++;
        } else if (sscanf_s(line, "%f %f %f %f %n", &w.p[0], &w.p[1], &w.p[2], &w.secs, &used) < 4) continue;
        strncpy_s(w.cmd, line + used, _TRUNCATE);
        for (size_t n = strlen(w.cmd); n && (w.cmd[n - 1] == '\n' || w.cmd[n - 1] == '\r' || w.cmd[n - 1] == ' '); n--) w.cmd[n - 1] = 0;
        g_autoN++;
    }
}
static void AutotestTick(CCharacter* ch, float dt) {
    if (g_autotest == -1) {
        g_autotest = 0;
        char path[MAX_PATH]; GetModuleFileNameA(NULL, path, MAX_PATH);
        char* sl = strrchr(path, '\\'); if (sl) sl[1] = 0;
        strcat_s(path, "scripts\\autotest.txt");
        FILE* f = nullptr;
        if (fopen_s(&f, path, "r") == 0 && f) {
            AutotestRead(f);
            fclose(f);
            if (g_autoN) g_autotest = 1;
            LogLine("AUTOTEST armed: %d waypoint(s)", g_autoN);
        }
    }
    if (g_autotest <= 0 || g_autotest == 3 || !ch) return;
    if (g_autoDiedAt >= 0) {
        if (g_saveLoads == g_autoDiedAt) return;         // dying, death screen: no invulnerability, no waypoint
        LogLine("AUTOTEST save loaded after the death (%d loads), player at %.1f %.1f %.1f", (int)g_saveLoads, g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z);
        g_autoDiedAt = -1;
    }
    g_autoClock += dt;
    ch->m_Invulnerable = true;
    // god mode and E3 invulnerability too (CDamageable +0xE9 / +0xEA: SetHealth keeps 0.1 at least): a whole camp killed the player
    // through m_Invulnerable alone (autotest 2026-11-10, fd2030, game over 50 s in)
    ((uint8_t*)ch)[0xE9] = 1; ((uint8_t*)ch)[0xEA] = 1;
    CVehicle* veh = ch->GetVehiclePtr();
    if (veh) veh->m_Invulnerable = true;
    if (g_autotest == 1) {
        if (g_autoClock < 20.f) return;
        g_autotest = 2; g_autoI = 0; g_autoClock = 0.f; g_autoLog = 0.f;
        LogLine("AUTOTEST start: player at %.1f %.1f %.1f (%s)", g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z, veh ? "car" : "on foot");
        AutotestTeleport(ch, veh, g_autoWp[0]);
        return;
    }
    AutoWp& w = g_autoWp[g_autoI];
    if (g_autoClock < 8.f) AutotestHold(ch, veh, w);
    else if (!g_autoCmdDone) {
        g_autoCmdDone = true;
        if (!strcmp(w.cmd, "die")) { AutotestDie(ch); return; }
        if (w.cmd[0]) AutotestCampCommand(w.cmd);
    }
    g_autoLog += dt;
    if (g_autoLog >= 5.f) { g_autoLog = 0.f; LogLine("AUTOTEST wp %d +%.0fs player at %.1f %.1f %.1f", g_autoI + 1, g_autoClock, g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z); }
    if (g_autoClock >= w.secs) {
        if (++g_autoI >= g_autoN) { LogLine("AUTOTEST done"); g_autotest = 3; return; }
        g_autoClock = 0.f;
        AutotestTeleport(ch, veh, g_autoWp[g_autoI]);
    }
}
#endif

#if MM_DEV_TOOLS
static ConvoyModOverlay g_convoyModOverlay;
#endif
#if MM_DEV_TOOLS || MM_WORLD_REPOP
static bool g_worldRepopOn = false;     // set in PluginHooks: on the GOG build whose addresses camp_repop.inc uses, or where ResolveRepopSteam found them all
#include "camp_repop.inc"
#include "threat_diag.inc"
#endif
#if MM_DEV_TOOLS || MM_WORLD_REPOP
// World Repop on Steam (2026-11-24). Its game addresses are fixed GOG ones; on the Steam build (see
// g_steamPatterns) each one is found by signature (analysis/steam/repop_sigs.py): the GOG form first,
// then the Steam form. RS_FUNC: the address is the match + off. RS_REF: a rip-relative disp32 (or a
// call's rel32) sits at match + off, and its instruction ends `extra` bytes after it. Every address
// must be found exactly once, or none is changed and World Repop stays off.
// GameGuiSetConvoyRouteWrecked has no signature (a leaf function like its neighbours, only called
// through a pointer): on Steam a respawned convoy's map route keeps its wrecked mark.
enum { RS_FUNC, RS_REF };
struct RepopSig {
    const char* name; void* var; int kind; uintptr_t gog;
    const char* gogPattern; int gogOff; const char* steamPattern; int steamOff; int extra;
};
static uintptr_t A_LocSaveStatus = 0x1406393B0;   // SLocationInfo save status (hooked: saves keep repopulated camps liberated)
static const RepopSig g_repopSigs[] = {
    { "NEvent::CSendEvent::SendMsg", &A_SendEventMsg, RS_FUNC, 0x140007CA0, "48 81 EC 68 02 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 50 02 00 00 48 8D 54 24 50", 0, "48 81 EC 68 02 00 00 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 84 24 50 02 00 00 48 8D 54 24 50", 0, 0 },   // sub rsp, 0x268; mov rax, qword ptr [rip + 0x17030ca]; xor rax, rsp; mov qword ptr [rsp + 0x250], rax; lea rdx, [rsp + 0x50]
    { "NAIConstantsProfilesHelpers::ComputeRegionThreatRatioForPosition", &A_TdRatio, RS_FUNC, 0x14002D480, "48 83 EC 28 48 8D 54 24 40 E8 ?? ?? ?? ?? 84 ?? 74 ?? 8B 4C 24 40", 0, nullptr, 0, 0 },   // sub rsp, 0x28; lea rdx, [rsp + 0x40]; call 0x1404ca060; test al, al; je 0x14002d4b1; mov ecx, dword ptr [rsp + 0x40]
    { "AiGameObjectToEntityHandle", &AiGameObjectToEntityHandle, RS_FUNC, 0x140038E30, "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 40 48 89 74 24 48 48 8B F2 48 8B F9 ?? 85 ??", 0, "57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 40 48 89 74 24 48 48 89 D6 48 89 CF ?? 85 ??", 0, 0 },   // push rdi; sub rsp, 0x30; mov qword ptr [rsp + 0x20], 0xfffffffffffffffe; mov qword ptr [rsp + 0x40], rbx; mov qword ptr [rsp + 0x48], rsi; mov rsi, rdx; mov rdi
    { "CCharacter::SetTransform", &CharSetTransform, RS_FUNC, 0x1400CD5F0, "40 53 48 83 EC 20 48 8B D9 48 81 C1 B8 01 00 00 E8 ?? ?? ?? ??", 0, "53 48 83 EC 20 48 89 CB 48 81 C1 B8 01 00 00 E8 ?? ?? ?? ??", 0, 0 },   // push rbx; sub rsp, 0x20; mov rbx, rcx; add rcx, 0x1b8; call 0x1406229a0
    { "CCharacter body time imm32 (mov [rbx+4FC], 6.0f)", &A_CharacterBodyTime, RS_FUNC, 0x140149ECF, "48 C7 83 FC 04 00 00 00 00 C0 40 4C 89 A3 08 05 00 00 4C 89 A3 10 05 00 00", 7, nullptr, 7, 0 },   // mov qword ptr [rbx + 0x4fc], 0x40c00000; mov qword ptr [rbx + 0x508], r12; mov qword ptr [rbx + 0x510], r12
    { "CLocator::GetLocalizedString", &A_LocatorGetString, RS_FUNC, 0x1402117A0, "48 83 EC 28 E8 ?? ?? ?? ?? ?? 85 ?? 75 ?? 48 8D 05 ?? ?? ?? ?? 48 83 C4 28", 0, nullptr, 0, 0 },   // sub rsp, 0x28; call 0x1409c1090; test rax, rax; jne 0x1402117b5; lea rax, [rip + 0xf8a003]; add rsp, 0x28
    { "LocMainDone call return in ShowIcon", &A_LocMainDoneShowIconRet, RS_FUNC, 0x14022E25E, "E8 ?? ?? ?? ?? 84 ?? 74 ?? 48 8D 05 ?? ?? ?? ?? 49 8B CD C6 84 28 EC 53 71 01 3C", 5, "E8 ?? ?? ?? ?? 84 ?? 74 ?? 48 8D 05 ?? ?? ?? ?? 4C 89 E9 C6 84 28 EC 53 71 01 3C", 5, 0 },   // call 0x1403df0d0; test al, al; je 0x14022e28f; lea rax, [rip - 0x22e269]; mov rcx, r13; mov byte ptr [rax + rbp + 0x17153ec], 0x3c
    { "GameGuiMessageQueuePush", &A_GuiMessagePush, RS_FUNC, 0x14026ABE0, "48 89 74 24 10 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 0F B6 F2", 0, nullptr, 0, 0 },   // mov qword ptr [rsp + 0x10], rsi; push rdi; sub rsp, 0x20; mov rax, qword ptr [rip + 0x14a9b17]; movzx esi, dl
    { "CGraphScriptGameObject::HandleEvent", &A_GraphScriptHandleEvent, RS_FUNC, 0x1402B2DA0, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 99 B8 00 00 00", 0, nullptr, 0, 0 },   // mov qword ptr [rsp + 8], rbx; mov qword ptr [rsp + 0x10], rbp; mov qword ptr [rsp + 0x18], rsi; push rdi; sub rsp, 0x20; mov rbx, qword ptr [rcx + 0xb8]
    { "NGSONodes::GetRegionThreat", &A_TdGso, RS_FUNC, 0x1402D52D0, "40 55 57 48 83 EC 58 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 40", 0, "55 57 48 83 EC 58 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 40", 0, 0 },   // push rbp; push rdi; sub rsp, 0x58; mov rax, qword ptr [rip + 0x1435a9a]; xor rax, rsp; mov qword ptr [rsp + 0x40], rax
    { "CLogBook::IsMissionCompleted", &A_LogBookIsMissionCompleted, RS_FUNC, 0x1403024B0, "48 8B 81 B8 04 00 00 4C 8B 89 C0 04 00 00 49 3B C1 74 ?? 44 8B 02", 0, "48 8B 81 B8 04 00 00 4C 8B 89 C0 04 00 00 4C 39 C8 74 ?? 44 8B 02", 0, 0 },   // mov rax, qword ptr [rcx + 0x4b8]; mov r9, qword ptr [rcx + 0x4c0]; cmp rax, r9; je 0x1403024d6; mov r8d, dword ptr [rdx]
    { "CLootBag::Load", &A_LootBagLoad, RS_FUNC, 0x140310E70, "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? 48 8B D0 48 8B CB E8 ?? ?? ?? ?? ?? 85 ?? 74 ?? 8B 00", 0, "48 89 5C 24 08 57 48 83 EC 20 48 89 D3 48 89 CF E8 ?? ?? ?? ?? 48 89 C2 48 89 D9 E8 ?? ?? ?? ?? ?? 85 ?? 74 ?? 8B 00", 0, 0 },   // mov qword ptr [rsp + 8], rbx; push rdi; sub rsp, 0x20; mov rbx, rdx; mov rdi, rcx; call 0x140681740; mov rdx, rax; mov rcx, rbx; call 0x14074bcd0; test rax, rax
    { "CSpawnPoint::Enable", &A_SpawnPointEnable, RS_FUNC, 0x1403128D0, "84 ?? 74 ?? 80 B9 22 02 00 00 00 75 ?? 80 A1 23 02 00 00 FE", 0, nullptr, 0, 0 },   // test dl, dl; je 0x1403128fb; cmp byte ptr [rcx + 0x222], 0; jne 0x140312912; and byte ptr [rcx + 0x223], 0xfe
    { "CTargetGameObject::Save", &A_TargetSave, RS_FUNC, 0x140317470, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? BE 01 00 00 00 48 8B CB 48 8B D0 44 8B C6 E8 ?? ?? ?? ?? 48 8B 5C 24 30", 0, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 89 D3 48 89 CF E8 ?? ?? ?? ?? BE 01 00 00 00 48 89 D9 48 89 C2 41 89 F0 E8 ?? ?? ?? ?? 48 8B 5C 24 30", 0, 0 },   // mov qword ptr [rsp + 8], rbx; mov qword ptr [rsp + 0x10], rsi; push rdi; sub rsp, 0x20; mov rbx, rdx; mov rdi, rcx; call 0x140681740; mov esi, 1; mov rcx, rbx; 
    { "CTargetGameObject::EnableTarget", &A_TargetEnable, RS_FUNC, 0x140317670, "40 53 48 83 EC 20 80 B9 A2 01 00 00 00 48 8B D9 75 ?? 48 8B 01", 0, "53 48 83 EC 20 80 B9 A2 01 00 00 00 48 89 CB 75 ?? 48 8B 01", 0, 0 },   // push rbx; sub rsp, 0x20; cmp byte ptr [rcx + 0x1a2], 0; mov rbx, rcx; jne 0x140317692; mov rax, qword ptr [rcx]
    { "CSpawnPoint::Spawn", &A_SpawnPointSpawn, RS_FUNC, 0x14032C1A0, "48 89 74 24 18 57 48 81 EC A0 00 00 00 48 8B F9 48 8D 54 24 20", 0, "48 89 74 24 18 57 48 81 EC A0 00 00 00 48 89 CF 48 8D 54 24 20", 0, 0 },   // mov qword ptr [rsp + 0x18], rsi; push rdi; sub rsp, 0xa0; mov rdi, rcx; lea rdx, [rsp + 0x20]
    { "LocationInfoIsCampMainMissionCompleted", &A_LocMainDone, RS_REF, 0x1403DF0D0, "E8 ?? ?? ?? ?? 84 ?? 74 ?? C6 83 DA 01 00 00 00 48 8B 7C 24 30", 1, nullptr, 1, 0 },   // call 0x1403df0d0; test al, al; je 0x14047da9b; mov byte ptr [rbx + 0x1da], 0; mov rdi, qword ptr [rsp + 0x30]
    { "LocationInfoGetTrackedItemInfo", &A_LocTrackedItems, RS_FUNC, 0x1403E0A30, "0F B7 C1 4C 8D 0C 40 48 8B 05 ?? ?? ?? ?? 48 8B 08 49 C1 E1 05", 0, nullptr, 0, 0 },   // movzx eax, cx; lea r9, [rax + rax*2]; mov rax, qword ptr [rip + 0x133a5c2]; mov rcx, qword ptr [rax]; shl r9, 5
    { "CEventRouter::Load", &A_RouterLoad, RS_FUNC, 0x1403E18F0, "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 4C 8D 83 E8 00 00 00", 0, "48 89 5C 24 08 57 48 83 EC 20 48 89 D7 48 89 CB E8 ?? ?? ?? ?? 4C 8D 83 E8 00 00 00", 0, 0 },   // mov qword ptr [rsp + 8], rbx; push rdi; sub rsp, 0x20; mov rdi, rdx; mov rbx, rcx; call 0x140681740; lea r8, [rbx + 0xe8]
    { "CDestructibleObject disable parts", &A_DestructibleDisableParts, RS_FUNC, 0x140447670, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 F6 81 40 02 00 00 01", 0, nullptr, 0, 0 },   // mov qword ptr [rsp + 8], rbx; mov qword ptr [rsp + 0x10], rbp; mov qword ptr [rsp + 0x18], rsi; push rdi; sub rsp, 0x20; test byte ptr [rcx + 0x240], 1
    { "CDestructibleObject enable parts", &A_DestructibleEnableParts, RS_FUNC, 0x140484B00, "4C 8B DC 55 53 49 8D 6B C8 48 81 EC 28 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 F0 48 8B D9", 0, "49 89 E3 55 53 49 8D 6B C8 48 81 EC 28 01 00 00 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 45 F0 48 89 CB", 0, 0 },   // mov r11, rsp; push rbp; push rbx; lea rbp, [r11 - 0x38]; sub rsp, 0x128; mov rax, qword ptr [rip + 0x1286261]; xor rax, rsp; mov qword ptr [rbp - 0x10], rax; mo
    { "CDestructibleObject::Load", &A_DestructibleLoad, RS_FUNC, 0x140485100, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? 48 8B D0", 0, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 89 D3 48 89 CF E8 ?? ?? ?? ?? 48 89 C2", 0, 0 },   // mov qword ptr [rsp + 8], rbx; mov qword ptr [rsp + 0x10], rsi; push rdi; sub rsp, 0x20; mov rbx, rdx; mov rdi, rcx; call 0x140681740; mov rdx, rax
    { "RegionInfoGetThreatSummaryInRegion", &A_TdSummary, RS_FUNC, 0x140491800, "41 54 48 83 EC 30 4C 8B E2 48 C7 02 00 00 00 00 48 8B 15 ?? ?? ?? ??", 0, "41 54 48 83 EC 30 49 89 D4 48 C7 02 00 00 00 00 48 8B 15 ?? ?? ?? ??", 0, 0 },   // push r12; sub rsp, 0x30; mov r12, rdx; mov qword ptr [rdx], 0; mov rdx, qword ptr [rip + 0x1289ae9]
    { "RegionInfoGetThreatLevelSummaryInRegion", &A_TdLevelSummary, RS_FUNC, 0x1404ADFF0, "40 53 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 45 33 C0 48 8B DA 44 8B 88 78 0E 24 00", 0, "53 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 45 31 C0 48 89 D3 44 8B 88 78 0E 24 00", 0, 0 },   // push rbx; sub rsp, 0x20; mov rax, qword ptr [rip + 0x1263c93]; xor r8d, r8d; mov rbx, rdx; mov r9d, dword ptr [rax + 0x240e78]
    { "CGameModule::GetRegionThreat", &A_TdXvmRegion, RS_FUNC, 0x1404B92C0, "48 8B C4 55 48 8D 68 A1 48 81 EC 90 00 00 00 48 C7 45 E7 FE FF FF FF 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 60 20", 0, "48 89 E0 55 48 8D 68 A1 48 81 EC 90 00 00 00 48 C7 45 E7 FE FF FF FF 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 60 20", 0, 0 },   // mov rax, rsp; push rbp; lea rbp, [rax - 0x5f]; sub rsp, 0x90; mov qword ptr [rbp - 0x19], 0xfffffffffffffffe; mov qword ptr [rax + 8], rbx; mov qword ptr [rax +
    { "CGameModule::GetTerritoryThreat", &A_TdXvmTerritory, RS_FUNC, 0x1404CBAC0, "4C 8B DC 57 48 81 EC 90 00 00 00 48 C7 44 24 20 FE FF FF FF 49 89 5B 08 4C 8B CA 48 8B D9", 0, "49 89 E3 57 48 81 EC 90 00 00 00 48 C7 44 24 20 FE FF FF FF 49 89 5B 08 49 89 D1 48 89 CB", 0, 0 },   // mov r11, rsp; push rdi; sub rsp, 0x90; mov qword ptr [rsp + 0x20], 0xfffffffffffffffe; mov qword ptr [r11 + 8], rbx; mov r9, rdx; mov rbx, rcx
    { "SpawnSystemGetStatus", &A_SpawnSystemGetStatus, RS_REF, 0x140530420, "E8 ?? ?? ?? ?? 4C 8B 6C 24 20 4C 8B 64 24 58 48 8B 74 24 48", 1, nullptr, 1, 0 },   // call 0x140530420; mov r13, qword ptr [rsp + 0x20]; mov r12, qword ptr [rsp + 0x58]; mov rsi, qword ptr [rsp + 0x48]
    { "CCountTrigger::Load", &A_CountTriggerLoad, RS_FUNC, 0x1405704F0, "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? 48 8B D0 48 8B CB E8 ?? ?? ?? ?? ?? 85 ?? 74 ?? 83 38 00", 0, "48 89 5C 24 08 57 48 83 EC 20 48 89 D3 48 89 CF E8 ?? ?? ?? ?? 48 89 C2 48 89 D9 E8 ?? ?? ?? ?? ?? 85 ?? 74 ?? 83 38 00", 0, 0 },   // mov qword ptr [rsp + 8], rbx; push rdi; sub rsp, 0x20; mov rbx, rdx; mov rdi, rcx; call 0x140681740; mov rdx, rax; mov rcx, rbx; call 0x14074bcd0; test rax, rax
    { "CAirVehicle::GetGroundHeight_Expensive", &A_GroundBelow, RS_FUNC, 0x14057FD40, "48 8B C4 55 48 8D A8 28 FF FF FF 48 81 EC D0 01 00 00 48 C7 45 88 FE FF FF FF 48 89 58 10 48 89 70 18", 0, "48 89 E0 55 48 8D A8 28 FF FF FF 48 81 EC D0 01 00 00 48 C7 45 88 FE FF FF FF 48 89 58 10 48 89 70 18", 0, 0 },   // mov rax, rsp; push rbp; lea rbp, [rax - 0xd8]; sub rsp, 0x1d0; mov qword ptr [rbp - 0x78], 0xfffffffffffffffe; mov qword ptr [rax + 0x10], rbx; mov qword ptr [r
    { "GameLocationSystemInfoDisableLoading", &A_LocDisableLoading, RS_FUNC, 0x140638990, "F6 41 65 0E 74 ?? 80 61 66 0F 48 8B 41 28 80 49 66 20 ?? 85 ??", 0, nullptr, 0, 0 },   // test byte ptr [rcx + 0x65], 0xe; je 0x1406389d2; and byte ptr [rcx + 0x66], 0xf; mov rax, qword ptr [rcx + 0x28]; or byte ptr [rcx + 0x66], 0x20; test rax, rax
    { "SLocationInfo save status", &A_LocSaveStatus, RS_FUNC, 0x1406393B0, "48 89 5C 24 08 48 89 6C 24 20 56 57 41 54 41 55 41 56 48 83 EC 20", 0, nullptr, 0, 0 },   // mov qword ptr [rsp + 8], rbx; mov qword ptr [rsp + 0x20], rbp; push rsi; push rdi; push r12; push r13; push r14; sub rsp, 0x20
    { "SLocationInfo start load", &A_LocStartLoad, RS_FUNC, 0x1406676D0, "48 8B C4 57 48 83 EC 70 48 C7 44 24 30 FE FF FF FF 48 89 58 18", 0, "48 89 E0 57 48 83 EC 70 48 C7 44 24 30 FE FF FF FF 48 89 58 18", 0, 0 },   // mov rax, rsp; push rdi; sub rsp, 0x70; mov qword ptr [rsp + 0x30], 0xfffffffffffffffe; mov qword ptr [rax + 0x18], rbx
    { "SLocationInfo load status", &A_LocLoadStatus, RS_FUNC, 0x140667E70, "40 53 56 48 83 EC 28 8B 15 ?? ?? ?? ?? 48 8B 1D ?? ?? ?? ?? 48 8B F1", 0, "53 56 48 83 EC 28 8B 15 ?? ?? ?? ?? 48 8B 1D ?? ?? ?? ?? 48 89 CE", 0, 0 },   // push rbx; push rsi; sub rsp, 0x28; mov edx, dword ptr [rip + 0x11372bb]; mov rbx, qword ptr [rip + 0x1137254]; mov rsi, rcx
    { "GameLocationSystemInfoEnableLoading", &A_LocEnableLoading, RS_FUNC, 0x14066C5F0, "48 83 EC 28 48 8B 41 28 80 61 66 0F ?? 85 ?? 74 ?? 80 60 66 0F", 0, nullptr, 0, 0 },   // sub rsp, 0x28; mov rax, qword ptr [rcx + 0x28]; and byte ptr [rcx + 0x66], 0xf; test rax, rax; je 0x14066c605; and byte ptr [rax + 0x66], 0xf
    { "CTerrainMapBase::GetBilinearHeight", &A_TerrainHeight, RS_FUNC, 0x14078BCF0, "48 83 EC 78 F3 0F 10 2A F3 0F 10 5A 08 0F 29 74 24 60 4C 8B C1", 0, "48 83 EC 78 F3 0F 10 2A F3 0F 10 5A 08 0F 29 74 24 60 49 89 C8", 0, 0 },   // sub rsp, 0x78; movss xmm5, dword ptr [rdx]; movss xmm3, dword ptr [rdx + 8]; movaps xmmword ptr [rsp + 0x60], xmm6; mov r8, rcx
    { "CTextUtil::GetLocalizedText", &A_GuiLocalizedText, RS_FUNC, 0x1409192D0, "40 53 56 57 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 40 48 8B DA", 0, "53 56 57 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 40 48 89 D3", 0, 0 },   // push rbx; push rsi; push rdi; sub rsp, 0x50; mov rax, qword ptr [rip + 0xdf1a99]; xor rax, rsp; mov qword ptr [rsp + 0x40], rax; mov rbx, rdx
    { "CLocator::GetLocalizedStringRaw", &A_LocatorGetStringRaw, RS_FUNC, 0x1409C1090, "40 53 48 83 EC 30 4C 8B 41 28 8B DA 49 8B 40 08 80 78 49 00", 0, "53 48 83 EC 30 4C 8B 41 28 89 D3 49 8B 40 08 80 78 49 00", 0, 0 },   // push rbx; sub rsp, 0x30; mov r8, qword ptr [rcx + 0x28]; mov ebx, edx; mov rax, qword ptr [r8 + 8]; cmp byte ptr [rax + 0x49], 0
    { "NEvent::CEventImpl::Send", &A_EventSend, RS_FUNC, 0x1409C25D0, "48 89 6C 24 18 57 48 83 EC 20 80 79 12 00 48 8B EA 48 8B F9", 0, "48 89 6C 24 18 57 48 83 EC 20 80 79 12 00 48 89 D5 48 89 CF", 0, 0 },   // mov qword ptr [rsp + 0x18], rbp; push rdi; sub rsp, 0x20; cmp byte ptr [rcx + 0x12], 0; mov rbp, rdx; mov rdi, rcx
    { "CEventSystem::SendEvent", &A_EventSystemSendEvent, RS_FUNC, 0x1409C3150, "?? 85 ?? 0F 8E ?? ?? ?? ?? 48 89 5C 24 10 48 89 6C 24 18 56", 0, nullptr, 0, 0 },   // test r8d, r8d; jle 0x1409c321d; mov qword ptr [rsp + 0x10], rbx; mov qword ptr [rsp + 0x18], rbp; push rsi
    { "NGraphScript::CGraph::GetInternalDataWritable", &A_GsGetDataWritable, RS_REF, 0x140F72140, "E8 ?? ?? ?? ?? C7 00 00 00 00 00 41 B8 AD DC BB 95 48 8B D6", 1, "E8 ?? ?? ?? ?? C7 00 00 00 00 00 41 B8 AD DC BB 95 48 89 F2", 1, 0 },   // call 0x140f72140; mov dword ptr [rax], 0; mov r8d, 0x95bbdcad; mov rdx, rsi
    { "NGraphScript::CGraph::IsTypeMatch<float>", &A_GsIsTypeFloat, RS_REF, 0x140F72270, "E8 ?? ?? ?? ?? 48 8B CF 84 ?? 74 ?? 4D 8B 27 F3 0F 11 75 BB", 1, "E8 ?? ?? ?? ?? 48 89 F9 84 ?? 74 ?? 4D 8B 27 F3 0F 11 75 BB", 1, 0 },   // call 0x140f72270; mov rcx, rdi; test al, al; je 0x140250976; mov r12, qword ptr [r15]; movss dword ptr [rbp - 0x45], xmm6
    { "NGraphScript::CGraph::ConvertSet<float>", &A_GsConvertSetFloat, RS_FUNC, 0x140F72A30, "48 83 EC 28 8B 42 04 4C 8B C9 3B 05 ?? ?? ?? ?? 75 ?? E8 ?? ?? ?? ??", 0, "48 83 EC 28 8B 42 04 49 89 C9 3B 05 ?? ?? ?? ?? 75 ?? E8 ?? ?? ?? ??", 0, 0 },   // sub rsp, 0x28; mov eax, dword ptr [rdx + 4]; mov r9, rcx; cmp eax, dword ptr [rip + 0x9adc44]; jne 0x140f72a4e; call 0x140281860
    { "NGraphScript::CProcessor::GetGraph", &A_GsGetGraph, RS_REF, 0x140F761E0, "E8 ?? ?? ?? ?? 48 8B F0 41 B8 5C 9B DD 75 49 8B D6 49 8B CC", 1, "E8 ?? ?? ?? ?? 48 89 C6 41 B8 5C 9B DD 75 4C 89 F2 4C 89 E1", 1, 0 },   // call 0x140f761e0; mov rsi, rax; mov r8d, 0x75dd9b5c; mov rdx, r14; mov rcx, r12
    { "NGraphScript::CProcessor::GetFirstVariableData", &A_GsGetFirstVariableData, RS_FUNC, 0x140F77060, "40 57 41 54 41 55 41 56 41 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 60 48 89 6C 24 68 48 89 74 24 70 45 8B F0", 0, "57 41 54 41 55 41 56 41 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 60 48 89 6C 24 68 48 89 74 24 70 45 89 C6", 0, 0 },   // push rdi; push r12; push r13; push r14; push r15; sub rsp, 0x30; mov qword ptr [rsp + 0x20], 0xfffffffffffffffe; mov qword ptr [rsp + 0x60], rbx; mov qword ptr 
    { "CCharacter CGameObject vftable", &VT_CHARACTER_GO, RS_REF, 0x1411BDBD8, "48 8D 0D ?? ?? ?? ?? 48 89 4B 08 48 8D 05 ?? ?? ?? ?? 48 89 83 D8 00 00 00", 3, nullptr, 3, 0 },   // lea rcx, [rip + 0x1073e83]; mov qword ptr [rbx + 8], rcx; lea rax, [rip + 0x1073e58]; mov qword ptr [rbx + 0xd8], rax
    { "CSpawnPoint CGameObject vftable", &VT_SPAWNPOINT_GO, RS_REF, 0x1411F06B8, "48 8D 05 ?? ?? ?? ?? 48 89 41 10 48 81 C1 98 01 00 00 E8 ?? ?? ?? ??", 3, nullptr, 3, 0 },   // lea rax, [rip + 0xea43ac]; mov qword ptr [rcx + 0x10], rax; add rcx, 0x198; call 0x1402e2320
    { "CAvaSingle<CEventSystem>::Instance", &A_EventSystemInstance, RS_REF, 0x1417116D8, "48 8B 0D ?? ?? ?? ?? 41 8D 54 24 40 4C 89 6D D7 4C 89 65 DF", 3, nullptr, 3, 0 },   // mov rcx, qword ptr [rip + 0x15553e5]; lea edx, [r12 + 0x40]; mov qword ptr [rbp - 0x29], r13; mov qword ptr [rbp - 0x21], r12
    { "CAvaSingle<CTerrainMapBase>::Instance", &A_TerrainInstance, RS_REF, 0x141711D10, "48 8B 0D ?? ?? ?? ?? 48 8D 54 24 30 0F 29 B4 24 B0 00 00 00", 3, nullptr, 3, 0 },   // mov rcx, qword ptr [rip + 0x12cbd06]; lea rdx, [rsp + 0x30]; movaps xmmword ptr [rsp + 0xb0], xmm6
    { "CLocator instance", &A_Locator, RS_REF, 0x1417127E0, "48 39 1D ?? ?? ?? ?? 74 ?? 4C 8D 4C 24 30 49 8B D5 48 8B CD", 3, "48 39 1D ?? ?? ?? ?? 74 ?? 4C 8D 4C 24 30 4C 89 EA 48 89 E9", 3, 0 },   // cmp qword ptr [rip + 0xdbc316], rbx; je 0x140956508; lea r9, [rsp + 0x30]; mov rdx, r13; mov rcx, rbp
    { "CLogBook instance", &A_LogBookInstance, RS_REF, 0x141719778, "48 8B 05 ?? ?? ?? ?? 8B 88 1C 06 00 00 41 89 8C 24 D8 1A 00 00", 3, nullptr, 3, 0 },   // mov rax, qword ptr [rip + 0x14a63dd]; mov ecx, dword ptr [rax + 0x61c]; mov dword ptr [r12 + 0x1ad8], ecx
    { "SLocationInfoTable", &A_LocationInfoTable, RS_REF, 0x14171B000, "48 8B 05 ?? ?? ?? ?? 33 DB 4C 8B E2 4C 8B E9 8B FB 39 58 08", 3, "48 8B 05 ?? ?? ?? ?? 31 DB 49 89 D4 49 89 CD 89 DF 39 58 08", 3, 0 },   // mov rax, qword ptr [rip + 0x133a3e1]; xor ebx, ebx; mov r12, rdx; mov r13, rcx; mov edi, ebx; cmp dword ptr [rax + 8], ebx
    { "s_SpawnSystemContext", &A_SpawnHandlerPtr, RS_REF, 0x14171B678, "48 8B 0D ?? ?? ?? ?? 45 33 C0 4C 8B 91 68 01 00 00 48 8B DA", 3, "48 8B 0D ?? ?? ?? ?? 45 31 C0 4C 8B 91 68 01 00 00 48 89 D3", 3, 0 },   // mov rcx, qword ptr [rip + 0x11eb129]; xor r8d, r8d; mov r10, qword ptr [rcx + 0x168]; mov rbx, rdx
    { "location context", &A_LocContext, RS_REF, 0x14179F0D8, "4C 8B 05 ?? ?? ?? ?? 48 8B B4 24 08 01 00 00 4C 8B 64 24 40", 3, nullptr, 3, 0 },   // mov r8, qword ptr [rip + 0x1132689]; mov rsi, qword ptr [rsp + 0x108]; mov r12, qword ptr [rsp + 0x40]
};
static const int REPOP_SIG_COUNT = (int)(sizeof g_repopSigs / sizeof g_repopSigs[0]);
static const uintptr_t SDK_STEAM_SPAWN_CONTEXT = 0x1417FEE78;   // the mm SDK's Steam address of s_SpawnSystemContext (spawnsystem.cpp)


// Second pass (steamtest7). steamtest6 found 46 of the 53 on Steam; the Steam exe (Oct 2015) is an older build
// than GOG's (Dec 2015), so some functions are not encoded the way the Steam forms predict. Its code keeps GOG's
// function order (GOG->Steam delta of the 46: +0x1B6CC90 at 0x140007CA0 growing to +0x2094870 at 0x140F77060),
// so each missing address lies between its found neighbours, and is found there by an anchor that does not
// depend on how its own bytes were encoded: an RTTI vtable slot, a call to a found function, a found global it
// reads, a constant next to its call. analysis/steam/repop_repair_check.py checks every finder on the GOG exe.
static const uintptr_t GOG_TEXT_END = 0x141195000;

static bool InScanRegion(uintptr_t a) {
    for (int r = 0; r < g_scanRegions; r++) if (a >= g_scanStart[r] && a < g_scanEnd[r]) return true;
    return false;
}

static int ParseRepairPattern(const char* pattern, unsigned char* bytes, bool* wild, int cap) {
    int len = 0;
    for (const char* c = pattern; *c && len < cap; ) {
        while (*c == ' ') c++;
        if (!*c) break;
        if (c[0] == '?') { wild[len] = true; bytes[len] = 0; len++; c += 2; continue; }
        unsigned int v = 0; sscanf_s(c, "%2x", &v); bytes[len] = (unsigned char)v; wild[len] = false; len++; c += 2;
    }
    return len;
}

// Matches of a pattern in [lo, hi): the count; the first `max` are kept.
static int FindInRange(uintptr_t lo, uintptr_t hi, const char* pattern, uintptr_t* out, int max) {
    unsigned char bytes[64]; bool wild[64];
    int len = ParseRepairPattern(pattern, bytes, wild, 64), n = 0;
    if (!len || hi <= lo + len || !RangeReadable(lo, hi - lo)) return 0;
    __try {
        for (const unsigned char* p = (const unsigned char*)lo; p + len <= (const unsigned char*)hi; p++) {
            if (p[0] != bytes[0] && !wild[0]) continue;
            int i = 1;
            for (; i < len; i++) if (!wild[i] && p[i] != bytes[i]) break;
            if (i == len) { if (n < max) out[n] = (uintptr_t)p; n++; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return n;
}

static int FindInScanRegions(const char* pattern, uintptr_t* out, int max) {
    int n = 0;
    for (int r = 0; r < g_scanRegions; r++) {
        uintptr_t tmp[16];
        int k = FindInRange(g_scanStart[r], g_scanEnd[r], pattern, tmp, 16);
        for (int j = 0; j < k && j < 16; j++) if (n + j < max) out[n + j] = tmp[j];
        n += k;
    }
    return n;
}

// E8 rel32 calls to target in the code regions.
static int FindCallsTo(uintptr_t target, uintptr_t* out, int max) {
    int n = 0;
    for (int r = 0; r < g_scanRegions; r++) {
        __try {
            for (const unsigned char* p = (const unsigned char*)g_scanStart[r]; p + 5 <= (const unsigned char*)g_scanEnd[r]; p++)
                if (p[0] == 0xE8 && (uintptr_t)p + 5 + *(const int32_t*)(p + 1) == target) { if (n < max) out[n] = (uintptr_t)p; n++; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return n;
}

// Raw bytes in the exe's other (data) sections, at `align`-aligned positions.
static int FindInData(const void* needle, size_t len, size_t align, uintptr_t* out, int max) {
    auto base = (uintptr_t)GetModuleHandleA(nullptr);
    auto nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int n = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uintptr_t lo = base + sec[i].VirtualAddress, hi = lo + sec[i].Misc.VirtualSize;
        if (InScanRegion(lo) || hi - lo < len || !RangeReadable(lo, hi - lo)) continue;
        __try {
            for (uintptr_t p = lo; p + len <= hi; p += align)
                if (memcmp((const void*)p, needle, len) == 0) { if (n < max) out[n] = p; n++; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return n;
}

static bool ReadU32(uintptr_t p, uint32_t& v) { __try { v = *(const uint32_t*)p; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static bool ReadU64(uintptr_t p, uint64_t& v) { __try { v = *(const uint64_t*)p; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static bool ReadBytes(uintptr_t p, void* dst, size_t n) { __try { memcpy(dst, (const void*)p, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }

// The vtable of an RTTI class (".?AVCCountTrigger@@"): type descriptor (name at +16) <- complete object locator
// {signature 1, offset 0, cdOffset, type descriptor RVA, hierarchy RVA, own RVA} <- the qword before the vtable.
static uintptr_t VtableOf(const char* rttiName) {
    auto base = (uintptr_t)GetModuleHandleA(nullptr);
    uintptr_t names[2];
    if (FindInData(rttiName, strlen(rttiName) + 1, 8, names, 2) != 1) return 0;
    uint32_t td = (uint32_t)(names[0] - 16 - base);
    uintptr_t refs[64];
    int nr = FindInData(&td, 4, 4, refs, 64), nvt = 0;
    uintptr_t vt = 0;
    for (int k = 0; k < nr && k < 64; k++) {
        uintptr_t col = refs[k] - 12;
        uint32_t sig = 0, off = 0, self = 0;
        if (!ReadU32(col, sig) || !ReadU32(col + 4, off) || !ReadU32(col + 20, self)) continue;
        if (sig != 1 || off != 0 || self != (uint32_t)(col - base)) continue;
        uint64_t colVa = col;
        uintptr_t slots[4];
        int ns = FindInData(&colVa, 8, 8, slots, 4);
        for (int j = 0; j < ns && j < 4; j++) { vt = slots[j] + 8; nvt++; }
    }
    return nvt == 1 ? vt : 0;
}

static uintptr_t VtableSlot(const char* rttiName, int slot) {
    uintptr_t vt = VtableOf(rttiName);
    uint64_t v = 0;
    return vt && ReadU64(vt + 8 * slot, v) ? (uintptr_t)v : 0;
}

static uintptr_t RipTarget(uintptr_t insn, int dispAt, int insnLen) {
    uint32_t d = 0;
    return ReadU32(insn + dispAt, d) ? insn + insnLen + (int32_t)d : 0;
}

static const char* HexAt(uintptr_t p, int n) {
    static char text[3 * 96 + 1];
    unsigned char b[96];
    if (n > 96) n = 96;
    if (!ReadBytes(p, b, n)) return "(unreadable)";
    for (int i = 0; i < n; i++) snprintf(text + 3 * i, 4, "%02X ", b[i]);
    text[n ? 3 * n - 1 : 0] = 0;
    return text;
}

static uintptr_t FoundOf(const uintptr_t* found, const void* var) {
    for (int j = 0; j < REPOP_SIG_COUNT; j++) if (g_repopSigs[j].var == var) return found[j];
    return 0;
}

// The found neighbours (in GOG address order) of entry i, and a straight-line estimate between them.
static bool RepopBracket(int i, const uintptr_t* found, uintptr_t& lo, uintptr_t& hi, uintptr_t& estimate) {
    uintptr_t g = g_repopSigs[i].gog, gl = 0, gh = ~(uintptr_t)0;
    lo = hi = estimate = 0;
    for (int j = 0; j < REPOP_SIG_COUNT; j++) {
        uintptr_t gj = g_repopSigs[j].gog;
        if (j == i || !found[j] || gj >= GOG_TEXT_END || !InScanRegion(found[j])) continue;
        if (gj < g && gj > gl) { gl = gj; lo = found[j]; }
        if (gj > g && gj < gh) { gh = gj; hi = found[j]; }
    }
    if (!lo || !hi || lo >= hi) return false;
    estimate = lo + (uintptr_t)((double)(hi - lo) * (double)(g - gl) / (double)(gh - gl));
    return true;
}

static uintptr_t RepopRepair(int i, const uintptr_t* found, uintptr_t lo, uintptr_t hi, const char*& how) {
    const void* var = g_repopSigs[i].var;
    uintptr_t c[16], got = 0;
    int n = 0, k;
    if (var == &A_LocMainDoneShowIconRet) {
        how = "call to LocationInfoIsCampMainMissionCompleted, then test al,al / je / lea rax,[rip] / mov byte [..], 3Ch";
        uintptr_t done = FoundOf(found, &A_LocMainDone);
        int nc = done ? FindCallsTo(done, c, 16) : 0;
        for (k = 0; k < nc && k < 16; k++) {
            unsigned char a[32];
            if (!ReadBytes(c[k] + 5, a, sizeof a) || a[0] != 0x84 || a[1] != 0xC0 || a[2] != 0x74) continue;
            bool lea = false, mov3c = false;
            for (int j = 0; j + 3 <= 12; j++) if (a[j] == 0x48 && a[j + 1] == 0x8D && a[j + 2] == 0x05) lea = true;
            for (int j = 0; j + 8 <= 24; j++) if (a[j] == 0xC6 && a[j + 1] == 0x84 && a[j + 7] == 0x3C) mov3c = true;
            if (lea && mov3c) { got = c[k] + 5; n++; }
        }
    } else if (var == &A_TargetSave) {
        how = "RTTI vtable CTargetGameObject slot 41 (= CHarpoonTarget slot 41)";
        got = VtableSlot(".?AVCTargetGameObject@@", 41);
        n = got && got == VtableSlot(".?AVCHarpoonTarget@@", 41) ? 1 : 0;
    } else if (var == &A_CountTriggerLoad) {
        how = "RTTI vtable CCountTrigger slot 42 (CLootBag and CEventRouter slot 42 = their found Load)";
        got = VtableSlot(".?AVCCountTrigger@@", 42);
        n = got && VtableSlot(".?AVCLootBag@@", 42) == FoundOf(found, &A_LootBagLoad)
                && VtableSlot(".?AVCEventRouter@@", 42) == FoundOf(found, &A_RouterLoad) ? 1 : 0;
    } else if (var == &A_LocTrackedItems) {
        how = "movzx eax,cx at a function start, mov rax,[SLocationInfoTable], then mov qword [r8], 0Bh / ret";
        uintptr_t table = FoundOf(found, &A_LocationInfoTable);
        int nc = table ? FindInRange(lo, hi, "0F B7 C1", c, 16) : 0;
        for (k = 0; k < nc && k < 16; k++) {
            unsigned char a[49];
            if (!ReadBytes(c[k] - 1, a, sizeof a) || (a[0] != 0xCC && a[0] != 0xC3)) continue;
            const unsigned char* f = a + 1;
            for (int j = 3; j <= 9; j++) {
                if (f[j] != 0x48 || f[j + 1] != 0x8B || f[j + 2] != 0x05 || RipTarget(c[k] + j, 3, 7) != table) continue;
                static const unsigned char tail[] = { 0xC7, 0x00, 0x0B, 0x00, 0x00, 0x00, 0xC3 };
                for (int t = j; t + (int)sizeof tail <= 48; t++) if (memcmp(f + t, tail, sizeof tail) == 0) { got = c[k]; n++; break; }
                break;
            }
        }
    } else if (var == &A_LocStartLoad) {
        how = "its signature, between its found neighbours";
        n = FindInRange(lo, hi, g_repopSigs[i].steamPattern, c, 16);
        if (!n) n = FindInRange(lo, hi, g_repopSigs[i].gogPattern, c, 16);
        if (n == 1) got = c[0];
    } else if (var == &A_GsConvertSetFloat) {
        how = "sub rsp,28h / mov eax,[rdx+4], then cmp eax,[float type id] as read by IsTypeMatch<float>";
        uintptr_t istm = FoundOf(found, &A_GsIsTypeFloat), gl = 0;
        unsigned char b[2];
        if (istm && ReadBytes(istm, b, 2) && b[0] == 0x8B && (b[1] & 0xC7) == 0x05) gl = RipTarget(istm, 2, 6);
        int nc = gl ? FindInRange(lo, hi, "48 83 EC 28 8B 42 04", c, 16) : 0;
        for (k = 0; k < nc && k < 16; k++)
            for (int j = 7; j <= 13; j++) {
                unsigned char op[2];
                if (ReadBytes(c[k] + j, op, 2) && op[0] == 0x3B && op[1] == 0x05 && RipTarget(c[k] + j, 2, 6) == gl) { got = c[k]; n++; break; }
            }
    } else if (var == &A_GsGetGraph) {
        how = "the call right before mov r8d, 75DD9B5Ch, landing on mov rax,[rcx+10h] / ret";
        int nc = FindInScanRegions("41 B8 5C 9B DD 75", c, 16);
        for (k = 0; k < nc && k < 16; k++)
            for (int back = 5; back <= 11; back++) {
                unsigned char op = 0;
                if (!ReadBytes(c[k] - back, &op, 1) || op != 0xE8) continue;
                uintptr_t t = RipTarget(c[k] - back, 1, 5);
                unsigned char body[5], want[5] = { 0x48, 0x8B, 0x41, 0x10, 0xC3 };
                if (t && ReadBytes(t, body, 5) && memcmp(body, want, 5) == 0 && t != got) { got = t; n++; }
                break;
            }
        if (!n) {   // Steam (dump 2026-11-26): no such call; the getter is one body shared by every identical getter
            how = "the one mov rax,[rcx+10h] / ret between its found neighbours";
            n = FindInRange(lo, hi, "48 8B 41 10 C3", c, 16);
            if (n == 1) got = c[0];
        }
    } else {
        how = "no second-pass finder";
        return 0;
    }
    if (n != 1 || got < lo || got >= hi) {
        if (n > 1) how = "more than one candidate";
        return 0;
    }
    return got;
}

static bool ResolveRepopSteam() {
    auto base = (uintptr_t)GetModuleHandleA(nullptr);
    auto nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
    static uintptr_t found[64];
    static_assert(sizeof g_repopSigs / sizeof g_repopSigs[0] <= 64, "found[] too small");
    int ok = 0;
    for (int i = 0; i < REPOP_SIG_COUNT; i++) {
        const RepopSig& r = g_repopSigs[i];
        GameSignature sig = { r.name, r.gogPattern, true, 0, 0, false };
        bool steam = false;
        bool hit = ScanPatternText(sig, r.gogPattern);
        if (!hit && !sig.matches && r.steamPattern) { steam = true; hit = ScanPatternText(sig, r.steamPattern); }
        int off = steam ? r.steamOff : r.gogOff;
        if (hit && sig.alreadyHooked && (r.kind != RS_FUNC || off)) hit = false;   // the E9 fallback only means something at a function start
        uintptr_t v = 0;
        if (hit) {
            v = sig.resolved + off;
            if (r.kind == RS_REF) {
                __try { v += 4 + r.extra + *(const int32_t*)v; } __except (EXCEPTION_EXECUTE_HANDLER) { hit = false; }
            }
            if (v < base || v >= end) hit = false;
        }
        found[i] = hit ? v : 0;
        if (hit) ok++;
        LogLine("  repop %-66s %s %p in %s (matches %d)%s%s", r.name, hit ? "OK" : "--", (void*)found[i], hit ? SectionNameOf(v) : "-",
            sig.matches, steam ? " (Steam form)" : "", sig.alreadyHooked ? " (already hooked)" : "");
        if (r.var == (void*)&A_SpawnHandlerPtr && hit)
            LogLine("  repop s_SpawnSystemContext %s the mm SDK's Steam address %p", v == SDK_STEAM_SPAWN_CONTEXT ? "matches" : "DIFFERS FROM", (void*)SDK_STEAM_SPAWN_CONTEXT);
    }
    for (int i = 0; i < REPOP_SIG_COUNT && ok < REPOP_SIG_COUNT; i++) {   // second pass, see RepopRepair
        if (found[i]) continue;
        const RepopSig& r = g_repopSigs[i];
        uintptr_t lo, hi, estimate;
        if (!RepopBracket(i, found, lo, hi, estimate)) { LogLine("  repop %-66s -- second pass: no found neighbours", r.name); continue; }
        const char* how = "";
        uintptr_t v = RepopRepair(i, found, lo, hi, how);
        if (v) {
            found[i] = v; ok++;
            LogLine("  repop %-66s OK %p in %s (second pass: %s; between %p and %p) bytes %s", r.name, (void*)v, SectionNameOf(v), how, (void*)lo, (void*)hi, HexAt(v, 24));
        } else {
            LogLine("  repop %-66s -- second pass failed (%s); between %p and %p, estimate %p", r.name, how, (void*)lo, (void*)hi, (void*)estimate);
            for (int d = -64; d < 128; d += 32) LogLine("      %p: %s", (void*)(estimate + d), HexAt(estimate + d, 32));
        }
    }
    int optionalMissing = 0;
    for (int i = 0; i < REPOP_SIG_COUNT; i++)
        if (!found[i] && g_repopSigs[i].var == (void*)&A_LocTrackedItems) optionalMissing++;
    if (ok + optionalMissing != REPOP_SIG_COUNT) {
        LogLine("world repopulation: %d of %d game addresses found on this build", ok, REPOP_SIG_COUNT);
        return false;
    }
    for (int i = 0; i < REPOP_SIG_COUNT; i++) *(uintptr_t*)g_repopSigs[i].var = found[i];
    if (optionalMissing) LogLine("world repopulation: %d of %d game addresses found on this build; LocationInfoGetTrackedItemInfo is optional "
                                 "(the status screen keeps its \"Ally Camp\" title on a camp the enemy took back)", ok, REPOP_SIG_COUNT);
    else LogLine("world repopulation: all %d game addresses found on this build", REPOP_SIG_COUNT);
    return true;
}
#endif
DEFHOOK(void, CPlayer__UpdateController, (void* thiz, float dt)) {
	CCharacter* ch = *(CCharacter**)((uintptr_t)thiz + 0x20);

	if (ch) {
		CMatrix4f posMat;
		// GetVehiclePtr comes from the signature table, not from the SDK's
		// hard-coded pair, so the distance gate also works on builds the SDK
		// does not know. GetTransform is virtual, so it needs no address.
		CVehicle* posVeh = CharacterGetVehiclePtr ? (CVehicle*)CharacterGetVehiclePtr(ch) : nullptr;
		if (posVeh) posVeh->GetTransform(&posMat);
		else ch->GetTransform(&posMat);
		g_PlayerPos = posMat.Position();
#if MM_DEV_TOOLS
		g_lastCurrentVehicle = (void*)posVeh;
		g_lastSignatureVehicle = GetSignatureVehicleNative();
#endif
	}

	// Seed rand() once, on this thread (the CRT keeps its state per thread), before any tick draws from it. It used to be seeded
	// by the first convoy delay, after World Repop had drawn its timers: the first camp attack was always 20.0 min (2026-11-27).
	static bool rngSeeded = false;
	if (!rngSeeded) { srand((unsigned)GetTickCount() ^ GetCurrentProcessId()); rngSeeded = true; }

	ConvoyRespawnTick(dt);
#if MM_DEV_TOOLS && MM_STORM_TOOLS
	WeatherTick(dt);
	StormPresetTick(dt);
#endif

#if MM_DEV_TOOLS || MM_AUTOTEST
    AutotestTick(ch, dt);
#endif
#if MM_DEV_TOOLS || MM_WORLD_REPOP
    if (g_worldRepopOn) { RepopTick(dt); ThreatDiagTick(dt); }
#endif
#if MM_DEV_TOOLS
    if (g_worldRepopOn) {   // camps: numpad 4 = log every camp, numpad 5 = repopulate the nearest liberated camp now, numpad 6 = repair (all liberated)
        static bool np4, np5, np6, np7;
        bool k4 = (GetAsyncKeyState(VK_NUMPAD4) & 0x8000) != 0, k5 = (GetAsyncKeyState(VK_NUMPAD5) & 0x8000) != 0;
        bool k6 = (GetAsyncKeyState(VK_NUMPAD6) & 0x8000) != 0, k7 = (GetAsyncKeyState(VK_NUMPAD7) & 0x8000) != 0;
        if (k7 && !np7 && g_repopInit) SniperForceAll();      // numpad 7: every sniper tower skips its cooldown
        np7 = k7;
        if (k4 && !np4) { CampLogAll("numpad 4"); RepopLog(); }
        if (k5 && !np5) RepopForceNearest();
        if (k6 && !np6) { CampRestoreLiberated(); CampLogAll("after numpad 6 (repair)"); }
        np4 = k4; np5 = k5; np6 = k6;
    }
#endif

    static CVector3f savePos;

#if MM_DEV_TOOLS
    if (GetAsyncKeyState(VK_F3) & 0x8000) {
        if (!f3Pressed) {
            f3Pressed = true;
            g_showDiagnostics = !g_showDiagnostics;
        }
    }
    else {
        f3Pressed = false;
    }
#endif

    // F4 (respawn every finished convoy now, the release twin of the dev F9) was removed on 2026-11-19 (user: "pode remover a função
    // de spawn dos convoys", the F4 key only): convoys come back by the timed sweep alone.

#if MM_DEV_TOOLS
    if (GetAsyncKeyState(VK_RSHIFT) & 0x8000) {
        if (!rightShiftPressed) {
            rightShiftPressed = true;
            enabledAirBrake = !enabledAirBrake;
            ch->m_Invulnerable = false;
            CMatrix4f mat;
            CVehicle* playerVeh = ch->GetVehiclePtr();
            if (playerVeh) {
                playerVeh->SetVelocity(CVector3f());
                playerVeh->GetTransform(&mat);
            }
            else {
                ch->ForceNeutralState();
                ch->GetTransform(&mat);
            }
            savePos = mat.Position();
        }
    }
    else {
        rightShiftPressed = false;
    }

    if (GetAsyncKeyState(VK_F1) & 0x8000) {
        if (!f1Pressed) {
            f1Pressed = true;
            enabledInvincibility = !enabledInvincibility;
        }
    }
    else {
        f1Pressed = false;
    }

    if (GetAsyncKeyState(VK_F2) & 0x8000) {
        if (!f2Pressed) {
            f2Pressed = true;
            enabledConvoyNeverWrecked = !enabledConvoyNeverWrecked;
        }
    }
    else {
        f2Pressed = false;
    }

    if (MM_STORM_TOOLS && (GetAsyncKeyState(VK_F5) & 0x8000)) {
        if (!f5Pressed) {
            f5Pressed = true;
            CycleStormPreset();
        }
    }
    else {
        f5Pressed = false;
    }

    if (GetAsyncKeyState(VK_F6) & 0x8000) {
        if (!f6Pressed) {
            f6Pressed = true;
            void* curVeh = (void*)ch->GetVehiclePtr();
            if (curVeh) {
                VehicleSetMass(curVeh, g_testVehicleMass);
                g_setMassPresses++;
            }
        }
    }
    else {
        f6Pressed = false;
    }

    if (GetAsyncKeyState(VK_F7) & 0x8000) {
        if (!f7Pressed) {
            f7Pressed = true;
            void* curVeh = (void*)ch->GetVehiclePtr();
            if (curVeh) {
                void* pfx = VehicleGetPfxVehicle(curVeh);
                void* chassis = pfx ? PfxVehicleGetChassis(pfx) : nullptr;
                g_lastChassis = chassis;
                if (chassis) {
                    ChassisSetEngineTorqueScale(chassis, g_testTorqueScale);
                    g_setTorquePresses++;
                }
            }
        }
    }
    else {
        f7Pressed = false;
    }

    if (MM_STORM_TOOLS && (GetAsyncKeyState(VK_F11) & 0x8000)) {
        if (!f11Pressed) {
            f11Pressed = true;
            g_forceStormPresses++;
            TriggerStormNow("manual F11");
        }
    }
    else {
        f11Pressed = false;
    }

    if (GetAsyncKeyState(VK_F10) & 0x8000) {
        if (!f10Pressed) {
            f10Pressed = true;
            DumpCompositionProfiles();
        }
    }
    else {
        f10Pressed = false;
    }

    if (GetAsyncKeyState(VK_F8) & 0x8000) {
        if (!f8Pressed) {
            f8Pressed = true;
            ResetAllKnownConvoys();
            // Changed 2026-09-18: the previous build called this with
            // `true` and left it there -- the CT table's own cheat (same
            // function, confirmed by matching AOB) names it "Nuke All
            // Spawned Entities" and only ever calls it in a tight
            // continuous loop while their toggle is on, restoring normal
            // spawning by simply *stopping* the calls, not by calling it
            // again with a different value. That's circumstantial, not
            // proof, but `true` reads as "suppress" and the live test after
            // our one `true` call showed convoys stopped respawning even
            // via area change (previously working) -- calling with `false`
            // instead to test whether that avoids the regression.
            SpawnSystemReset(false);
            g_spawnSystemResetPresses++;
        }
    }
    else {
        f8Pressed = false;
    }

    if (GetAsyncKeyState(VK_F9) & 0x8000) {
        if (!f9Pressed) {
            f9Pressed = true;
            // Dev shortcut: run the v2 sweep immediately, ignoring the random
            // delay and the 2000 m distance gate. Still requires the wreck
            // flow to be finished (graph state 4) -- that part is the whole
            // point of v2 and is never bypassed.
            g_forcedSweeps++;
            ConvoyRespawnSweep(0.0f, true);
        }
    }
    else {
        f9Pressed = false;
    }

    if (enabledAirBrake) {

        auto map = CAvaSingleInstance_EXE(CDeviceManager, ->GetInputManager()->GetActionMap("player"));

        speedBrake += map->GetValue("VehicleWeaponSelectLeft") * 60.f; // MWHEELUP
        speedBrake -= map->GetValue("VehicleWeaponSelectRight") * 60.f; // MWHEELDOWN

        if (speedBrake < 10.0f)
            speedBrake = 10.0f;

        CMatrix4f camMat, chMat;
        CQuaternion chQuat;
        CAvaSingle<CCameraControlManager>::Instance->GetCameraMatrix(camMat);
        float radX, radY, radZ;
        camMat.ToEuler(radX, radY, radZ);

        CVehicle* playerVeh = ch->GetVehiclePtr();
        CGameObject* goBrake = nullptr;
        ch->m_Invulnerable = true;
        if (playerVeh) {
            goBrake = playerVeh;
            playerVeh->SetVelocity(CVector3f());
            playerVeh->GetTransform(&chMat);

            chMat = camMat;

            chQuat.FromEuler(0, radY, 0);
        }
        else {
            goBrake = ch;
            ch->ForceNeutralState();
            ch->RotateInstantly(radY);
            ch->GetTransform(&chMat);

            chQuat.FromMatrix4(chMat);
        }

        CVector3f chPos = savePos;

        auto speedDt = speedBrake * dt;

        chPos = chPos + (chQuat * CVector3f(
            map->GetValue("MoveLeft") ? -speedDt : map->GetValue("MoveRight") ? speedDt : 0,
            0.0,
            map->GetValue("MoveForward") ? -speedDt : map->GetValue("MoveBackward") ? speedDt : 0));

        chPos.y += (GetAsyncKeyState(VK_SPACE) & 0x8000) ? (speedDt * 0.5f) : (GetAsyncKeyState(VK_LSHIFT) & 0x8000) ? (speedDt * -0.5f) : 0;

        chMat.SetPosition(chPos);

        savePos = chPos;
        goBrake->SetTransform(&chMat);
    }

    if (enabledInvincibility) {
        ch->m_Invulnerable = true;
        CVehicle* invVeh = ch->GetVehiclePtr();
        if (invVeh) invVeh->m_Invulnerable = true;
    }
#endif // MM_DEV_TOOLS

    return CPlayer__UpdateController_orig(thiz, dt);
}

void PluginHooks() {
	CGameObject_FindOptional = (FindOptionalFn)g_sigs[SIG_FINDOPTIONAL].resolved;
	if (g_sigs[SIG_GETVEHICLEPTR].matches == 1) CharacterGetVehiclePtr = (CharacterGetVehicleFn)g_sigs[SIG_GETVEHICLEPTR].resolved;
	if (g_sigs[SIG_FIRESTART].matches == 1) ProcessorFireStart = (FireStartFn)g_sigs[SIG_FIRESTART].resolved;

	// The overlay lives in the vendored SDK, which still addresses the render
	// hooks by hard-coded address for two known builds. On anything else it is
	// skipped: the mod itself keeps working, it just has no status box.
	g_overlayAvailable = HookMgr::SdkAddressesUsable();
	if (g_overlayAvailable) ImGuiRenderer::Install();
	LogLine("overlay: %s", g_overlayAvailable ? "installed" : "skipped (unknown build; mod still active, diagnostics only in this log)");

    HookMgr::Install(g_sigs[SIG_UPDATECONTROLLER].resolved, CPlayer__UpdateController_hook, CPlayer__UpdateController_orig);
#if MM_DEV_TOOLS || MM_WORLD_REPOP
    g_worldRepopOn = HookMgr::IsKnownGogBuild() || (SteamFormsUsed() && ResolveRepopSteam());   // Steam forms are in use: the Steam build, or steam_try.txt
    if (!g_worldRepopOn) LogLine("world repopulation: OFF (%s)", SteamFormsUsed() ? "not every game address was found, see the repop lines above"
                                                                                  : "not the GOG build its addresses were taken from");
    else {   // camp repopulation: saves keep repopulated camps liberated (GOG addresses)
        MH_STATUS c = MH_CreateHook((LPVOID)A_LocSaveStatus, (LPVOID)LocSaveStatus_hook, (LPVOID*)&LocSaveStatus_orig);
        MH_STATUS e = c == MH_OK ? MH_EnableHook((LPVOID)A_LocSaveStatus) : c;
        LogLine("camp repopulation: save-status hook %s", MH_StatusToString(e));
        MH_STATUS cl = MH_CreateHook((LPVOID)A_LocLoadStatus, (LPVOID)LocLoadStatus_hook, (LPVOID*)&LocLoadStatus_orig);
        MH_STATUS el = cl == MH_OK ? MH_EnableHook((LPVOID)A_LocLoadStatus) : cl;
        LogLine("camp repopulation: load-status hook (held camps after a save load) %s", MH_StatusToString(el));
        MH_STATUS c2 = MH_CreateHook((LPVOID)A_DestructibleLoad, (LPVOID)DestructibleLoad_hook, (LPVOID*)&DestructibleLoad_orig);
        MH_STATUS e2 = c2 == MH_OK ? MH_EnableHook((LPVOID)A_DestructibleLoad) : c2;
        LogLine("camp repopulation: destructible load hook (sniper towers) %s", MH_StatusToString(e2));
        MH_STATUS c3 = MH_CreateHook((LPVOID)A_TargetSave, (LPVOID)TargetSave_hook, (LPVOID*)&TargetSave_orig);
        MH_STATUS e3 = c3 == MH_OK ? MH_EnableHook((LPVOID)A_TargetSave) : c3;
        LogLine("sniper repopulation: harpoon-target save hook %s", MH_StatusToString(e3));
        MH_STATUS cg = MH_CreateHook((LPVOID)A_GraphScriptHandleEvent, (LPVOID)GraphScriptHandleEvent_hook, (LPVOID*)&GraphScriptHandleEvent_orig);
        MH_STATUS eg = cg == MH_OK ? MH_EnableHook((LPVOID)A_GraphScriptHandleEvent) : cg;
        LogLine("camp repopulation: completion graph hook %s", MH_StatusToString(eg));
        MH_STATUS mf[3]; const uintptr_t mfAt[3] = { A_RouterLoad, A_CountTriggerLoad, A_LootBagLoad };
        LPVOID mfHook[3] = { (LPVOID)RouterLoad_hook, (LPVOID)CountTriggerLoad_hook, (LPVOID)LootBagLoad_hook };
        LPVOID* mfOrig[3] = { (LPVOID*)&RouterLoad_orig, (LPVOID*)&CountTriggerLoad_orig, (LPVOID*)&LootBagLoad_orig };
        for (int k = 0; k < 3; k++) { MH_STATUS c = MH_CreateHook((LPVOID)mfAt[k], mfHook[k], mfOrig[k]); mf[k] = c == MH_OK ? MH_EnableHook((LPVOID)mfAt[k]) : c; }
        LogLine("minefield repopulation: router / count trigger / loot bag load hooks %s, %s, %s", MH_StatusToString(mf[0]), MH_StatusToString(mf[1]), MH_StatusToString(mf[2]));
#if MM_POINT_DIAG
        MH_STATUS cp = MH_CreateHook((LPVOID)A_SpawnPointEnable, (LPVOID)SpawnPointEnable_hook, (LPVOID*)&SpawnPointEnable_orig);
        MH_STATUS ep = cp == MH_OK ? MH_EnableHook((LPVOID)A_SpawnPointEnable) : cp;
        LogLine("camp diagnostic: spawn point enable hook %s", MH_StatusToString(ep));
        MH_STATUS cl = MH_CreateHook((LPVOID)A_LocInfoHandleEvent, (LPVOID)LocInfoHandleEvent_hook, (LPVOID*)&LocInfoHandleEvent_orig);
        MH_STATUS el = cl == MH_OK ? MH_EnableHook((LPVOID)A_LocInfoHandleEvent) : cl;
        LogLine("camp diagnostic: location event hook %s", MH_StatusToString(el));
        MH_STATUS ev[2]; const uintptr_t evAt[2] = { A_EventSystemPost, A_EventImplSend };
        LPVOID evHook[2] = { (LPVOID)EventSystemPost_hook, (LPVOID)EventImplSend_hook };
        LPVOID* evOrig[2] = { (LPVOID*)&EventSystemPost_orig, (LPVOID*)&EventImplSend_orig };
        for (int k = 0; k < 2; k++) { MH_STATUS c = MH_CreateHook((LPVOID)evAt[k], evHook[k], evOrig[k]); ev[k] = c == MH_OK ? MH_EnableHook((LPVOID)evAt[k]) : c; }
        LogLine("camp diagnostic: event post / send hooks %s, %s", MH_StatusToString(ev[0]), MH_StatusToString(ev[1]));
        MH_STATUS fp[2]; const uintptr_t fpAt[2] = { A_ForcePulseExplode, A_ForcePulseExplodeObject };
        LPVOID fpHook[2] = { (LPVOID)ForcePulseExplode_hook, (LPVOID)ForcePulseExplodeObject_hook };
        LPVOID* fpOrig[2] = { (LPVOID*)&ForcePulseExplode_orig, (LPVOID*)&ForcePulseExplodeObject_orig };
        for (int k = 0; k < 2; k++) { MH_STATUS c = MH_CreateHook((LPVOID)fpAt[k], fpHook[k], fpOrig[k]); fp[k] = c == MH_OK ? MH_EnableHook((LPVOID)fpAt[k]) : c; }
        LogLine("camp diagnostic: force pulse explode / explode object hooks %s, %s", MH_StatusToString(fp[0]), MH_StatusToString(fp[1]));
#endif
#if MF_DIAG
        MH_STATUS md[2]; const uintptr_t mdAt[2] = { A_RouterHandleEvent, A_SaveOnDestroy };
        LPVOID mdHook[2] = { (LPVOID)RouterHandleEvent_hook, (LPVOID)SaveOnDestroy_hook };
        LPVOID* mdOrig[2] = { (LPVOID*)&RouterHandleEvent_orig, (LPVOID*)&SaveOnDestroy_orig };
        for (int k = 0; k < 2; k++) { MH_STATUS c = MH_CreateHook((LPVOID)mdAt[k], mdHook[k], mdOrig[k]); md[k] = c == MH_OK ? MH_EnableHook((LPVOID)mdAt[k]) : c; }
        LogLine("minefield diagnostic: router event / save-on-destroy hooks %s, %s", MH_StatusToString(md[0]), MH_StatusToString(md[1]));
#endif
        MH_STATUS c4 = MH_CreateHook((LPVOID)A_LocMainDone, (LPVOID)LocMainDone_hook, (LPVOID*)&LocMainDone_orig);
        MH_STATUS e4 = c4 == MH_OK ? MH_EnableHook((LPVOID)A_LocMainDone) : c4;
        LogLine("camp repopulation: map icon hook %s", MH_StatusToString(e4));
        if (A_LocTrackedItems) {   // 0 on Steam: not in that build (see ResolveRepopSteam)
            MH_STATUS c5 = MH_CreateHook((LPVOID)A_LocTrackedItems, (LPVOID)LocTrackedItems_hook, (LPVOID*)&LocTrackedItems_orig);
            MH_STATUS e5 = c5 == MH_OK ? MH_EnableHook((LPVOID)A_LocTrackedItems) : c5;
            MH_STATUS c6 = MH_CreateHook((LPVOID)A_GuiLocalizedText, (LPVOID)GuiLocalizedText_hook, (LPVOID*)&GuiLocalizedText_orig);
            MH_STATUS e6 = c6 == MH_OK ? MH_EnableHook((LPVOID)A_GuiLocalizedText) : c6;
            LogLine("camp repopulation: banner hooks %s, %s", MH_StatusToString(e5), MH_StatusToString(e6));
        } else LogLine("camp repopulation: banner hooks skipped (no LocationInfoGetTrackedItemInfo on this build)");
        MH_STATUS c7 = MH_CreateHook((LPVOID)A_LocatorGetStringRaw, (LPVOID)LocatorGetStringRaw_hook, (LPVOID*)&LocatorGetStringRaw_orig);
        MH_STATUS e7 = c7 == MH_OK ? MH_EnableHook((LPVOID)A_LocatorGetStringRaw) : c7;
        g_nativeOn = e7 == MH_OK;
        LogLine("camp repopulation: game message text hook %s", MH_StatusToString(e7));
        CampNoticeInit();
        LogLine("camp repopulation: bodies stay %.0f s: %s", REPOP_BODY_SECONDS, RepopBodyTimePatch() ? "patched" : "NOT patched (unexpected bytes)");
        ThreatDiagInstall();
        FILE* f = nullptr; char path[MAX_PATH]; GetModuleFileNameA(NULL, path, MAX_PATH);
        char* sl = strrchr(path, '\\'); if (sl) sl[1] = 0;
        {   // r44: Top Dogs come back with their camp (sd1300 Stump Grinder, sd1210 Rim Jobbie; the warchief camps are dev only)
            // In a warchief camp the Top Dog's death is what opens the arena gates (base object "05_open_gates_trigger (camp_completed)"):
            // dropping its completion trapped the player there (user test 2026-10-16 wc1020). The two scrap depots have no arena: their
            // completion events are dropped while the camp is held and the player is near it, and the mod's own count retakes the camp.
            MH_STATUS c8 = MH_CreateHook((LPVOID)A_EventSend, (LPVOID)EventSend_hook, (LPVOID*)&EventSend_orig);
            MH_STATUS e8 = c8 == MH_OK ? MH_EnableHook((LPVOID)A_EventSend) : c8;
            if (e8 != MH_OK) g_topDogsBack = false;   // without the filter a Top Dog's death would complete the camp: keep them dead
            LogLine("camp repopulation: Top Dogs %s (event filter %s)", g_topDogsBack ? "come back" : "stay dead", MH_StatusToString(e8));
        }
        strcat_s(path, "scripts\\repop_test.txt");             // test override: "cooldown_min_s cooldown_max_s [parts of camp names, they take the next attacks in this order]"
        if (fopen_s(&f, path, "r") == 0 && f) {
            float a = 0, b = 0;
            char first[8 * 17] = "";
            if (fscanf_s(f, "%f %f", &a, &b) == 2 && a >= 0 && b >= a) {
                g_repopCooldownMin = g_repopAttackMin = a; g_repopCooldownMax = g_repopAttackMax = b;
                while (g_repopFirstN < 8 && fscanf_s(f, "%15s", g_repopFirstCamp[g_repopFirstN], (unsigned)sizeof g_repopFirstCamp[0]) == 1) {
                    strcat_s(first, " "); strcat_s(first, g_repopFirstCamp[g_repopFirstN++]);
                }
            }
            fclose(f);
            LogLine("camp repopulation: test cooldown %.0f-%.0f s%s%s", g_repopCooldownMin, g_repopCooldownMax, g_repopFirstN ? ", next attacks on" : "", first);
        }
    }
#endif

    // Addresses from AVAMain_F.pdb, verified 2026-09-15 by parsing the user's
    // own AVAMain.exe PE headers and confirming the RVA lands on a real
    // function prologue (push rdi; sub rsp,0x40; ...) in .text -- not
    // GOG/Steam-gated (that isSteam check was for a different, unrelated
    // dual-address hook and wrongly skipped this one entirely on a non-Steam
    // build in an earlier test).
    {
        LPVOID target = (LPVOID)g_sigs[SIG_SETWRECKED].resolved;
        MH_STATUS createStatus = MH_CreateHook(target, (LPVOID)ConvoyDataSetWrecked_hook, (LPVOID*)&ConvoyDataSetWrecked_orig);
        if (createStatus == MH_OK) {
            MH_STATUS enableStatus = MH_EnableHook(target);
            g_convoySetWreckedHookInstallStatus = MH_StatusToString(enableStatus);
        } else {
            g_convoySetWreckedHookInstallStatus = MH_StatusToString(createStatus);
        }
    }

    // Composition: more escorts (release). Optional signatures -- if any of
    // them is missing the mod simply keeps vanilla convoy compositions.
    {
        if (g_sigs[SIG_TABLEACCESSOR].matches == 1) CompositionTableAccessor = (ResourceTableAccessorFn)g_sigs[SIG_TABLEACCESSOR].resolved;
        if (g_sigs[SIG_MANAGERPTR].matches == 1) g_aiConstantsManagerPtr = (void**)g_sigs[SIG_MANAGERPTR].resolved;
        LPVOID target = (LPVOID)g_sigs[SIG_RESOLVECOMPOSITION].resolved;
        MH_STATUS createStatus = target ? MH_CreateHook(target, (LPVOID)ResolveConvoysComposition_hook, (LPVOID*)&ResolveConvoysComposition_orig) : MH_ERROR_NOT_EXECUTABLE;
        if (createStatus == MH_OK) {
            MH_STATUS enableStatus = MH_EnableHook(target);
            g_compositionResolveHookInstallStatus = MH_StatusToString(enableStatus);
        } else {
            g_compositionResolveHookInstallStatus = MH_StatusToString(createStatus);
        }
        LogLine("composition hook install: %s (%s)", g_compositionResolveHookInstallStatus, g_convoyMapStatus);
    }

    AddVectoredExceptionHandler(1, CrashStackHandler);

#if MM_DEV_TOOLS
    // Spawn-flow logging hooks (GOG addresses from AVAMain_F.pdb; dev only)
    InstallLoggedNodeHook(0x140F76B00ull, (LPVOID)FireConnectionsLog_hook, (LPVOID*)&FireConnectionsLog_orig, "CProcessor::FireConnections");
    InstallLoggedNodeHook(0x1402E2420ull, (LPVOID)RequestSpawnLog_hook, (LPVOID*)&RequestSpawnLog_orig, "RequestSpawn");
    InstallLoggedNodeHook(0x1402BE010ull, (LPVOID)SpawnPossibleFailedLog_hook, (LPVOID*)&SpawnPossibleFailedLog_orig, "ConvoyMetricIsSpawnPossibleFailed");
    InstallLoggedNodeHook(0x1402BFD10ull, (LPVOID)SpawningFailedLog_hook, (LPVOID*)&SpawningFailedLog_orig, "ConvoyMetricInProgressSpawningFailed");
    InstallLoggedNodeHook(0x1402E2E80ull, (LPVOID)SpawnAvailableLog_hook, (LPVOID*)&SpawnAvailableLog_orig, "SpawnMultipleEntitiesAvailable");
    InstallLoggedNodeHook(0x1402EBDD0ull, (LPVOID)SpawnHoldLog_hook, (LPVOID*)&SpawnHoldLog_orig, "SpawnHoldAfterResourcesLoaded");
    InstallLoggedNodeHook(0x1402EBEA0ull, (LPVOID)SpawnResourcesLoadedLog_hook, (LPVOID*)&SpawnResourcesLoadedLog_orig, "SpawnResourcesLoaded");
    InstallLoggedNodeHook(0x1402C3B30ull, (LPVOID)IterateGuardsLog_hook, (LPVOID*)&IterateGuardsLog_orig, "ConvoysCompositionIterateGuards");
    InstallLoggedNodeHook(0x1402C3740ull, (LPVOID)CCMapIsBlockedLog_hook, (LPVOID*)&CCMapIsBlockedLog_orig, "ConvoyDataCCMapIsBlocked");


    // NGSONodes::SpawnStorm -- address from AVAMain_F.pdb (0x1402F0400), same
    // PDB/exe pairing already relied on for every other hook in this file.
    // No known GOG offset yet (found via a Steam-oriented PDB grep), so this
    // is installed unconditionally like ConvoyDataSetWrecked above, not via
    // the ADDRESS(gog, steam) macro.
    {
        LPVOID target = (LPVOID)g_sigs[SIG_SPAWNSTORM].resolved;
        MH_STATUS createStatus = target ? MH_CreateHook(target, (LPVOID)SpawnStorm_hook, (LPVOID*)&SpawnStorm_orig) : MH_ERROR_NOT_EXECUTABLE;
        if (createStatus == MH_OK) {
            MH_STATUS enableStatus = MH_EnableHook(target);
            g_spawnStormHookInstallStatus = MH_StatusToString(enableStatus);
        } else {
            g_spawnStormHookInstallStatus = MH_StatusToString(createStatus);
        }
    }
    // Weather manager render hook for the storm probe (fixed GOG address).
    if (MM_STORM_TOOLS && HookMgr::isGOG) {
        LPVOID target = (LPVOID)0x140205650;
        MH_STATUS c = MH_CreateHook(target, (LPVOID)TodInternalUpdateRender_hook, (LPVOID*)&TodInternalUpdateRender_orig);
        MH_STATUS e = (c == MH_OK) ? MH_EnableHook(target) : c;
        snprintf(g_todHookStatus, sizeof(g_todHookStatus), "%s", MH_StatusToString(e));
    } else {
        snprintf(g_todHookStatus, sizeof(g_todHookStatus), "skipped (not the GOG build)");
    }
    LogLine("STORMPROBE: weather manager render hook: %s", g_todHookStatus);
#endif // MM_DEV_TOOLS
}