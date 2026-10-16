// ---- camp repopulation (dev, 2026-10-14) -----------------------------------------------------------------------------------------
// A liberated camp gets its enemies back for a while, without touching the game's progress (no liberation event, no collectible,
// no message: manual test 2026-10-14). Works like the convoy respawn: the game's own camp content, switched back on natively.
//   liberated camp = its enemy location (world.bin *_gameplay / *_spawn) is off (SLocationInfo +0x67 bit0 = 0) and its civilian
//   location on; the camp's CSpawnPoints are saved with m_Enabled = 0 (each switches itself off when its character dies).
//   IDLE     liberated, cooldown running (game time).
//   PENDING  cooldown over and the player far away: enemy location on, civilian location off and blocked (DisableLoading -- clearing
//            bit0 does not unload a location that is already loaded).
//   ACTIVE   player within LOAD_RANGE: the enemy location is loaded (StartLoad, as SLocationInfo::HandleEvent does -- streaming
//            alone did not pick it up), then its switched-off spawn points are switched on (CSpawnPoint::Enable) and spawned.
//   CLEARED  every revived spawn point switched itself off again (all its enemies died): the camp goes back to its liberated state
//            (enemy location off, civilians unblocked and on) and a new cooldown starts.
// Saves never keep the repopulated state: GameLocationSystemSaveStatus is hooked and writes the liberated flags for every camp that
// is not IDLE, so a save made mid-fight loads as a liberated camp (and the mod can be removed at any time).
// GOG addresses (dev build only).
static const uintptr_t A_LocStartLoad = 0x1406676D0;            // void (SLocationInfo*)
static const uintptr_t A_LocEnableLoading = 0x14066C5F0;        // GameLocationSystemInfoEnableLoading (clears the stream block)
static const uintptr_t A_LocDisableLoading = 0x140638990;       // GameLocationSystemInfoDisableLoading (sets the stream block)
static const uintptr_t A_SpawnPointSpawn = 0x14032C1A0;         // CSpawnPoint::Spawn()
static const uintptr_t A_SpawnPointEnable = 0x1403128D0;        // virtual CSpawnPoint::Enable(bool)
static const uintptr_t VT_SPAWNPOINT_GO = 0x1411F06B8;          // ??_7CSpawnPoint@@6BCGameObject@@@ (container vector entries)
typedef void (*LocFn)(void*);
typedef void (*SpawnPtEnableFn)(void*, bool);

static float g_repopCooldownMin = 1800.f, g_repopCooldownMax = 3600.f;   // game seconds
static const float REPOP_FAR = 600.f;        // the switch to PENDING happens only this far from the camp (off screen)
static const float REPOP_LOAD = 350.f;       // load the enemy location within this distance (its world.bin radius is ~400)
static const float REPOP_FORCED_FAR = 300.f; // numpad 5: the switch still waits until the player is this far (nothing changes on screen)
static const float REPOP_CIV_BACK = 400.f;   // after the camp is cleared the civilians come back when the player is this far
static const float REPOP_SPAWN = 180.f;      // the enemies are spawned only this close: the camp spawn points despawn at ~300 m, so
                                             // spawning them from farther (the enemy location streams at ~400 m) spawns them and the
                                             // game despawns them the same frame (user test 2026-10-16 wc1020: spawned at 344 m, only
                                             // the Top Dog survived). CSpawnPoint::Spawn makes a fresh instance every call, so it is
                                             // done once, when the player is inside the despawn range.

// each camp's base location (a01_sd1170 etc., always on, holds the collectibles): its own character spawn points (War Crier, plank
// and crouching guards; spawn distance <= 100) are revived too -- they are not in the enemy location (user test 2026-10-14: no war
// criers). Same order as g_camps (tools: world.bin entry whose path is the camp folder's main location).
static const unsigned g_campBase[] = { 0x180ADBF7, 0x2D040BA1, 0x83BE25A8, 0x3F83775C, 0xF9BD88FD, 0x21B85061, 0x2BE27BE1, 0xDDAD9B47,
    0x8018A9C0, 0x8008BC49, 0x6A68D9E2, 0xC02B00B7, 0xC462A6F4, 0x8BFA79BB, 0x0E59D6EB, 0xDF5C7DED, 0xD3ECCCEA, 0x03C2A561, 0x211103FC,
    0x04954B61, 0xC75D913A, 0x269F62CB, 0xEEC1F583, 0x7628D67A
#if MM_DEV_TOOLS
    , 0x12DD44F1, 0x13D3757A, 0x77A8A17F, 0x61D964F9            // warchief camps: a01_wc1050, a02_wc2040, a01_wc1020_camp, a02_wc2020
#endif
};
static_assert(sizeof g_campBase / sizeof g_campBase[0] == sizeof g_camps / sizeof g_camps[0], "g_campBase must match g_camps");
// Top Dogs never come back: killing one runs the camp's completion script again (user test 2026-10-14, sd1300 Stump Grinder: the
// game switched the enemy location off and unloaded it). Ids = the spawn points' object id (CFFF8405 in the .blo of "CSpawnPoint - <name>", what the runtime reads at +0x14).
// Warchiefs (warchief camps): 0x93C5A6FE wc1050 Gut Noose, 0xC1238E96 wc2040 Pig n Sticker, 0x897C8F67 wc1020 Gaspa Grope,
// 0x582F2735 wc2020 Cocka Locka. Test switch: <game>\scripts\repop_topdogs.txt brings them back too (g_topDogsBack); when one is
// killed the game re-runs the camp completion and switches the enemy location off -> the camp goes to CLEARED (civilians return).
static const unsigned g_topDogSpawnPoints[] = { 0x583D5194 /* sd1300 Stump Grinder */, 0xD73AB55E /* sd1210 Rim Jobbie */,
    0x93C5A6FE, 0xC1238E96, 0x897C8F67, 0x582F2735 };
static bool g_topDogsBack = false;
static bool IsTopDog(unsigned id) { if (g_topDogsBack) return false; for (unsigned t : g_topDogSpawnPoints) if (t == id) return true; return false; }

// War Criers: the tower is a CDestructibleObject "dmg_parent" (with its CLightCharacter "operator", save=0) in the camp's base
// location, created by its own CSpawnPoint (spawn distance 350, the tower in its object list B6B07728); camp completion switches that
// spawn point off (saved m_Enabled = 0), so the War Crier never comes back. During a repopulation the spawn point is revived and
// tracked like a character one. A tower brought down earlier keeps that in the save: CDestructibleObject::Load (0x140485100,
// this = CGameObject base = object + 8) copies the saved byte into m_Enabled (+0x240 bit3) / m_RespawnAsBroken (bit4) and rebuilds the
// parts; the hook clears the broken bits and rebuilds the parts again, so the tower spawns intact, and sets m_ForceSaveAsBroken (bit5)
// so saves still write it broken. Ids = CFFF8405 of the spawn point and of the tower (tools: the CDestructibleObject holding a
// warcrier skeleton, and the base CSpawnPoint listing it), camp = index in g_camps.
static const struct { int camp; unsigned spawnPoint, tower; } g_warCriers[] = { { 1, 0x31E8A897, 0xB0FA1339 }, { 2, 0xBA1430AE, 0x4B25BA9D },
    { 3, 0xC958AEDA, 0x13279EF7 }, { 4, 0xF2C5A5FD, 0x8694521B }, { 4, 0x6FA0CF50, 0x66DFF83C }, { 5, 0xC9710675, 0xFE66D83A },
    { 7, 0x6A4E7782, 0xE7E54C3C }, { 8, 0x38EB3DF1, 0xA45DA5BB }, { 8, 0x69EA28CB, 0xAB26650A }, { 9, 0x8C5B755B, 0x3B9A72E8 },
    { 10, 0x75D3B41F, 0x915845F6 }, { 11, 0x6DE89D9D, 0xF685BAE6 }, { 12, 0x9194F3EF, 0xBA7EC67D }, { 13, 0xC1C80DBA, 0x09C8D2CE },
    { 14, 0xB6D360EA, 0x96B986F1 }, { 14, 0xE438D2FA, 0xD0AA336F }, { 15, 0xF4C2BE3C, 0xAF3E94DE }, { 16, 0x1A724B1B, 0x8610B738 },
    { 17, 0x8137E92E, 0xADEDF784 }, { 19, 0x0D88A892, 0x2C2EFC29 }, { 20, 0x9190D231, 0xA49F3DB4 }, { 21, 0x0768641F, 0xFAC3A0D4 },
    { 21, 0x0348A492, 0x7B0F494E }, { 21, 0x5A2F0C58, 0x174CC12E }, { 22, 0x3A523A9D, 0x2439F962 }, { 23, 0x0A89B9BD, 0x309CA2A9 } };
static bool IsWarCrierSpawnPoint(int camp, unsigned id) { for (auto& w : g_warCriers) if (w.camp == camp && w.spawnPoint == id) return true; return false; }
static const uintptr_t A_DestructibleLoad = 0x140485100;        // CDestructibleObject::Load(CSaveContext&) (this = CGameObject base)
static const uintptr_t A_DestructibleDisableParts = 0x140447670;
static const uintptr_t A_DestructibleEnableParts = 0x140484B00;

// ---- scavenging locations (2026-10-15) ----------------------------------------------------------------------------------------
// Their logic part (*_logic, always on) holds the scavengers / gimps / defenders; each spawn point switches itself off when its
// character dies and stays off in the save. The location's own scrap (the 'optional' count) is separate pickups in the main part,
// not touched; the enemies' scrap is ordinary enemy loot (user, 2026-10-15). After the cooldown, once the player has been far away
// (LOOT_FAR), the switched-off character spawn points are switched on and spawned when he comes back within LOOT_NEAR.
#include "loot_table.inc"
static const float LOOT_FAR = 300.f, LOOT_NEAR = 160.f;
static float g_lootCd[sizeof g_loot / sizeof g_loot[0]];
static bool g_lootArmed[sizeof g_loot / sizeof g_loot[0]];
static int g_lootRevivedTotal;
static float LootDistance(const LootEntry& e) { float dx = g_PlayerPos.x - e.x, dz = g_PlayerPos.z - e.z; return sqrtf(dx * dx + dz * dz); }

enum RepopState { RP_IDLE = 0, RP_PENDING, RP_ACTIVE, RP_CLEARED };
static const char* RP_NAMES[] = { "idle", "pending", "active", "cleared" };
struct RepopCamp { int state; float cooldown; bool revived; int revivedCount; unsigned revivedIds[32]; bool forced; bool cycleStarted;
                   int baseCount; unsigned baseIds[16]; bool baseDone; float baseWait;
                   int towerCount; unsigned towerIds[4]; };
static volatile LONG g_repopSaving;   // set while the save hook has the flags flipped (the save may run on another thread)
static RepopCamp g_repop[sizeof g_camps / sizeof g_camps[0]];

// Top Dogs that come back (repop_topdogs.txt): a Top Dog's on_destroy sends global events that run the camp's completion in the
// base location again (user test 2026-10-16 sd1300: "liberated" message and the already collected history relic dropped again).
// While a camp is repopulated those events are dropped; the others (outline/loot, flame/anim, the Top Dog spawn point switching
// itself off) still go through. From the Top Dog templates' on_destroy (138B2A05) and their listeners (2026-10-16):
//   868D48E1 top_dog.camp_complete (every Top Dog camp), 400BC781 evt.objective.state_memory / smsf.objective.on (every one),
//   evt.top_dog_dead: B2F680F5 wc1020, 4B8A1508 wc1050, CD9CA9DC wc2020, 870DD82C wc2040.
// NEvent::CEventImpl::Send (0x1409C25D0): the event's sort id dwords at +8 / +0xC hold the event hash (ThunderdomeProbe).
static const uintptr_t A_EventSend = 0x1409C25D0;
typedef void (*EventSendFn)(void*, void*);
static EventSendFn EventSend_orig = nullptr;
static const unsigned g_topDogCompletion[] = { 0x868D48E1, 0x400BC781, 0xB2F680F5, 0x4B8A1508, 0xCD9CA9DC, 0x870DD82C };
static volatile LONG g_topDogBlocked = 0;
static bool AnyCampRepopulated() {
    for (auto& r : g_repop) if (r.state == RP_PENDING || r.state == RP_ACTIVE) return true;
    return false;
}
static void EventSend_hook(void* e, void* data) {
    if (g_topDogsBack && AnyCampRepopulated()) {
        unsigned a = 0, b = 0;
        __try { a = *(const unsigned*)((uint8_t*)e + 8); b = *(const unsigned*)((uint8_t*)e + 0xC); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        for (unsigned h : g_topDogCompletion)
            if (a == h || b == h) {
                if (InterlockedIncrement(&g_topDogBlocked) <= 50) LogLine("CAMPREPOP Top Dog completion event %08X dropped (camp repopulated)", h);
                return;
            }
    }
    EventSend_orig(e, data);
}
static bool g_repopInit;
static int g_repopTotal;

static bool SafeLoc(uintptr_t fn, void* l) { __try { ((LocFn)fn)(l); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static bool SafeRevive(uint8_t* sp) {
    // m_Enabled (+0x232) set directly, as the diagnostic did (CSpawnPoint::Enable is a virtual of a secondary base: its 'this' would
    // need adjusting, not worth the risk)
    __try { sp[0x232] = 1; ((LocFn)A_SpawnPointSpawn)(sp); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void SetLocBit(unsigned id, bool on) {
    uint8_t* l[4]; int n = FindLocations(id, l, 4);
    for (int i = 0; i < n; i++) { if (on) l[i][0x67] |= 1; else l[i][0x67] &= ~1; }
}
static void BlockCivilians(unsigned id, bool block) {
    uint8_t* l[4]; int n = FindLocations(id, l, 4);
    for (int i = 0; i < n; i++) SafeLoc(block ? A_LocDisableLoading : A_LocEnableLoading, l[i]);
}
static float RandCooldown() { return g_repopCooldownMin + (g_repopCooldownMax - g_repopCooldownMin) * (rand() / (float)RAND_MAX); }

// spawn points of the camp's loaded enemy location (CSpawnPoint* = vector entry - 0x10); -1 when not loaded
static int LocSpawnPoints(unsigned locId, uint8_t** out, unsigned* ids, int max) {
    uint8_t* l[4]; int n = FindLocations(locId, l, 4), k = 0;
    for (int i = 0; i < n; i++) {
        __try {
            const uint8_t* cont = *(const uint8_t* const*)(l[i] + 0x20);
            if (!cont) continue;
            const uint8_t* oc = *(const uint8_t* const*)(cont + 0x20);
            if (!oc) continue;
            const uint8_t* b = *(const uint8_t* const*)oc; const uint8_t* e = *(const uint8_t* const*)(oc + 8);
            if (!b || e < b || (e - b) / 16 > 5000) continue;
            for (const uint8_t* it = b; it != e && k < max; it += 16) {
                uint8_t* o = *(uint8_t* const*)it;
                if (o && *(const uintptr_t*)o == VT_SPAWNPOINT_GO) { out[k] = o - 0x10; ids[k] = *(const unsigned*)(o + 0x14); k++; }
            }
            return k;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }
    return -1;
}

static void TowersForCycle(int i) {
    RepopCamp& r = g_repop[i];
    r.towerCount = 0;
    for (auto& w : g_warCriers) if (w.camp == i && r.towerCount < 4) r.towerIds[r.towerCount++] = w.tower;
}

// the camp's enemies are gone: its enemy location goes off now; the civilians come back later, when the player is away (RP_CLEARED)
static void RepopRestore(int i, const char* why) {
    const CampEntry& c = g_camps[i];
    SetLocBit(c.enemyId, false);
    g_repop[i].state = RP_CLEARED; g_repop[i].revived = false; g_repop[i].revivedCount = 0; g_repop[i].forced = false; g_repop[i].cycleStarted = false;
    g_repop[i].baseCount = 0; g_repop[i].baseDone = false; g_repop[i].towerCount = 0;
    LogLine("CAMPREPOP %s: %s -> enemy location off, civilians return when the player is %.0f m away", c.name, why, REPOP_CIV_BACK);
}
static void RepopCiviliansBack(int i) {
    const CampEntry& c = g_camps[i];
    BlockCivilians(c.civilianId, false);
    SetLocBit(c.civilianId, true);
    g_repop[i].state = RP_IDLE;
    g_repop[i].cooldown = RandCooldown();
    LogLine("CAMPREPOP %s: civilians back (camp liberated again), next repopulation in %.0f min", c.name, g_repop[i].cooldown / 60.f);
}

// ---- sniper towers (2026-10-15) ---------------------------------------------------------------------------------------------------
// A sniper location (aXX_snNNNN, always on) is done once its sniper died: the character's death event sets the saved CEventRouter
// statusCheckKilled (m_State +0xE8 = true; a later death sets it again, it is never toggled back), which on every later load switches
// the spawn points off instead of spawning. Pulling the tower down (harpoon on the pylon) breaks pylon and platform (CDestructibleObject,
// saved broken) and disables their harpoon targets (saved); the game then uses the ground spawn point. Killing the sniper again does
// not touch the progress: threat_transfer.gsr (started by the same death event) exits when the location's threat resource is already
// 0 -- no threat change, no message.
// Cycle: cooldown (same as the camps) -> armed once the player is SNIPER_FAR away -> if the location loads while armed, a broken tower
// is rebuilt intact (DestructibleLoad hook; saved as broken) -> when the player is near and the location has been loaded for 3 s: if
// the sniper is dead in this game, the tower spawn point (the ground one -- the game's own fallback -- if the tower is still broken,
// i.e. the location stayed loaded: many have a 1 km stream-out radius) is switched on and spawned, and a rebuilt tower's harpoon
// targets are enabled (saves still write them disabled: TargetSave hook). User test 2026-10-15: arming beyond the stream-out radius
// (up to 1100 m) never happened in a normal session. Numpad 7 skips every cooldown; the overlay shows the nearest tower.
#include "sniper_table.inc"
static const int SN_N = sizeof g_snipers / sizeof g_snipers[0];
static const uintptr_t A_TargetEnable = 0x140317670;            // CTargetGameObject::EnableTarget()
static const uintptr_t A_TargetSave = 0x140317470;              // CTargetGameObject::Save(CSaveContext&) (CHarpoonTarget has none of its own)
struct SniperState { float cd; bool armed, towerRestored, notCleared; float loadedFor; float watch; bool back, backOnTower; int lastStatus; };
static const float SNIPER_FAR = 600.f;
static SniperState g_sn[SN_N];
static int g_snRevivedTotal;
static float SniperDistance(const SniperEntry& e) { float dx = g_PlayerPos.x - e.x, dz = g_PlayerPos.z - e.z; return sqrtf(dx * dx + dz * dz); }
static float SniperArmDistance(const SniperEntry&) { return SNIPER_FAR; }
static float SniperNear(const SniperEntry& e) { float n = e.loadR - 30.f; return n > 280.f ? 280.f : n < 150.f ? 150.f : n; }

// objects of a loaded location by runtime id (the CGameObject* the container holds; null if absent); -1 when not loaded
static int LocObjects(unsigned locId, const unsigned* want, int nWant, uint8_t** out) {
    for (int j = 0; j < nWant; j++) out[j] = nullptr;
    uint8_t* l[4]; int n = FindLocations(locId, l, 4), found = 0; bool loaded = false;
    for (int i = 0; i < n; i++) {
        __try {
            const uint8_t* cont = *(const uint8_t* const*)(l[i] + 0x20);
            if (!cont) continue;
            const uint8_t* oc = *(const uint8_t* const*)(cont + 0x20);
            if (!oc) continue;
            const uint8_t* b = *(const uint8_t* const*)oc; const uint8_t* e = *(const uint8_t* const*)(oc + 8);
            if (!b || e < b || (e - b) / 16 > 5000) continue;
            loaded = true;
            for (const uint8_t* it = b; it != e; it += 16) {
                uint8_t* o = *(uint8_t* const*)it;
                if (!o) continue;
                unsigned id = *(const unsigned*)(o + 0x14);
                for (int j = 0; j < nWant; j++) if (want[j] && want[j] == id && !out[j]) { out[j] = o; found++; }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }
    return loaded ? found : -1;
}
// a child of a game object by runtime id (CGameObject::m_Children +0x90: vector<shared_ptr<CGameObject>>); the location container
// only lists top-level objects (the towers' harpoon targets are children)
static uint8_t* ChildById(uint8_t* go, unsigned id) {
    if (!go || !id) return nullptr;
    __try {
        uint8_t* const* b = *(uint8_t* const* const*)(go + 0x90); uint8_t* const* e = *(uint8_t* const* const*)(go + 0x98);
        if (!b || e < b || e - b > 2 * 256) return nullptr;
        for (uint8_t* const* it = b; it < e; it += 2) if (*it && *(const unsigned*)(*it + 0x14) == id) return *it;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}
// a sniper spawn point is switched on the game's way: CSpawnPoint::Enable(true) (this = the container's CGameObject*, sp + 0x10)
// puts it back in the update list (a disabled spawn point leaves it), so it spawns its sniper by itself whenever the player is within
// its spawn distance and the spawn system has none of it -- also after a despawn by distance -- until the death event disables it.
// Spawn() right after gives the first one at once. SpawnSystemGetStatus(m_SpawnInstanceInfo.m_SpawnId) is logged as proof.
static const uintptr_t A_SpawnSystemGetStatus = 0x140530420;   // unsigned SpawnSystemGetStatus(const CHashString&)
static bool SafeEnableSpawnPoint(uint8_t* go) {
    __try { ((SpawnPtEnableFn)A_SpawnPointEnable)(go, true); ((LocFn)A_SpawnPointSpawn)(go - 0x10); return go[0x222] != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static int SpawnStatus(uint8_t* go) {
    __try { return (int)((unsigned (*)(const void*))A_SpawnSystemGetStatus)(go + 0x188); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static bool SafeEnableTarget(uint8_t* t) {
    __try { if (t[0x1A2]) return false; ((LocFn)A_TargetEnable)(t); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void SniperOverlayUpdate() {
    int best = -1; float bd = 1e30f;
    for (int i = 0; i < SN_N; i++) { float d = SniperDistance(g_snipers[i]); if (d < bd) { bd = d; best = i; } }
    if (best < 0) return;
    const SniperEntry& e = g_snipers[best]; const SniperState& s = g_sn[best];
    char st[96];
    if (s.back) sprintf_s(st, "SNIPER IS BACK on the %s", s.backOnTower ? "tower" : "ground");
    else if (s.notCleared) sprintf_s(st, "not cleared in this game");
    else if (s.armed) sprintf_s(st, "ready: come within %.0f m", SniperNear(e));
    else if (s.cd > 0.f) sprintf_s(st, "back in %.0f min", s.cd / 60.f + 0.5f);
    else sprintf_s(st, "go %.0f m away, then come back", SNIPER_FAR);
    char line[sizeof g_snOverlay];
    sprintf_s(line, "Nearest sniper tower: %s %.0f m at X=%.0f Z=%.0f -- %s", e.name + 4, bd, e.x, e.z, st);
    memcpy(g_snOverlay, line, sizeof line);
}

static void SniperForceAll() {
    int nFar = 0;
    for (int i = 0; i < SN_N; i++) { g_sn[i].cd = 0.f; g_sn[i].armed = false; if (SniperDistance(g_snipers[i]) > SNIPER_FAR) nFar++; }
    LogLine("SNIPEREPOP force all: cooldowns cleared, %d of %d towers are already %.0f m away (their snipers come back when you get near)", nFar, SN_N, SNIPER_FAR);
}

static float* CharacterPos(uint8_t* go) {          // the container's CGameObject*: CCharacter::GetTransform copies go + 0x1D0
    __try { float* p = (float*)(go + 0x200); return (p[0] == p[0] && p[1] > -1000.f && p[1] < 5000.f) ? p : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static void SniperWatch(const SniperEntry& e, SniperState& s, float step, float d) {
    if (d > e.loadR + 30.f) return;                         // not loaded: nothing to check
    const unsigned w[3] = { e.spMain, e.spGround, e.character };
    uint8_t* o[3];
    if (LocObjects(e.id, w, 3, o) < 0) return;
    uint8_t* sp = s.backOnTower ? o[0] : o[1];
    if (sp && *(const uintptr_t*)sp == VT_SPAWNPOINT_GO) {
        int st = SpawnStatus(sp);
        if (st != s.lastStatus) {
            LogLine("SNIPEREPOP %s: spawn point %s, spawn system status %d -> %d (player %.0f m from the tower)", e.name, sp[0x222] ? "on" : "off", s.lastStatus, st, d);
            s.lastStatus = st;
        }
    }
    if (s.watch > 0.f) {
        s.watch -= step;
        float* p = o[2] ? CharacterPos(o[2]) : nullptr;
        if (p) { LogLine("SNIPEREPOP %s: sniper character %08X found at %.1f %.1f %.1f (player %.0f m from the tower)", e.name, e.character, p[0], p[1], p[2], d); s.watch = 0.f; }
    }
    if (s.back) {
        bool on = false;
        for (int k = 0; k < 2; k++) if (o[k] && *(const uintptr_t*)o[k] == VT_SPAWNPOINT_GO && o[k][0x232 - 0x10]) on = true;
        if (!on) { s.back = false; LogLine("SNIPEREPOP %s: spawn points off: the revived sniper died (or the location reloaded); next in %.0f min at the earliest", e.name, s.cd / 60.f); }
    }
}

static void SniperTick(float step) {
    SniperOverlayUpdate();
    for (int i = 0; i < SN_N; i++) {
        const SniperEntry& e = g_snipers[i]; SniperState& s = g_sn[i];
        float d = SniperDistance(e);
        if (s.watch > 0.f || s.back) SniperWatch(e, s, step, d);
        if (!s.armed) {
            if (s.cd > 0.f) { s.cd -= step; continue; }
            if (d > SniperArmDistance(e)) { s.armed = true; s.towerRestored = false; s.loadedFor = 0.f; }
            continue;
        }
        if (d > e.loadR + 30.f) { s.loadedFor = 0.f; continue; }   // cannot be loaded yet
        const unsigned want[5] = { e.killed, e.spMain, e.spGround, e.platform, e.pylon };
        uint8_t* o[5];
        if (LocObjects(e.id, want, 5, o) < 0 || !o[0]) { s.loadedFor = 0.f; continue; }   // not loaded (or only its far part)
        s.loadedFor += step;
        if (d > SniperNear(e) || s.loadedFor < 3.f) continue;      // after the location's own creation events
        s.armed = false; s.cd = RandCooldown();
        if (!o[0][0xE8]) {
            s.notCleared = true;
            LogLine("SNIPEREPOP %s: the sniper is still alive in the game, nothing to do; next in %.0f min", e.name, s.cd / 60.f);
            continue;
        }
        s.notCleared = false;
        bool towerDown = o[3] && ((o[3] - 8)[0x240] & 1);
        uint8_t* sp = (!towerDown && o[1]) ? o[1] : o[2];
        if (!sp || *(const uintptr_t*)sp != VT_SPAWNPOINT_GO) { LogLine("SNIPEREPOP %s: spawn point not found", e.name); continue; }
        bool busy = false;
        for (int k = 1; k <= 2; k++) if (o[k] && *(const uintptr_t*)o[k] == VT_SPAWNPOINT_GO && o[k][0x232 - 0x10]) busy = true;
        if (busy) { LogLine("SNIPEREPOP %s: the last revived sniper is still alive (spawn point on), nothing to do; next in %.0f min", e.name, s.cd / 60.f); continue; }
        LogLine("SNIPEREPOP %s: %s spawn point %08X spawn distance %.0f, despawn distance %.0f", e.name, sp == o[1] ? "tower" : "ground",
            *(const unsigned*)(sp + 0x14), *(const float*)(sp + 0x158), *(const float*)(sp + 0x15C));
        bool ok = SafeEnableSpawnPoint(sp);
        s.lastStatus = -2;
        int targets = 0, tFound = 0, tOn = 0;
        uint8_t* tg[2] = { ChildById(o[4], e.pylonTarget), ChildById(o[3], e.platformTarget) };
        for (uint8_t* t : tg) {
            if (!t) continue;
            tFound++;
            if (s.towerRestored && SafeEnableTarget(t)) targets++; else if (t[0x1A2]) tOn++;
        }
        if (ok) { g_snRevivedTotal++; s.watch = 30.f; s.back = true; s.backOnTower = sp == o[1]; }
        LogLine("SNIPEREPOP %s: player %.0f m away, sniper %s on the %s%s; harpoon targets %d found, %d were on, %d enabled; next in %.0f min",
            e.name, d, ok ? "spawned" : "FAILED", sp == o[1] ? "tower" : "ground", s.towerRestored ? " (tower rebuilt)" : towerDown ? " (tower down)" : "",
            tFound, tOn, targets, s.cd / 60.f);
    }
}

static void LootTick(float step) {
    const int NL = sizeof g_loot / sizeof g_loot[0];
    for (int i = 0; i < NL; i++) {
        const LootEntry& e = g_loot[i];
        float d = LootDistance(e);
        if (!g_lootArmed[i]) {
            if (g_lootCd[i] > 0.f) { g_lootCd[i] -= step; continue; }
            if (d > LOOT_FAR) g_lootArmed[i] = true;           // the player has been away: the next visit gets enemies
            continue;
        }
        if (d > LOOT_NEAR) continue;
        uint8_t* sp[32]; unsigned ids[32];
        int n = LocSpawnPoints(e.id, sp, ids, 32);
        if (n < 0) continue;                                   // logic part not loaded yet
        int off = 0, ok = 0;
        for (int k = 0; k < n; k++) {
            if (*(float*)(sp[k] + 0x168) > 100.f || sp[k][0x232] || IsTopDog(ids[k])) continue;   // characters that died only
            off++;
            if (SafeRevive(sp[k])) ok++;
        }
        g_lootArmed[i] = false; g_lootCd[i] = RandCooldown(); g_lootRevivedTotal += ok;
        if (off) LogLine("LOOTREPOP %s: player %.0f m away, %d spawn points, %d were off, %d revived; next in %.0f min", e.name, d, n, off, ok, g_lootCd[i] / 60.f);
    }
}

static void RepopTick(float dt) {
    static float acc; acc += dt;
    if (acc < 1.f || g_repopSaving) return;
    float step = acc; acc = 0.f;
    const int N = sizeof g_camps / sizeof g_camps[0];
    if (!g_repopInit) {
        int el; bool any = false;
        for (int i = 0; i < N; i++) if (LocEnabled(g_camps[i].enemyId, &el) != -1) any = true;
        if (!any) return;                                       // location system not ready yet
        g_repopInit = true;
        int lib = 0;
        for (int i = 0; i < N; i++) {
            g_repop[i].state = RP_IDLE; g_repop[i].cooldown = RandCooldown();
            if (LocEnabled(g_camps[i].enemyId, &el) == 0) lib++;
        }
        LogLine("CAMPREPOP ready: %d of %d table camps liberated, cooldown %.0f-%.0f min", lib, N, g_repopCooldownMin / 60.f, g_repopCooldownMax / 60.f);
        for (int i = 0; i < (int)(sizeof g_loot / sizeof g_loot[0]); i++) { g_lootCd[i] = RandCooldown(); g_lootArmed[i] = false; }
        LogLine("LOOTREPOP ready: %d scavenging locations with enemies", (int)(sizeof g_loot / sizeof g_loot[0]));
        for (int i = 0; i < SN_N; i++) { g_sn[i].cd = RandCooldown(); g_sn[i].armed = false; }
        LogLine("SNIPEREPOP ready: %d sniper locations", SN_N);
    }
    LootTick(step);
    SniperTick(step);
    for (int i = 0; i < N; i++) {
        const CampEntry& c = g_camps[i]; RepopCamp& r = g_repop[i];
        int el, cl; int en = LocEnabled(c.enemyId, &el), cv = LocEnabled(c.civilianId, &cl);
        float d = CampDistance(c);
        switch (r.state) {
        case RP_IDLE:
            if (en != 0) break;                                  // not liberated (or not in this game state)
            if (!r.forced) { r.cooldown -= step; if (r.cooldown > 0.f) break; }
            if (d < (r.forced ? REPOP_FORCED_FAR : REPOP_FAR)) break;   // wait until the player is far away (nothing changes on screen)
            SetLocBit(c.enemyId, true);
            SetLocBit(c.civilianId, false);
            BlockCivilians(c.civilianId, true);
            TowersForCycle(i);
            r.state = RP_PENDING; r.revived = false; r.revivedCount = 0; r.cycleStarted = false;
            LogLine("CAMPREPOP %s: repopulating (player %.0f m away%s)", c.name, d, r.forced ? ", forced" : "");
            break;
        case RP_PENDING:
            if (en == 0) { RepopRestore(i, "enemy location switched off by the game"); break; }
            if (d > REPOP_LOAD) break;
            {
                uint8_t* l[4]; int n = FindLocations(c.enemyId, l, 4), started = 0;
                for (int k = 0; k < n; k++)
                    if (!(l[k][0x67] & 4) && *(void**)(l[k] + 0x20) == nullptr && SafeLoc(A_LocStartLoad, l[k])) started++;
                r.state = RP_ACTIVE;
                LogLine("CAMPREPOP %s: player %.0f m away, loading the enemy location (%d StartLoad)", c.name, d, started);
            }
            break;
        case RP_ACTIVE: {
            // switched off by the game (save loaded, or a camp script) -> the civilians must come back (user test 2026-10-14:
            // sd1300 stayed empty after the reset left them blocked)
            if (en == 0) { RepopRestore(i, "enemy location switched off by the game"); break; }
            uint8_t* sp[32]; unsigned ids[32];
            int n = LocSpawnPoints(c.enemyId, sp, ids, 32);
            if (n < 0) {                                         // not loaded (yet, or unloaded because the player left)
                if (d > REPOP_LOAD + 150.f && r.revived) { r.revived = false; r.cycleStarted = false; r.baseDone = false; LogLine("CAMPREPOP %s: player left, enemies will respawn on the next approach", c.name); r.state = RP_PENDING; }
                break;
            }
            if (!r.cycleStarted) {
                if (d > REPOP_SPAWN) break;                       // wait until the player is close enough that the enemies will not despawn at once
                // once per repopulation. Only character spawn points (spawn distance <= 100 m, the camp's spp.*) are tracked: the
                // others (370 m) switch themselves off right after spawning, which is not a death. Every character spawn point of the camp is switched on and spawned, and all of them are tracked: some were left
                // on by earlier cycles or by the save and spawn enemies too (user test 2026-10-14: tracking only the revived ones
                // declared the camp cleared with enemies alive); the others are revived but not tracked
                int off = 0, ok = 0;
                r.revivedCount = 0;
                for (int k = 0; k < n; k++) {
                    bool character = *(float*)(sp[k] + 0x168) <= 100.f;
                    bool wasOff = !sp[k][0x232];
                    if (wasOff) off++;
                    if (!wasOff && !character) continue;
                    if (IsTopDog(ids[k])) { LogLine("CAMPREPOP %s: Top Dog spawn point %08X left off", c.name, ids[k]); continue; }
                    if (SafeRevive(sp[k])) {
                        ok++;
                        if (character && r.revivedCount < 32) r.revivedIds[r.revivedCount++] = ids[k];
                    }
                }
                r.revived = true; r.cycleStarted = true; r.baseCount = 0; r.baseDone = false; r.baseWait = 0.f;
                g_repopTotal++;
                LogLine("CAMPREPOP %s: %d spawn points, %d were off, %d spawned, %d character spawn points tracked", c.name, n, off, ok, r.revivedCount);
                if (!r.revivedCount) RepopRestore(i, "nothing to revive");
                break;
            }
            if (!r.revived) { r.revived = true; LogLine("CAMPREPOP %s: back in range, %d tracked spawn points", c.name, r.revivedCount); }
            // the base location's own characters (War Crier etc.): once its container is there (it streams on its own; given up after 60 s)
            uint8_t* bsp[48]; unsigned bids[48];
            int bn = LocSpawnPoints(g_campBase[i], bsp, bids, 48);
            if (!r.baseDone) {
                r.baseWait += step;
                if (bn >= 0) {
                    r.baseDone = true;
                    int boff = 0;
                    for (int k = 0; k < bn; k++) {
                        bool warCrier = IsWarCrierSpawnPoint(i, bids[k]);
                        if ((*(float*)(bsp[k] + 0x168) > 100.f && !warCrier) || bsp[k][0x232]) continue;   // characters and War Criers; ones already on are left alone
                        if (IsTopDog(bids[k])) continue;
                        boff++;
                        if (SafeRevive(bsp[k]) && r.baseCount < 16) { r.baseIds[r.baseCount++] = bids[k]; LogLine("CAMPREPOP %s: base spawn point %08X revived%s", c.name, bids[k], warCrier ? " (War Crier)" : ""); }
                    }
                    LogLine("CAMPREPOP %s: base location %d spawn points, %d character/War Crier ones were off, %d revived and tracked", c.name, bn, boff, r.baseCount);
                } else if (r.baseWait > 60.f) { r.baseDone = true; LogLine("CAMPREPOP %s: base location not loaded, its characters skipped", c.name); }
            }
            int alive = 0;                                       // revived spawn points still on = their enemies not dead yet
            for (int k = 0; k < n; k++)
                for (int j = 0; j < r.revivedCount; j++) if (ids[k] == r.revivedIds[j] && sp[k][0x232]) alive++;
            for (int k = 0; k < bn; k++)
                for (int j = 0; j < r.baseCount; j++) if (bids[k] == r.baseIds[j] && bsp[k][0x232]) alive++;
            if (!alive) RepopRestore(i, "all enemies defeated");
            break;
        }
        case RP_CLEARED:
            if (d > REPOP_CIV_BACK) RepopCiviliansBack(i);
            break;
        default: break;
        }
    }
}

// numpad 5: the nearest liberated camp repopulates now (no cooldown, no distance gate); numpad 4: state of every camp
static void RepopForceNearest() {
    const int N = sizeof g_camps / sizeof g_camps[0];
    int best = -1; float bd = 1e30f; int el;
    for (int i = 0; i < N; i++)
        if (g_repop[i].state == RP_IDLE && LocEnabled(g_camps[i].enemyId, &el) == 0 && CampDistance(g_camps[i]) < bd) { bd = CampDistance(g_camps[i]); best = i; }
    int bestLoot = -1; float bl = 1e30f;
    for (int i = 0; i < (int)(sizeof g_loot / sizeof g_loot[0]); i++)
        if (LootDistance(g_loot[i]) < bl) { bl = LootDistance(g_loot[i]); bestLoot = i; }
    int bestSn = -1; float bs = 1e30f;
    for (int i = 0; i < SN_N; i++)
        if (SniperDistance(g_snipers[i]) < bs) { bs = SniperDistance(g_snipers[i]); bestSn = i; }
    if (bestSn >= 0 && bs < bd && bs < bl) {                     // a sniper tower is nearest: no cooldown, the player must still leave
        g_sn[bestSn].cd = 0.f; g_sn[bestSn].armed = false;       // beyond its stream-out radius and come back
        LogLine("SNIPEREPOP force: %s (%.0f m) -- the sniper comes back after you go %.0f m away and return", g_snipers[bestSn].name, bs,
            SniperArmDistance(g_snipers[bestSn]));
        return;
    }
    if (bestLoot >= 0 && bl < bd) {                            // a scavenging location is nearer: no cooldown, but the player must
        g_lootCd[bestLoot] = 0.f; g_lootArmed[bestLoot] = false;   // still leave (> LOOT_FAR) and come back (user 2026-10-15)
        LogLine("LOOTREPOP force: %s (%.0f m) -- enemies come back after you go %.0f m away and return", g_loot[bestLoot].name, bl, LOOT_FAR);
        return;
    }
    if (best < 0) { LogLine("CAMPREPOP force: no liberated idle camp"); return; }
    g_repop[best].forced = true;
    LogLine("CAMPREPOP force: %s (%.0f m)", g_camps[best].name, bd);
}
static void RepopLog() {
    const int N = sizeof g_camps / sizeof g_camps[0];
    for (int i = 0; i < N; i++) {
        int el, cl; int en = LocEnabled(g_camps[i].enemyId, &el), cv = LocEnabled(g_camps[i].civilianId, &cl);
        LogLine("CAMPREPOP %-24s %-8s enemies %d civilians %d cooldown %.0f min %.0f m", g_camps[i].name, RP_NAMES[g_repop[i].state], en, cv,
            g_repop[i].cooldown / 60.f, CampDistance(g_camps[i]));
    }
    int armed = 0;
    for (int i = 0; i < (int)(sizeof g_loot / sizeof g_loot[0]); i++) armed += g_lootArmed[i];
    LogLine("LOOTREPOP %d scavenging locations, %d armed, %d enemies revived this session", (int)(sizeof g_loot / sizeof g_loot[0]), armed, g_lootRevivedTotal);
    int snArmed = 0, snCd = 0;
    for (int i = 0; i < SN_N; i++) { snArmed += g_sn[i].armed; snCd += !g_sn[i].armed && g_sn[i].cd > 0.f; }
    LogLine("SNIPEREPOP %d sniper locations, %d armed, %d cooling down, %d snipers revived this session", SN_N, snArmed, snCd, g_snRevivedTotal);
}

typedef void (*DestructibleFn)(void*);
static bool SafeParts(uint8_t* obj) {
    __try { ((DestructibleFn)A_DestructibleDisableParts)(obj); ((DestructibleFn)A_DestructibleEnableParts)(obj); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
DEFHOOK(void, DestructibleLoad, (uint8_t* go, void* ctx)) {
    DestructibleLoad_orig(go, ctx);
    if (!g_repopInit) return;
    unsigned id = 0;
    __try { id = *(const unsigned*)(go + 0x14); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    const int N = sizeof g_camps / sizeof g_camps[0];
    for (int i = 0; i < N; i++) {
        RepopCamp& r = g_repop[i];
        if (r.state != RP_PENDING && r.state != RP_ACTIVE) continue;
        for (int j = 0; j < r.towerCount; j++) {
            if (r.towerIds[j] != id) continue;
            uint8_t* obj = go - 8;
            uint8_t before = obj[0x240];
            obj[0x240] = (uint8_t)((before & ~(0x01 | 0x10)) | 0x08 | 0x20);   // not broken, enabled, saved as broken
            bool ok = SafeParts(obj);
            LogLine("CAMPREPOP %s: War Crier tower %08X loaded intact (flags %02X -> %02X)%s", g_camps[i].name, id, before, obj[0x240], ok ? "" : " FAULTED");
            return;
        }
    }
    for (int i = 0; i < SN_N; i++) {
        const SniperEntry& e = g_snipers[i];
        if (!g_sn[i].armed || !id || (id != e.platform && id != e.pylon)) continue;
        uint8_t* obj = go - 8;
        uint8_t before = obj[0x240];
        if (!(before & (0x01 | 0x10))) return;                  // intact: nothing to rebuild
        obj[0x240] = (uint8_t)((before & ~(0x01 | 0x10)) | 0x08 | 0x20);   // not broken, enabled, saved as broken
        bool ok = SafeParts(obj);
        g_sn[i].towerRestored = true;
        LogLine("SNIPEREPOP %s: tower %s %08X loaded intact (flags %02X -> %02X)%s", e.name, id == e.platform ? "platform" : "pylon", id, before,
            obj[0x240], ok ? "" : " FAULTED");
        return;
    }
}

// a rebuilt sniper tower's harpoon targets are saved disabled, as the tower is saved broken
DEFHOOK(void, TargetSave, (uint8_t* t, void* ctx)) {
    unsigned id = 0; bool flip = false;
    __try { id = *(const unsigned*)(t + 0x14); } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
    if (id && g_repopInit)
        for (int i = 0; i < SN_N && !flip; i++)
            if (g_sn[i].towerRestored && (id == g_snipers[i].pylonTarget || id == g_snipers[i].platformTarget) && t[0x1A2]) flip = true;
    if (!flip) { TargetSave_orig(t, ctx); return; }
    t[0x1A2] = 0;
    TargetSave_orig(t, ctx);
    t[0x1A2] = 1;
}

// saves keep every repopulated camp liberated
DEFHOOK(void, LocSaveStatus, (void* save)) {
    const int N = sizeof g_camps / sizeof g_camps[0];
    bool changed[sizeof g_camps / sizeof g_camps[0]] = {};
    int n = 0;
    InterlockedExchange(&g_repopSaving, 1);
    bool enemyWas[sizeof g_camps / sizeof g_camps[0]] = {}, civWas[sizeof g_camps / sizeof g_camps[0]] = {};
    for (int i = 0; i < N; i++) if (g_repopInit && g_repop[i].state != RP_IDLE) {
        int el, cl;
        enemyWas[i] = LocEnabled(g_camps[i].enemyId, &el) > 0; civWas[i] = LocEnabled(g_camps[i].civilianId, &cl) > 0;
        SetLocBit(g_camps[i].enemyId, false); SetLocBit(g_camps[i].civilianId, true); changed[i] = true; n++;
    }
    // the base location is saved as it is: the characters the mod revived there are written switched off (the liberated state)
    uint8_t* bOn[64]; int nb = 0;
    for (int i = 0; i < N; i++) if (g_repopInit && g_repop[i].baseCount) {
        uint8_t* bsp[48]; unsigned bids[48];
        int bn = LocSpawnPoints(g_campBase[i], bsp, bids, 48);
        for (int k = 0; k < bn; k++)
            for (int j = 0; j < g_repop[i].baseCount; j++)
                if (bids[k] == g_repop[i].baseIds[j] && bsp[k][0x232] && nb < 64) { bsp[k][0x232] = 0; bOn[nb++] = bsp[k]; }
    }
    LocSaveStatus_orig(save);
    for (int k = 0; k < nb; k++) bOn[k][0x232] = 1;
    for (int i = 0; i < N; i++) if (changed[i]) { SetLocBit(g_camps[i].enemyId, enemyWas[i]); SetLocBit(g_camps[i].civilianId, civWas[i]); }
    InterlockedExchange(&g_repopSaving, 0);
    if (n) LogLine("CAMPREPOP save: %d repopulated camp(s) written as liberated", n);
}
