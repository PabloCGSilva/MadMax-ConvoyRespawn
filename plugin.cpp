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

// ---- Game version gate (public beta, 2026-09-21) ----
// Every address in this file is absolute and was verified against exactly
// one binary: AVAMain.exe, non-Steam build, PE TimeDateStamp 0x566520B2
// (2015-12-07), SizeOfImage 0x01AA8000, no ASLR (DYNAMIC_BASE off, so it
// always loads at 0x140000000). On any other executable those addresses are
// garbage and hooking them would crash someone else's game. So before
// touching anything -- including the SDK's own HookMgr::Initialize(), which
// reads a magic value at a fixed address -- the main module's PE header and
// 24 bytes of code at every function this mod calls or hooks are compared
// against what that binary contains. Any mismatch: the plugin stays loaded
// but completely inert, tells the user why, and returns.
static bool g_gameVersionOk = false;
static char g_gameVersionStatus[160] = "not checked";

struct CodeSignature { const char* name; uintptr_t addr; unsigned char bytes[24]; };
static const CodeSignature g_signatures[] = {
    { "NGSONodes::ConvoyDataSetWrecked", 0x1402BDB20ull, { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x5C, 0x24, 0x50, 0x48, 0x89, 0x6C, 0x24 } },
    { "GetGameObjectTyped<CConvoyDataContainer>", 0x1402BC8B0ull, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0xE8, 0x4C, 0x38, 0xFE, 0xFF, 0x48, 0x8B, 0xD8, 0x48 } },
    { "CGameObject::FindOptional", 0x1406B8900ull, { 0x48, 0x8B, 0xC4, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x48, 0xC7, 0x44, 0x24, 0x48, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70 } },
    { "CGameObject::AddToUpdate", 0x14068E290ull, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01, 0x80, 0xA1, 0x8C, 0x00, 0x00, 0x00 } },
    { "CGraphScriptGameObject::UpdatePostSim", 0x1402AEF60ull, { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x5C, 0x24, 0x58, 0x48, 0x89, 0x74, 0x24 } },
    { "CPlayer::UpdateController", 0x1404C6670ull, { 0x48, 0x8B, 0xC4, 0x41, 0x54, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x58 } },
};

static bool VerifyGameVersion() {
    HMODULE exe = GetModuleHandleA(NULL);
    if ((uintptr_t)exe != 0x140000000ull) {
        snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "executable not loaded at 0x140000000 (got %p)", (void*)exe);
        return false;
    }
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)exe;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "no DOS header"); return false; }
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)((uintptr_t)exe + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "no PE header"); return false; }
    if (nt->FileHeader.TimeDateStamp != 0x566520B2u || nt->OptionalHeader.SizeOfImage != 0x01AA8000u) {
        snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "different AVAMain.exe build (timestamp 0x%08X, image size 0x%08X; expected 0x566520B2 / 0x01AA8000)",
            (unsigned)nt->FileHeader.TimeDateStamp, (unsigned)nt->OptionalHeader.SizeOfImage);
        return false;
    }
    for (const CodeSignature& sig : g_signatures) {
        if (memcmp((const void*)sig.addr, sig.bytes, sizeof(sig.bytes)) != 0) {
            snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "code mismatch at %s", sig.name);
            return false;
        }
    }
    // The variable-pin hash immediate inside ConvoyDataSetWrecked (mov r8d, 0xBAFB74B7).
    if (*(const unsigned int*)(0x1402BDB56ull) != 0xBAFB74B7u) {
        snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "code mismatch at ConvoyDataSetWrecked pin hash");
        return false;
    }
    snprintf(g_gameVersionStatus, sizeof(g_gameVersionStatus), "OK (AVAMain.exe build 2015-12-07, non-Steam)");
    return true;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH) {
        g_gameVersionOk = VerifyGameVersion();
        if (!g_gameVersionOk) {
            char msg[512];
            snprintf(msg, sizeof(msg),
                "Mad Max Convoy Respawn Mod is DISABLED: this game executable is not the build it was made for.\n\n"
                "Reason: %s\n\n"
                "The mod stays loaded but does nothing, so the game is safe to play. "
                "Please report your game version (store + patch) to the mod author.", g_gameVersionStatus);
            MessageBoxA(NULL, msg, "Mad Max Convoy Respawn Mod", MB_OK | MB_ICONWARNING);
            return TRUE;
        }
        HookMgr::Initialize();
        PluginAttach(hModule, dwReason, lpReserved);
    }
    return TRUE;
}

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

// User-requested (2026-09-18): always show a visible tag for whatever
// build is currently installed, so it's never ambiguous which test is
// running. Bump this string every time a new test build goes out.
#define MM_BUILD_TAG "v0.9.0-beta1 (2026-09-21)"

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
static GetConvoyContainerFn GetConvoyContainer = (GetConvoyContainerFn)0x1402BC8B0;

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
    {
        void* container = GetConvoyContainer(processor, node, CONVOY_NODE_CONTAINER_PIN_HASH);
        if (container) g_convoySetWreckedCleared++; else g_convoySetWreckedResolveFailed++;
    }

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
static FindOptionalFn CGameObject_FindOptional = (FindOptionalFn)0x1406B8900;

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
typedef void (*SendEventMsgFn)(const char* msg);
static SendEventMsgFn SendEventMsg = (SendEventMsgFn)0x140007ca0;

bool f5Pressed = false;
int g_spawnStormCalls = 0;
const char* g_spawnStormHookInstallStatus = "not attempted yet";
int g_forceStormPresses = 0;

DEFHOOK(uint32_t, SpawnStorm, (void* processor, const void* node, unsigned int pin)) {
    g_spawnStormCalls++;
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

static CVector3f g_PlayerPos(0.f, 0.f, 0.f);

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

        if (!t.wrecked) {
            t.armed = false;
            // Recovery: a rebuild we kicked off that the engine never picked up
            // (state 0, no processor) -- re-register it so UpdatePostSim runs.
            // Freshly created objects also pass through state 0 for a frame
            // (and are registered normally), so only act after it has sat
            // there across two consecutive sweeps (>= 1 s).
            if (t.graphState == 0 && *(void**)((uintptr_t)graphObj + 0xF0) == nullptr) {
                if (++t.stateZeroSweeps >= 2) { GameObjectAddToUpdate(graphObj); t.stateZeroSweeps = 0; }
            } else {
                t.stateZeroSweeps = 0;
            }
            continue;
        }
        if (t.graphState != CONVOY_GRAPH_STATE_DONE) { t.armed = false; continue; } // vanilla wreck flow still running

        if (!t.armed) {
            t.armed = true;
            t.delayRemaining = RandomDelaySeconds();
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
    }
}

static void ConvoyRespawnTick(float dt) {
    if (!enabledConvoyNeverWrecked) return;
    g_sweepAccum += dt;
    if (g_sweepAccum < CONVOY_RESPAWN_SWEEP_INTERVAL_S) return;
    float elapsed = g_sweepAccum;
    g_sweepAccum = 0.0f;
    ConvoyRespawnSweep(elapsed, false);
}

// Release overlay: small, silent-by-default status the mod author needs
// (proof it's alive) without dumping internal hook plumbing on an end user.
// F3 expands it into the full diagnostic view for troubleshooting reports.
class ConvoyModOverlay : public ImGuiRenderer {
	void Render() override {
		ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
		ImGui::Begin("Convoy Respawn Mod", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);

		ImGui::Text("Build: %s", MM_BUILD_TAG);
		ImGui::Text("Convoy Respawn Mod: %s  |  game version check: %s", enabledConvoyNeverWrecked ? "ACTIVE" : "off", g_gameVersionStatus);
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
			ImGui::Text("ConvoyDataSetWrecked calls: %d  (container resolved: %d, resolve failed: %d)",
				g_convoySetWreckedCalls, g_convoySetWreckedCleared, g_convoySetWreckedResolveFailed);
			ImGui::Text("  Trigger pin seen on last call: %u  |  resolving container via variable-pin hash %u", g_lastConvoySetWreckedRealPin, CONVOY_NODE_CONTAINER_PIN_HASH);
			ImGui::Text("Reset ALL known convoys (F4): last run -> found %d, cleared %d, skipped(sanity) %d",
				g_resetConvoysLastFound, g_resetConvoysLastCleared, g_resetConvoysLastSkippedSanity);
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
			ImGui::Text("Force storm.trigger (F5): pressed %d time(s)", g_forceStormPresses);
			ImGui::Text("Current vehicle: %p  |  Signature vehicle: %p  |  %s", g_lastCurrentVehicle, g_lastSignatureVehicle,
				(g_lastCurrentVehicle && g_lastCurrentVehicle == g_lastSignatureVehicle) ? "SAME (driving own car)" :
				(g_lastCurrentVehicle ? "DIFFERENT (driving a captured/other car!)" : "no vehicle"));
			ImGui::Text("Set CURRENT vehicle mass to %.0f (F6): pressed %d time(s)", g_testVehicleMass, g_setMassPresses);
			ImGui::Text("Chassis (via GetPfxVehicle->GetChassis): %p", g_lastChassis);
			ImGui::Text("Set CURRENT vehicle engine torque scale to %.1fx (F7): pressed %d time(s)", g_testTorqueScale, g_setTorquePresses);
			ImGui::Text("Reset convoys + SpawnSystemReset, no area change (F8): pressed %d time(s)", g_spawnSystemResetPresses);
			ImGui::Text("  -> watch the map/HUD Threat display before and after pressing F8");
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
		CVehicle* posVeh = ch->GetVehiclePtr();
		if (posVeh) posVeh->GetTransform(&posMat);
		else ch->GetTransform(&posMat);
		g_PlayerPos = posMat.Position();
#if MM_DEV_TOOLS
		g_lastCurrentVehicle = (void*)posVeh;
		g_lastSignatureVehicle = GetSignatureVehicleNative();
#endif
	}

	ConvoyRespawnTick(dt);

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

    if (GetAsyncKeyState(VK_F5) & 0x8000) {
        if (!f5Pressed) {
            f5Pressed = true;
            g_forceStormPresses++;
            SendEventMsg("storm.trigger");
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
	ImGuiRenderer::Install();

    HookMgr::Install(ADDRESS(0x1404C6670, 0x1420DEAC0), CPlayer__UpdateController_hook, CPlayer__UpdateController_orig);

    // Addresses from AVAMain_F.pdb, verified 2026-09-15 by parsing the user's
    // own AVAMain.exe PE headers and confirming the RVA lands on a real
    // function prologue (push rdi; sub rsp,0x40; ...) in .text -- not
    // GOG/Steam-gated (that isSteam check was for a different, unrelated
    // dual-address hook and wrongly skipped this one entirely on a non-Steam
    // build in an earlier test).
    {
        MH_STATUS createStatus = MH_CreateHook((LPVOID)0x1402BDB20, (LPVOID)ConvoyDataSetWrecked_hook, (LPVOID*)&ConvoyDataSetWrecked_orig);
        if (createStatus == MH_OK) {
            MH_STATUS enableStatus = MH_EnableHook((LPVOID)0x1402BDB20);
            g_convoySetWreckedHookInstallStatus = MH_StatusToString(enableStatus);
        } else {
            g_convoySetWreckedHookInstallStatus = MH_StatusToString(createStatus);
        }
    }

#if MM_DEV_TOOLS
    // NGSONodes::SpawnStorm -- address from AVAMain_F.pdb (0x1402F0400), same
    // PDB/exe pairing already relied on for every other hook in this file.
    // No known GOG offset yet (found via a Steam-oriented PDB grep), so this
    // is installed unconditionally like ConvoyDataSetWrecked above, not via
    // the ADDRESS(gog, steam) macro.
    {
        MH_STATUS createStatus = MH_CreateHook((LPVOID)0x1402F0400, (LPVOID)SpawnStorm_hook, (LPVOID*)&SpawnStorm_orig);
        if (createStatus == MH_OK) {
            MH_STATUS enableStatus = MH_EnableHook((LPVOID)0x1402F0400);
            g_spawnStormHookInstallStatus = MH_StatusToString(enableStatus);
        } else {
            g_spawnStormHookInstallStatus = MH_StatusToString(createStatus);
        }
    }
#endif // MM_DEV_TOOLS
}