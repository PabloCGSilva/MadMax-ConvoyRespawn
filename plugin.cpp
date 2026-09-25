#include "plugin.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
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

// Storm presets moved to their own plugin, Wasteland Storms (WastelandStorms.asi,
// 2026-09-23). Kept here, switched off, so both can be installed side by side
// without two plugins answering F5/F11 and firing storms.
#define MM_STORM_TOOLS 0

// User-requested (2026-09-18): always show a visible tag for whatever
// build is currently installed, so it's never ambiguous which test is
// running. Bump this string every time a new test build goes out.
#define MM_BUILD_TAG "v0.9.2-beta3 (2026-09-24)"

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

typedef void* (*CharacterGetVehicleFn)(void* character);
static CharacterGetVehicleFn CharacterGetVehiclePtr = nullptr; // from g_sigs[SIG_GETVEHICLEPTR]
static bool g_overlayAvailable = false;   // the SDK's own hard-coded addresses (overlay) only fit two known builds

static uintptr_t g_exeBase = 0;
static uintptr_t g_textStart = 0, g_textEnd = 0;
static unsigned int g_exeTimeStamp = 0, g_exeImageSize = 0;

static bool ScanBytes(const unsigned char* bytes, const bool* wild, int len, GameSignature& sig) {
    sig.matches = 0; sig.resolved = 0;
    const unsigned char* p = (const unsigned char*)g_textStart;
    const unsigned char* end = (const unsigned char*)g_textEnd - len;
    for (; p <= end; p++) {
        if (!wild[0] && p[0] != bytes[0]) continue;
        int i = 1;
        for (; i < len; i++) if (!wild[i] && p[i] != bytes[i]) break;
        if (i == len) {
            if (++sig.matches == 1) sig.resolved = (uintptr_t)p;
            else break;
        }
    }
    return sig.matches == 1;
}

// Another ASI may already have hooked the same function (Enhanced Convoys and
// Wasteland Storms both hook CPlayer::UpdateController). MinHook replaces the
// first 5 bytes with `jmp rel32` (E9 xx xx xx xx) and leaves every byte after
// them untouched, so if the plain pattern finds nothing, look for exactly
// that: E9 + 4 wildcards + the original pattern from byte 5 on.
static bool ScanPattern(GameSignature& sig) {
    unsigned char bytes[128]; bool wild[128]; int len = 0;
    for (const char* c = sig.pattern; *c && len < 128; ) {
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

static bool ResolveGameAddresses() {
    HMODULE exe = GetModuleHandleA(NULL);
    g_exeBase = (uintptr_t)exe;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)exe;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "no DOS header"); return false; }
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(g_exeBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "no PE header"); return false; }
    g_exeTimeStamp = nt->FileHeader.TimeDateStamp;
    g_exeImageSize = nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (memcmp(sec->Name, ".text", 5) == 0) {
            g_textStart = g_exeBase + sec->VirtualAddress;
            g_textEnd = g_textStart + sec->Misc.VirtualSize;
            break;
        }
    }
    if (!g_textStart) { snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "no .text section"); return false; }

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
        snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "signature not found: %s (exe timestamp 0x%08X)", failList, g_exeTimeStamp);
        return false;
    }
    snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "OK, %d/%d signatures resolved (exe timestamp 0x%08X, %s)",
        SIG_COUNT - (int)(!g_sigs[SIG_RESOLVECOMPOSITION].matches) - (int)(g_sigs[SIG_TABLEACCESSOR].matches != 1) - (int)(g_sigs[SIG_MANAGERPTR].matches != 1) - (int)(g_sigs[SIG_SPAWNSTORM].matches != 1),
        SIG_COUNT, g_exeTimeStamp, g_exeTimeStamp == 0x566520B2u ? "the GOG build this was made on" : "a build not seen by the author -- please report");
    return true;
}

// ------------------------------------------------------------- startup --
// Test switch: MADMAX_MODS_DEFER=1 in the environment, or a file
// scriptsorce_steam_path.txt, forces the deferred (Steam) startup path on a
// build whose code is readable at load time.
static bool ForceDefer() {
    char v[8];
    if (GetEnvironmentVariableA("MADMAX_MODS_DEFER", v, sizeof(v)) > 0 && v[0] == '1') return true;
    char path[MAX_PATH];                             // or a marker file: <game>\scriptsorce_steam_path.txt
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* slash = strrchr(path, '\\'); if (slash) slash[1] = 0;
    strcat_s(path, "scripts\\force_steam_path.txt");
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
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
static void WINAPI GetStartupInfoW_hook(LPSTARTUPINFOW info, uintptr_t chain) {
    GetStartupInfoW_orig(info, chain);
    if (g_installed) return;
    g_gameVersionOk = ResolveGameAddresses();
    if (!g_gameVersionOk && NoSignatureMatched()) return;   // still encrypted, wait for the next call
    if (InterlockedExchange(&g_installed, 1) == 0) {
        LogLine("game code ready (C runtime startup), installing");
        InstallMod(g_attachModule);
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
    if (dwReason == DLL_PROCESS_DETACH) {
        LogConvoySnapshot("game exit");
        LogLine("session end");
        return TRUE;
    }
    if (dwReason == DLL_PROCESS_ATTACH) {
        InitModPaths(hModule);
        LogSessionStart();
        DetectConvoyMapPoints();
        MH_Initialize();
        g_gameVersionOk = ResolveGameAddresses();
        LogLine("session start, build %s", MM_BUILD_TAG);
        if ((!g_gameVersionOk && NoSignatureMatched()) || ForceDefer()) {
            // Steam: the executable is wrapped by Steam's DRM and its code is
            // still encrypted while plugins load, so nothing can match yet. The
            // game's C runtime startup calls GetStartupInfoW once the real code
            // runs; resolve and install from there (what mm_sdk's
            // HookMgr::Initialize does on Steam, minus its hard-coded address).
            LPVOID gsi = (LPVOID)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetStartupInfoW");
            MH_STATUS c = gsi ? MH_CreateHook(gsi, (LPVOID)GetStartupInfoW_hook, (LPVOID*)&GetStartupInfoW_orig) : MH_ERROR_FUNCTION_NOT_FOUND;
            MH_STATUS e = (c == MH_OK) ? MH_EnableHook(gsi) : c;
            LogLine("game code not readable yet (Steam DRM?), installing at game startup: %s", MH_StatusToString(e));
            g_attachModule = hModule;
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
            LogLine("  signature %-70s %s %p (matches %d)%s", g_sigs[i].name, g_sigs[i].matches == 1 ? "OK" : "--", (void*)g_sigs[i].resolved, g_sigs[i].matches,
                g_sigs[i].alreadyHooked ? " -- already hooked by another mod, chaining behind it" : "");
        if (!g_gameVersionOk) {
            char msg[512];
            snprintf(msg, sizeof(msg),
                "Mad Max - Enhanced Convoys is DISABLED: it could not locate the game functions it needs in this executable.\n\n"
                "Reason: %s\n\n"
                "The mod stays loaded but does nothing, so the game is safe to play. "
                "Please report your game version (store + patch) to the mod author.", g_gameVersionStatus);
            MessageBoxA(NULL, msg, "Mad Max - Enhanced Convoys", MB_OK | MB_ICONWARNING);
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
bool f4Pressed = false;
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
typedef bool (*FireStartFn)(void* processor, unsigned int startNameHash);
static FireStartFn ProcessorFireStart = nullptr;   // from g_sigs[SIG_FIRESTART]

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
struct ConvoyPair { uint64_t containerId; uint64_t logicGraphId; };
static const ConvoyPair g_convoyPairs[] = {
    { 0x7E90E3F6, 0xE6D229A4 },
    { 0x38A45D73, 0x65271E53 },
    { 0x8F1728CD, 0x8902D2DC },
    { 0x59501178, 0xEA9391A6 },
    { 0x7D6BB232, 0xC52D952C },
    { 0x132E3492, 0x12F7F772 },
    { 0x42D456AE, 0xA79A34DB },
    { 0x7C5903DF, 0x02794167 },
    { 0x6FDA7EF0, 0x7389B76D },
    { 0x337019D7, 0x70F9DD52 },
    { 0x1FA21EA1, 0xC415F9A8 },
    { 0xB6418D01, 0x84FCD7AC },
    { 0x74C87945, 0x2C42382A },
    { 0x35762FBA, 0x204F397C },
};
const int g_convoyPairCount = sizeof(g_convoyPairs) / sizeof(g_convoyPairs[0]);

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
};
static TrackedConvoy g_tracked[sizeof(g_convoyPairs) / sizeof(g_convoyPairs[0])] = {};
static float g_sweepAccum = 0.0f;
static int g_sweepCount = 0;
static int g_totalAutoRespawns = 0;
static bool g_respawnRngSeeded = false;

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
    if (!g_respawnRngSeeded) { srand((unsigned)GetTickCount()); g_respawnRngSeeded = true; }
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
        if (!t.prevFound) { LogLine("convoy %2d: found (wrecked=%d, graph state %d)", i + 1, t.wrecked, t.graphState); t.prevFound = true; t.prevWrecked = t.wrecked; t.prevGraphState = t.graphState; }
        if (t.wrecked != t.prevWrecked) { LogLine("convoy %2d: %s", i + 1, t.wrecked ? "WRECKED (flag set)" : "wrecked flag cleared"); t.prevWrecked = t.wrecked; }
        if (t.graphState != t.prevGraphState) { LogLine("convoy %2d: graph state %d -> %d", i + 1, t.prevGraphState, t.graphState); t.prevGraphState = t.graphState; }

        if (!t.wrecked) {
            t.armed = false;
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

        const float* wreckMat = (const float*)((uintptr_t)container + 0xD8);
        float wx = wreckMat[12], wy = wreckMat[13], wz = wreckMat[14];
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
        LogLine("convoy %2d: RESPAWN triggered%s (player %.0fm from wreck, total this session %d)", i + 1, force ? " by F4/F9" : "", t.lastDistance, g_totalAutoRespawns);
    }
}

static void LogConvoySnapshot(const char* why) {
    LogLine("--- snapshot (%s): sweeps %d, auto-respawns %d, player at %.0f %.0f %.0f", why, g_sweepCount, g_totalAutoRespawns, g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z);
    for (int i = 0; i < g_convoyPairCount; i++) {
        const TrackedConvoy& t = g_tracked[i];
        if (!t.foundThisSweep) { LogLine("    convoy %2d: not found", i + 1); continue; }
        if (!t.wrecked) LogLine("    convoy %2d: active (graph state %d) respawns %d", i + 1, t.graphState, t.respawns);
        else if (!t.armed) LogLine("    convoy %2d: WRECKED, wreck flow running (graph state %d)", i + 1, t.graphState);
        else LogLine("    convoy %2d: WRECKED, respawn in %.0fs, player %.0fm from wreck", i + 1, t.delayRemaining, t.lastDistance);
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

// Release overlay: small, silent-by-default status the mod author needs
// (proof it's alive) without dumping internal hook plumbing on an end user.
// F3 expands it into the full diagnostic view for troubleshooting reports.
class ConvoyModOverlay : public ImGuiRenderer {
	void Render() override {
#if MM_DEV_TOOLS
		// Dev builds (2026-09-24): Enhanced Convoys and Wasteland Storms are
		// released, so their status lines are gone from our own overlay -- only
		// what is still being worked on stays. The release overlay below is
		// what a MM_DEV_TOOLS 0 build shows.
		ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
		ImGui::Begin("Mad Max dev tools", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
		ImGui::Text("Dev build: %s", MM_BUILD_TAG);
		ImGui::Text("Pos: X=%.1f Y=%.1f Z=%.1f", g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z);
		ImGui::Text("Invincibility (F1): %s", enabledInvincibility ? "ON" : "off");
		ImGui::Text("Vehicle: %s", (g_lastCurrentVehicle && g_lastCurrentVehicle == g_lastSignatureVehicle) ? "Magnum Opus" :
			(g_lastCurrentVehicle ? "another car" : "on foot"));
		if (g_showDiagnostics) {
			ImGui::Separator();
			ImGui::Text("Current vehicle: %p  |  Magnum Opus: %p  |  chassis: %p", g_lastCurrentVehicle, g_lastSignatureVehicle, g_lastChassis);
			ImGui::Text("F6: set current vehicle mass to %.0f (pressed %d)", g_testVehicleMass, g_setMassPresses);
			ImGui::Text("F7: set current vehicle engine torque scale to %.1fx (pressed %d)", g_testTorqueScale, g_setTorquePresses);
		}
		ImGui::Text("F3: %s details", g_showDiagnostics ? "hide" : "show");
		ImGui::End();
		return;
#endif
		ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
		ImGui::Begin("Convoy Respawn Mod", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);

		ImGui::Text("Build: %s", MM_BUILD_TAG);
		ImGui::Text("Enhanced Convoys: %s  |  game check: %s", enabledConvoyNeverWrecked ? "ACTIVE" : "off", g_gameVersionStatus);
		{
			int wrecked = 0, armed = 0;
			for (int i = 0; i < g_convoyPairCount; i++) { if (g_tracked[i].foundThisSweep && g_tracked[i].wrecked) wrecked++; if (g_tracked[i].armed) armed++; }
			ImGui::Text("Convoys wrecked: %d  |  waiting to respawn: %d  |  auto-respawned this session: %d", wrecked, armed, g_totalAutoRespawns);
		}
		ImGui::Text("F4: respawn every destroyed convoy now (skips the random wait and the distance check)");
		ImGui::Text("F3: show/hide diagnostics");

		if (g_showDiagnostics) {
			ImGui::Separator();
			ImGui::Text("SetWrecked hook install: %s", g_convoySetWreckedHookInstallStatus);
			ImGui::Text("ConvoyDataSetWrecked calls: %d", g_convoySetWreckedCalls);
			ImGui::Text("Resolved game addresses (by byte signature):");
			for (int i = 0; i < SIG_COUNT; i++)
				ImGui::Text("  %-70s %s %p%s", g_sigs[i].name, g_sigs[i].matches == 1 ? "OK " : "-- ", (void*)g_sigs[i].resolved,
					g_sigs[i].matches == 1 ? "" : (g_sigs[i].required ? "  (REQUIRED, not found)" : "  (optional, not found)"));
			ImGui::Text("Reset ALL known convoys (F4): last run -> found %d, cleared %d, skipped(sanity) %d",
				g_resetConvoysLastFound, g_resetConvoysLastCleared, g_resetConvoysLastSkippedSanity);
			ImGui::Text("Escort boost: %s  |  hook %s  |  %s", g_compositionPatchStatus, g_compositionResolveHookInstallStatus, g_convoyMapStatus);
			ImGui::Text("  ResolveConvoysComposition calls: %d  |  last id: %u (%d)  %s  |  guard entries in last profile: %d",
				g_compositionResolveCalls, g_compositionLastId, (int)g_compositionLastId, g_compositionLastOk ? "found" : "NOT FOUND", g_compositionLastGuardEntries);
			ImGui::Separator();
			ImGui::Text("  AddToUpdate() calls (re-registering rebuilt graph objects): %d", g_addToUpdateCalls);
			ImGui::Text("Respawn v2 sweep: every %.0fs (%d done)  |  delay %.0f-%.0fs after the wreck flow finishes (graph state %d)  |  player must be > %.0fm from the wreck",
				CONVOY_RESPAWN_SWEEP_INTERVAL_S, g_sweepCount, CONVOY_RESPAWN_DELAY_MIN_S, CONVOY_RESPAWN_DELAY_MAX_S, CONVOY_GRAPH_STATE_DONE, CONVOY_RESPAWN_MIN_PLAYER_DISTANCE);
			for (int i = 0; i < g_convoyPairCount; i++) {
				const TrackedConvoy& t = g_tracked[i];
				if (!t.foundThisSweep) { ImGui::Text("  convoy %2d: not found", i + 1); continue; }
				if (!t.wrecked) { ImGui::Text("  convoy %2d: active (graph state %d)", i + 1, t.graphState); continue; }
				if (!t.armed) { ImGui::Text("  convoy %2d: WRECKED, vanilla wreck flow still running (graph state %d) -- collect the hood ornament / let wrecks despawn", i + 1, t.graphState); continue; }
				if (t.lastDistance < 0.0f)
					ImGui::Text("  convoy %2d: WRECKED, respawn in %.0fs (no wreck position known, distance gate skipped)  respawns so far: %d", i + 1, t.delayRemaining, t.respawns);
				else
					ImGui::Text("  convoy %2d: WRECKED, respawn in %.0fs, player %.0fm from wreck (need > %.0fm)  respawns so far: %d", i + 1, t.delayRemaining, t.lastDistance, CONVOY_RESPAWN_MIN_PLAYER_DISTANCE, t.respawns);
			}
#if MM_DEV_TOOLS
			ImGui::Separator();
			ImGui::Text("[MM_DEV_TOOLS build]");
			ImGui::Text("Pos: X=%.1f Y=%.1f Z=%.1f", g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z);
			ImGui::Text("Invincibility (F1): %s", enabledInvincibility ? "ON" : "off");
			ImGui::Text("SpawnStorm hook install: %s", g_spawnStormHookInstallStatus);
			ImGui::Text("  SpawnStorm calls (natural, not caused by us): %d", g_spawnStormCalls);
			ImGui::Text("Storms (F5 cycles): %s", g_stormPresets[g_stormPreset].name);
			if (g_stormPreset != STORM_OFF)
				ImGui::Text("   triggered %d  |  reached you %d  |  missed %d", g_stormTriggers, g_stormArrived, g_stormMissed);
			ImGui::Text("   %s", g_stormToggleStatus);
			ImGui::Text("Force one storm now, storm.trigger (F11): pressed %d time(s)", g_forceStormPresses);
			ImGui::Text("%s", g_stormProbeStatus);
			ImGui::Text("Current vehicle: %p  |  Signature vehicle: %p  |  %s", g_lastCurrentVehicle, g_lastSignatureVehicle,
				(g_lastCurrentVehicle && g_lastCurrentVehicle == g_lastSignatureVehicle) ? "SAME (driving own car)" :
				(g_lastCurrentVehicle ? "DIFFERENT (driving a captured/other car!)" : "no vehicle"));
			ImGui::Text("Set CURRENT vehicle mass to %.0f (F6): pressed %d time(s)", g_testVehicleMass, g_setMassPresses);
			ImGui::Text("Chassis (via GetPfxVehicle->GetChassis): %p", g_lastChassis);
			ImGui::Text("Set CURRENT vehicle engine torque scale to %.1fx (F7): pressed %d time(s)", g_testTorqueScale, g_setTorquePresses);
			ImGui::Text("Reset convoys + SpawnSystemReset, no area change (F8): pressed %d time(s)", g_spawnSystemResetPresses);
			ImGui::Text("  -> watch the map/HUD Threat display before and after pressing F8");
			ImGui::Separator();
			ImGui::Separator();
			ImGui::Text("Convoy graph object captures: %d  (path-hash mismatches: %d)", g_convoyGraphObjectCaptures, g_convoyGraphObjectCaptureMismatches);
			ImGui::Text("  Last graph path hash read: %u  (expect %u for convoy_choreographer.gsr)", g_lastConvoyGraphPathHash, CONVOY_CHOREOGRAPHER_PATH_HASH);
			ImGui::Text("  Captured graph object: %p", g_lastConvoyGraphObject);
			ImGui::Text("F9: force the v2 respawn sweep now (bypasses delay + distance; still needs graph state 4) -- pressed %d", g_forcedSweeps);
			if (g_lastConvoyGraphObject) {
				ImGui::Text("  Last wrecked convoy's graph: m_State=%d  m_GraphProcessor=%p",
					*(int*)((uintptr_t)g_lastConvoyGraphObject + 0xE0), *(void**)((uintptr_t)g_lastConvoyGraphObject + 0xF0));
			}
#endif
		}
		ImGui::End();
	}
};
static ConvoyModOverlay g_convoyModOverlay;
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

	ConvoyRespawnTick(dt);
#if MM_DEV_TOOLS && MM_STORM_TOOLS
	WeatherTick(dt);
	StormPresetTick(dt);
#endif

    static CVector3f savePos;

    if (GetAsyncKeyState(VK_F3) & 0x8000) {
        if (!f3Pressed) {
            f3Pressed = true;
            g_showDiagnostics = !g_showDiagnostics;
        }
    }
    else {
        f3Pressed = false;
    }

    if (GetAsyncKeyState(VK_F4) & 0x8000) {
        if (!f4Pressed) {
            f4Pressed = true;
            // v2: F4 is the same forced sweep as the dev F9 -- respawn every
            // convoy whose wreck flow has finished, skipping the wait and the
            // distance gate. It no longer clears flags on convoys whose wreck
            // handler is still running (that would break the hood-ornament
            // flow; see the SetWrecked hook comment).
            g_forcedSweeps++;
            ConvoyRespawnSweep(0.0f, true);
        }
    }
    else {
        f4Pressed = false;
    }

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
#if MM_DEV_TOOLS
	if (g_sigs[SIG_FIRESTART].matches == 1) ProcessorFireStart = (FireStartFn)g_sigs[SIG_FIRESTART].resolved;
#endif

	// The overlay lives in the vendored SDK, which still addresses the render
	// hooks by hard-coded address for two known builds. On anything else it is
	// skipped: the mod itself keeps working, it just has no status box.
	g_overlayAvailable = HookMgr::SdkAddressesUsable();
	if (g_overlayAvailable) ImGuiRenderer::Install();
	LogLine("overlay: %s", g_overlayAvailable ? "installed" : "skipped (unknown build; mod still active, diagnostics only in this log)");

    HookMgr::Install(g_sigs[SIG_UPDATECONTROLLER].resolved, CPlayer__UpdateController_hook, CPlayer__UpdateController_orig);

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