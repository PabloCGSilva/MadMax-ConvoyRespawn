// ---- camp repopulation (dev, 2026-10-14) -----------------------------------------------------------------------------------------
// A liberated camp gets its enemies back for a while, without touching the game's progress (no liberation event, no collectible,
// no message: manual test 2026-10-14). Works like the convoy respawn: the game's own camp content, switched back on natively.
//   liberated camp = its enemy location (world.bin *_gameplay / *_spawn) is off (SLocationInfo +0x67 bit0 = 0) and its civilian
//   location on; the camp's CSpawnPoints are saved with m_Enabled = 0 (each switches itself off when its character dies).
//   IDLE     liberated. One clock for the whole map picks the next camp to fall (RepopAttack; game time); a retaken camp is safe for a while first.
//   PENDING  picked, with the player far away: enemy location on, civilian location off and blocked (DisableLoading -- clearing
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
static float g_repopAttackMin = 1200.f, g_repopAttackMax = 2400.f;       // game seconds between two attacks anywhere on the map
static const int REPOP_MAX_TAKEN = 3;         // camps held by the enemy at once: the clock stops here, or all 24 fall within the hour (user 2026-11-06)
static const float REPOP_FAR = 600.f;        // the switch to PENDING happens only this far from the camp (off screen)
static const float REPOP_LOAD = 350.f;       // load the enemy location within this distance (its world.bin radius is ~400)
static const float REPOP_FORCED_FAR = 300.f; // numpad 5: the switch still waits until the player is this far (nothing changes on screen)
static const float REPOP_CIV_BACK = 400.f;   // after the camp is cleared the civilians come back when the player is this far
static const float REPOP_POINT = 120.f;      // a character spawn point is spawned when the player is this close to IT (the game spawns
                                             // these at 50 m on its own; all 20 at once overloaded the spawn budget: corpses vanished at
                                             // once and some enemies never appeared -- user test 2026-10-16, release build)
static const int REPOP_MAX_LIVE = 10;        // at most this many tracked enemies alive at once, the next ones spawn as these die. REPOP_POINT alone does not
                                             // stagger: 120 m covers a whole camp (user test 2026-11-05 sd1270: 20 points in 3 s; right after the last spawn
                                             // the game despawned 6-7 live enemies at once and the first 5-6 deaths of each round lost their bodies in ~6 s)
static const float REPOP_REQUEST_WAIT = 5.f; // a requested spawn counts as alive this long before it shows in the spawn system
static const float REPOP_NO_SPAWN = 20.f;    // a spawned point with no entity this long: asked again if it had one, ignored if it never did
static const float REPOP_TRIGGER_WAIT = 20.f; // a point left to its own trigger, the last one alive: spawned by the mod after the player stays this long within REPOP_POINT
                                             // (sd1270 point A9E5B926 never spawns and stays on: the camp could never be cleared)
static const float REPOP_LOST_RANGE = 200.f;  // a short life only counts this close to the point: farther the game despawns a healthy enemy as the player drives by (300 m, 10 points of sd1270 in the r8 test)
static const float REPOP_LOOKOUT_FAR = 200.f; // a lookout point (370 m kind) farther than this from the camp centre is left to the game, not tracked (r43b autotest
                                             // 2026-11-18: Black Cove's DF0DDFA9/41C6EC9B at 266/312 m never spawned, Overlook's D7B8EED9 at 228 m died out of
                                             // REPOP_LOST_RANGE: both camps stayed taken with every enemy dead; the 24 older camps have none past 158 m)
static const int REPOP_MAX_LOST = 2;        // a point whose enemy vanished alive this many times, each within REPOP_NO_SPAWN of appearing, is switched off until the next repopulation
                                             // (user test 2026-11-06: with the raised budget 5 points of sd1270/sd1290 gave enemies that lived 3-13 s, asked again without end)
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
    0x04954B61, 0xC75D913A, 0x269F62CB, 0xEEC1F583, 0x7628D67A,
    0x26FDD987, 0xFC6A241B, 0xC500C861, 0x2C2DE2B6, 0xF8046095, 0x7565F5EB, 0x7F694C70   // r43: vv1050_camp, fo2040, fo2020, vv2020, vv1110, vv1060, vv1080
    , 0x12DD44F1, 0x13D3757A, 0x77A8A17F, 0x61D964F9            // warchief camps: a01_wc1050, a02_wc2040, a01_wc1020_camp, a02_wc2020 (r47: release too)
};
static_assert(sizeof g_campBase / sizeof g_campBase[0] == sizeof g_camps / sizeof g_camps[0], "g_campBase must match g_camps");
// each camp's SLocationInfoEntry key (+0x30): global/location_info, the enemy camp entry nearest to the camp (1-105 m, next one 78 m or more away). Same order as g_camps.
static const unsigned g_campLocKey[] = { 0x2CE4BEDC, 0x95976E7E, 0x1E87AF5B, 0x2DBB8962, 0x2364BD26, 0x03B743E8, 0xEE19845B, 0x634385C1,
    0x7E96D6EF, 0xD9FF1A87, 0x9E91F414, 0xFB392F66, 0xDB279FE8, 0x7544C6BD, 0xADEF4EF9, 0xE57A49A2, 0xB350D06A, 0xA4E3AA6B, 0x3A374C69,
    0x525AE42F, 0x7BE7AA6A, 0x8F1B437F, 0xD65FD680, 0xB9A15A77,
    // r43: Overlook (107 m), Skull Top, Havoc Point, Twin Sisters, Scorched Sand, Jugular Canyon (96 m: the nearer entry, 62 m, is
    // Debris Site, type 9, no camp), Black Cove
    0xB8BA74D8, 0xFC107996, 0x6A92820A, 0x83D3B23F, 0x3CAD37C2, 0x401765F4, 0x3B66AA1A
    , 0x25B1896C, 0xE2DC276D, 0xBABF660A, 0xFBCB4761            // warchief camps (r47: release too)
};
static_assert(sizeof g_campLocKey / sizeof g_campLocKey[0] == sizeof g_camps / sizeof g_camps[0], "g_campLocKey must match g_camps");

// Spawn distance <= 100 does not mean a character: of the 117 such base points of the 24 camps 12 create characters, the rest are relics and project crates (25, one pickup opens the
// camp completion screen: user test 2026-11-06), items and destructibles. Only enemies come back, so each kind is read from the data (tools/repop/build_base_chars.py).
#include "base_char_table.inc"
#include "camp_notice.inc"
template <size_t N> static bool InPoints(const unsigned (&t)[N], unsigned id) {
    for (unsigned v : t) if (v == id) return true;
    return false;
}

// Points whose enemy vanishes alive 3-13 s after spawning, in every test since r5 and at any distance (<= 98 m in the r8 test): they stay off, or the player sees enemies disappear. Each has a second
// event in 214B0328 and, except 0956BD6D, that event starts aic.cinematic (cinematic_use_interactions.bts) on the character: inferred cause, not proven. Six more points of other camps have a second event
// with no such link and were never tested, so they are not listed: REPOP_MAX_LOST deals with them if they do the same.
static const unsigned g_vanishPoints[] = { 0xDAD48D15, 0x12A50091, 0x0956BD6D, 0xFA08D0B3, 0xA9E5B926 };

// Points the game spawns on their own spawn_event (7A7D0F4B): an ambush, the enemy appears when the player walks into a trigger. Spawned earlier by this mod, 3B9F5107 of sd2090 (a gimp on a perch, sent by
// sht.a02_sd2090.:flamepipe.on, which also lights the flame pipe) sat on its perch for good: user tests 2026-11-07, twice. These stay on and counted, but only the game spawns them. One point for now (test)
static const unsigned g_triggerPoints[] = { 0x3B9F5107 };

// r18: its jump edges were enabled and traversable, so the AI never chooses to come down. r19: moved down to the start of its jump, it was back on the perch node 2 s later.
// What the AI runs for it, next to the gimp of the same camp that does leap (688D7B6A): its behaviour tree instances and its entity blackboard
static const unsigned g_aiProbePoints[] = { 0x3B9F5107, 0x688D7B6A };

// every objective of the camp done (panel all ticked): a loot crate still on there was left on by an older build of this mod, not by the player
static const uintptr_t A_LocationInfoTable = 0x14171B000;   // SLocationInfoTable*: +0 entries, +8 count
static bool CampItemsDone(int i) {
    __try {
        const uint8_t* t = *(const uint8_t* const*)A_LocationInfoTable;
        if (!t) return false;
        const uint8_t* e = *(const uint8_t* const*)t; unsigned count = *(const unsigned*)(t + 8);
        for (unsigned n = 0; e && n < count && n < 1024; n++, e += 0x60) {
            if (*(const unsigned*)(e + 0x30) != g_campLocKey[i]) continue;
            for (int k = 0; k < 11; k++) if (e[0x35 + 2 * k]) return false;
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
// Top Dogs never come back: killing one runs the camp's completion script again (user test 2026-10-14, sd1300 Stump Grinder: the
// game switched the enemy location off and unloaded it). Ids = the spawn points' object id (CFFF8405 in the .blo of "CSpawnPoint - <name>", what the runtime reads at +0x14).
// Warchiefs (warchief camps): 0x93C5A6FE wc1050 Gut Noose, 0xC1238E96 wc2040 Pig n Sticker, 0x897C8F67 wc1020 Gaspa Grope,
// 0x582F2735 wc2020 Cocka Locka. Test switch: <game>\scripts\repop_topdogs.txt brings them back too (g_topDogsBack); when one is
// killed the game re-runs the camp completion and switches the enemy location off -> the camp goes to CLEARED (civilians return).
// r46: Rim Jobbie was 0xD73AB55E, an id no spawn point has (user test 2026-11-19: he was counted as a plain enemy, D73B025E)
// r47: the four warchief ids above were wrong the same way; the .blo spawn points are 93C567FE, C1237D96, 897CCB67, 582F4035
static const unsigned g_topDogSpawnPoints[] = { 0x583D5194 /* sd1300 Stump Grinder */, 0xD73B025E /* sd1210 Rim Jobbie */,
    0x93C567FE /* wc1050 Gut Noose */, 0xC1237D96 /* wc2040 Pig n Sticker */, 0x897CCB67 /* wc1020 Gaspa Grope */, 0x582F4035 /* wc2020 Cocka Locka */ };
// r44 (user 2026-11-18: "sim os top dogs devem voltar. apenas cuidar para que a morte deles não conte como um trigger para a retomada"):
// they come back by default; their death is one more kill of the mod's count, and its completion events are dropped (EventSend_hook)
static bool g_topDogsBack = true;
static bool TopDogPoint(unsigned id) { for (unsigned t : g_topDogSpawnPoints) if (t == id) return true; return false; }
static bool IsTopDog(unsigned id) { return !g_topDogsBack && TopDogPoint(id); }

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
// r47: up to REPOP_TRACK spawn points per camp (Proving Grounds a01_wc1050_gameplay has 45, 40 of them characters; 32 dropped Gut Noose's)
static const int REPOP_TRACK = 64;
struct RepopCamp { int state; float cooldown; bool revived; int revivedCount; unsigned revivedIds[REPOP_TRACK]; bool forced; bool cycleStarted;
                   int baseCount; unsigned baseIds[16]; bool baseDone; float baseWait;
                   float baseNoLive[16]; uint8_t baseLost[16]; unsigned baseIgnored;   // per base guard: seconds with no enemy / asked again / not counted
                   int towerCount; unsigned towerIds[16]; uint64_t spawnedMask;   // lookout tower parts (diagnostic)
                   uint64_t liveMask; uint64_t ignoredMask; float noLive[REPOP_TRACK];   // per tracked point: had an entity / never spawns / seconds with no entity
                   float liveTime[REPOP_TRACK]; uint8_t lost[REPOP_TRACK];                        // per tracked point: seconds its current entity has been there / short lives so far
                   int deadCount; unsigned deadIds[80]; bool armed;             // killed in this repopulation (kept when the player leaves and returns); 370 m points done
                   uint64_t lookoutMask;                                        // tracked points that are lookouts (spawn distance over 100 m): the game spawns them, see SpawnEntityChars
                   unsigned topDog; };                                          // r44: the camp's Top Dog spawn point, tracked like the others (0: none)
static volatile LONG g_repopSaving;   // set while the save hook has the flags flipped (the save may run on another thread)
static RepopCamp g_repop[sizeof g_camps / sizeof g_camps[0]];
static bool RepopIsDead(const RepopCamp& r, unsigned id) {
    for (int i = 0; i < r.deadCount; i++) if (r.deadIds[i] == id) return true;
    return false;
}
// r46 (user test 2026-11-19, Large Blade: "ao derrotar o top dog, as manivelas para desligar o fogo da arena não ficaram disponíveis,
// então fiquei preso"): the Top Dog intro (a01_sd1210_spawn evt.skipped / its sequence) lights the arena's two flame pipes; the valves
// (evt.action) put them out with a01_sd1210.flame_pipe_{left,right}.off (A980C7AF / D6D84B89, Jenkins confirmed), which the base
// location's EventTrigger_FlameOff objects listen to. SendMsg turns each name into SEventID(hash, 0), the same id the valves send.
// r48 (user 2026-11-20, after the r47 test where Gasworks' valves were usable: "1 - mas pode diminuir o tempo para 1 min"): the valves
// (max.interaction A59513C8, entry.usable) are made usable by the base's evt.top_dog_dead, which listens to BAA9AE93 (scope 0). Rim
// Jobbie's death does not send it (his on-death list: D49D0730 868D48E1 400BC781 E321E903) and no sd1210 file does: it most likely comes
// after the camp completion the mod blocks (Gaspa Grope sends his camp's B2F680F5 himself). The mod sends BAA9AE93 when the Top Dog
// dies and puts the flames out the r46 way when no valve was turned within ARENA_FALLBACK game seconds.
static const struct { const char* camp; unsigned valvesOn; unsigned valveOff[2]; const char* flamesOff; } g_topDogArena[] = {
    { "sd1210", 0xBAA9AE93, { 0xA980C7AF, 0xD6D84B89 }, "a01_sd1210.flame_pipe_left.off a01_sd1210.flame_pipe_right.off" },
};
static const float ARENA_FALLBACK = 60.f;
static struct { int camp; int arena; float left; } g_arenaWait = { -1, -1, 0.f };
static volatile LONG g_valvesTurned;   // bit k: valveOff[k] seen by EventSend_hook since the valves were made usable
static volatile LONG g_arenaOwnSend;   // set while the mod sends an arena event itself (EventSend_hook sees those too)
static const uintptr_t A_SendEventMsg = 0x140007CA0;   // NEvent::CSendEvent<...>::SendMsg(const char*)
// r47 (user 2026-11-19: "pode fazer o r47"): a warchief camp's arena gate opens from the base location's 04_camp_completed (CEventTrigger,
// listens to 87545B6C, sent at the end of the camp completion that the warchief's death starts: user test 2026-10-16 wc1020, the filter
// dropped it and the gates stayed shut). Its three global events, the camp's own: open the gate, enable 05_open_gates_trigger, destroy
// the restricted gate. The mod sends them itself when the warchief dies; no name is known, so they go by id through
// CEventSystem::SendEvent, as SendMsg does after GetEventsFromString (SEventID = { hash, scope }, scope 0xFFFFFFFF = the .blo's global ones).
static const struct { const char* camp; unsigned events[3]; } g_warchiefGates[] = {
    { "wc1020", { 0x375C946E, 0x67C001BE, 0x23E4B827 } },   // Gasworks
    { "wc1050", { 0x72A5A43F, 0x43FE29AF, 0x4F4CEB62 } },   // Proving Grounds
    { "wc2020", { 0x0466F222, 0x46E3CA32, 0xEB1DF9B1 } },   // The Drop
    { "wc2040", { 0x92DE580A, 0xDBA08BCA, 0xE69EFF1A } },   // Tyrant's Lash
};
static const uintptr_t A_EventSystemSendEvent = 0x1409C3150;   // CEventSystem::SendEvent(this, const SEventID*, int, CEventData*)
static const uintptr_t A_EventSystemInstance = 0x1417116D8;    // CAvaSingle<CEventSystem>::Instance
static bool SendEventIds(const unsigned* hashes, int n, unsigned scope) {
    unsigned ids[2 * 8];
    if (n > 8) n = 8;
    for (int k = 0; k < n; k++) { ids[2 * k] = hashes[k]; ids[2 * k + 1] = scope; }
    uint64_t data[6] = { 0xFFFFFFFFFFull, 0, 0, 0, 0, 0 };
    __try {
        void* sys = *(void* const*)A_EventSystemInstance;
        if (!sys) return false;
        ((void (*)(void*, const void*, int, void*))A_EventSystemSendEvent)(sys, ids, n, data);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
static bool ArenaFlamesOff(int a) {
    bool ok = true;
    InterlockedExchange(&g_arenaOwnSend, 1);
    __try { ((void (*)(const char*))A_SendEventMsg)(g_topDogArena[a].flamesOff); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    InterlockedExchange(&g_arenaOwnSend, 0);
    return ok;
}
static void TopDogArenaOff(int i, const char* why) {
    for (int a = 0; a < (int)(sizeof g_topDogArena / sizeof g_topDogArena[0]); a++) {
        if (!strstr(g_camps[i].name, g_topDogArena[a].camp)) continue;
        InterlockedExchange(&g_valvesTurned, 0);
        InterlockedExchange(&g_arenaOwnSend, 1);
        bool ok = SendEventIds(&g_topDogArena[a].valvesOn, 1, 0);
        InterlockedExchange(&g_arenaOwnSend, 0);
        if (ok) {
            g_arenaWait.camp = i; g_arenaWait.arena = a; g_arenaWait.left = ARENA_FALLBACK;
            LogLine("CAMPREPOP %s: %s -> arena valves made usable (event %08X), flames put out by the mod in %.0f s if no valve is turned",
                g_camps[i].name, why, g_topDogArena[a].valvesOn, ARENA_FALLBACK);
        } else {
            bool off = ArenaFlamesOff(a);
            LogLine("CAMPREPOP %s: %s -> valves event NOT sent, the call failed; arena flames %s (%s)", g_camps[i].name, why,
                off ? "switched off" : "NOT switched off, the call failed", g_topDogArena[a].flamesOff);
        }
    }
    for (auto& g : g_warchiefGates) {
        if (!strstr(g_camps[i].name, g.camp)) continue;
        bool ok = SendEventIds(g.events, 3, 0xFFFFFFFFu);
        LogLine("CAMPREPOP %s: %s -> arena gate %s (events %08X %08X %08X)", g_camps[i].name, why, ok ? "opened" : "NOT opened, the call failed",
            g.events[0], g.events[1], g.events[2]);
    }
}
// r48: the fallback of TopDogArenaOff (RepopTick, game seconds)
static void ArenaTick(float step) {
    if (g_arenaWait.camp < 0) return;
    int i = g_arenaWait.camp, a = g_arenaWait.arena;
    if ((g_valvesTurned & 3) == 3) {
        LogLine("CAMPREPOP %s: both arena valves turned by the player", g_camps[i].name);
        g_arenaWait.camp = -1;
        return;
    }
    if ((g_arenaWait.left -= step) > 0.f) return;
    bool ok = ArenaFlamesOff(a);
    LogLine("CAMPREPOP %s: %s valve turned in %.0f s -> arena flames %s (%s)", g_camps[i].name, g_valvesTurned ? "one" : "no", ARENA_FALLBACK,
        ok ? "switched off by the mod" : "NOT switched off, the call failed", g_topDogArena[a].flamesOff);
    g_arenaWait.camp = -1;
}
static void RepopRememberDead(RepopCamp& r, unsigned id) {
    if (RepopIsDead(r, id)) return;
    if (r.deadCount < 80) r.deadIds[r.deadCount++] = id;
    if (id && id == r.topDog) {
        int i = (int)(&r - g_repop);
        LogLine("CAMPREPOP %s: Top Dog %08X dead", g_camps[i].name, id);
        TopDogArenaOff(i, "Top Dog dead");
    }
}
static void RepopTrackLookout(RepopCamp& r, unsigned id) {
    if (r.revivedCount < REPOP_TRACK) { r.lookoutMask |= 1ull << r.revivedCount; r.revivedIds[r.revivedCount++] = id; }
}

// Spawn budget: CSpawnedEntityHandler counts the dead too (+0x1667 used characters, +0x1669 limit). Over the limit it flags every dead entity for despawn and takes
// bodies or lower priority characters for a new spawn (Update 0x140565000, IsSpawnPossible 0x14054EE00). User test 2026-11-06 sd1270: bodies vanished only while spawns
// were running (8 in 6-24 s), the 12 after the last spawn stayed. +0x1669 follows +0x166B (the default from Init, read only by Update) every frame, and when +0x166B
// goes back down it only falls as far as the characters in use: nothing is flushed. So +0x166B is raised while a camp is active.
static const uintptr_t A_SpawnHandlerPtr = 0x14171B678;
static const int REPOP_BUDGET_EXTRA = 32;
static uint8_t g_budgetBase, g_budgetRaised;
static void RepopBudget(bool raise) {
    uint8_t* h = *(uint8_t* const*)A_SpawnHandlerPtr;
    if (!h) return;
    if (raise) {
        if (g_budgetRaised && h[0x166B] == g_budgetRaised) return;
        g_budgetBase = h[0x166B];
        g_budgetRaised = h[0x166B] = (uint8_t)(g_budgetBase + REPOP_BUDGET_EXTRA);
        LogLine("CAMPREPOP spawn budget raised: %d -> %d (characters in use %d, limit %d)", g_budgetBase, g_budgetRaised, h[0x1667], h[0x1669]);
    } else if (g_budgetRaised) {
        if (h[0x166B] == g_budgetRaised) h[0x166B] = g_budgetBase;
        g_budgetRaised = 0;
        LogLine("CAMPREPOP spawn budget back to %d (characters in use %d, limit %d)", g_budgetBase, h[0x1667], h[0x1669]);
    }
}

// How long a body is protected: CSpawnedEntity::AddDeadObject 0x14055FF80 copies CCharacter +0x4FC (6 s, set in the constructor, read nowhere else) into SDeadObject +0x10. While that
// timer runs, UpdateDeadObject 0x1405088A0 keeps the body even behind the camera or flagged by the budget; only the despawn distance and m_ForceDespawnDeadCharacterCount (a spawn that
// needs the room) still remove it. Idea from the mod "Bodies stay longer" (600 s, built for another exe), which writes the field per character; here the constructor's constant changes.
static const uintptr_t A_CharacterBodyTime = 0x140149ECF;       // imm32 of mov dword ptr [rbx + 0x4FC], 6.0f
static const float REPOP_BODY_SECONDS = 600.f;
static bool RepopBodyTimePatch() {
    uint32_t* imm = (uint32_t*)A_CharacterBodyTime;
    DWORD old;
    if (*imm != 0x40C00000 || !VirtualProtect(imm, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(imm, &REPOP_BODY_SECONDS, 4);
    VirtualProtect(imm, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), imm, 4);
    return true;
}

// A camp held by the enemy reads as enemy on the map, with no byte of the location table changed (it goes to the save, and +0x43 also feeds the camp scripts and dialogue:
// scripts/gameobjects/encampment/camp_is_completed). CGameMapIcon::ShowIcon re-reads the main mission for every icon whenever the GUI list is rebuilt, so only that call
// is answered differently. The camp panel keeps the real data: user 2026-11-06, objectives read as done (10/10) on a retaken camp.
static const uintptr_t A_LocMainDone = 0x1403DF0D0;             // LocationInfoIsCampMainMissionCompleted: +0x42 (total) && !+0x43 (left)
static const uintptr_t A_LocMainDoneShowIconRet = 0x14022E25E;
typedef bool (*LocMainDoneFn)(const uint8_t*);
static LocMainDoneFn LocMainDone_orig = nullptr;
static bool CampHeldByEnemy(const uint8_t* entry) {
    unsigned key = *(const unsigned*)(entry + 0x30);
    for (size_t i = 0; i < sizeof g_campLocKey / sizeof g_campLocKey[0]; i++)
        if (g_campLocKey[i] == key) return g_repop[i].state == RP_PENDING || g_repop[i].state == RP_ACTIVE;
    return false;
}
static bool LocMainDone_hook(const uint8_t* entry) {
    if ((uintptr_t)_ReturnAddress() == A_LocMainDoneShowIconRet && CampHeldByEnemy(entry)) return false;
    return LocMainDone_orig(entry);
}

// The status screen (gui/hud_status_cam, setup_location_info) takes the title and the main objective's tick from the same test, left of pair 7 == 0: true shows "Ally Camp" and the tick, false the camp type
// and no tick. No value there gives the enemy title with 10/10, so the data stays real and only the text changes: "Ally Camp" is used by this screen alone (no other GUI file, not in the exe), and the
// location on show is the one the GUI last asked the tracked items of. Text ids from location_info_helpers.get_location_type_string_from_type, in every language of the game.
static const uintptr_t A_LocTrackedItems = 0x1403E0A30;   // LocationInfoGetTrackedItemInfo: 11 pairs of bytes at entry +0x34, called by CGameGuiCallbacks::HandleGameEvents only
static const uintptr_t A_GuiLocalizedText = 0x1409192D0;  // CTextUtil::GetLocalizedText: called by GameGuiRenderText and FontNodeResizeHandler, text id at SGUIXFontData +4
static const unsigned TEXT_ALLY_CAMP = 0x52760149;
typedef void (*LocTrackedItemsFn)(unsigned short, const uint8_t**, unsigned long long*);
typedef void (*GuiLocalizedTextFn)(uint8_t*, void*, bool*);
static LocTrackedItemsFn LocTrackedItems_orig = nullptr;
static GuiLocalizedTextFn GuiLocalizedText_orig = nullptr;
static volatile unsigned g_guiEnemyTitle = 0;             // text id for the location on show when the enemy holds it, else 0
static void LocTrackedItems_hook(unsigned short id, const uint8_t** items, unsigned long long* count) {
    LocTrackedItems_orig(id, items, count);
    const uint8_t* entry = *items - 0x34;
    unsigned title = 0;
    if (CampHeldByEnemy(entry))
        switch (entry[4]) {
        case 0: title = 0x3C8BA479; break;                // Oil transfer camp
        case 1: title = 0xEE1BB53B; break;                // Oil Pump Camp
        case 2: title = 0x638153D4; break;                // Top Dog camp
        case 6: title = 0xBB21FAA4; break;                // Stank Gum Camp
        }
    g_guiEnemyTitle = title;
}
static void GuiLocalizedText_hook(uint8_t* font, void* out, bool* system) {
    unsigned title = g_guiEnemyTitle;
    if (!title || *(unsigned*)(font + 4) != TEXT_ALLY_CAMP) { GuiLocalizedText_orig(font, out, system); return; }
    static bool logged = false;
    if (!logged) { logged = true; LogLine("CAMPREPOP banner: enemy title %08X in use", title); }
    *(unsigned*)(font + 4) = title;
    GuiLocalizedText_orig(font, out, system);
    *(unsigned*)(font + 4) = TEXT_ALLY_CAMP;
}

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
// r47: the four evt.top_dog_dead events go through: in every warchief camp their only listeners are evt.top_dog_dead triggers that make
// the arena's valve interactions usable (entry.usable of mmax_turn_valve contexts): dropped, the valves would stay unusable like
// Large Blade's did (user 2026-11-19; why: see g_topDogArena)
static const unsigned g_topDogCompletion[] = { 0x868D48E1, 0x400BC781 };
static volatile LONG g_topDogBlocked = 0;
// r44: only while a held camp whose Top Dog is back has the player within REPOP_LOAD (the events carry no camp: the first liberation of
// another Top Dog camp must still complete; the 2026-10-16 filter dropped them whenever any camp was held)
static bool TopDogCampHeldNear() {
    for (int i = 0; i < (int)(sizeof g_camps / sizeof g_camps[0]); i++) {
        const RepopCamp& r = g_repop[i];
        if (r.topDog && (r.state == RP_PENDING || r.state == RP_ACTIVE) && CampDistance(g_camps[i]) <= REPOP_LOAD) return true;
    }
    return false;
}
// r48: the arena events seen: the valves turned (valveOff) and whether the game sends valvesOn itself (unknown, see g_topDogArena)
static volatile LONG g_arenaSeenLogs;
static void ArenaEventSeen(unsigned a, unsigned b) {
    if (g_arenaOwnSend) return;
    for (auto& t : g_topDogArena) {
        if ((a == t.valvesOn || b == t.valvesOn) && InterlockedIncrement(&g_arenaSeenLogs) <= 20)
            LogLine("CAMPREPOP arena: the game sent the valves event %08X itself (%s)", t.valvesOn, t.camp);
        for (int k = 0; k < 2; k++)
            if (a == t.valveOff[k] || b == t.valveOff[k]) {
                InterlockedOr(&g_valvesTurned, 1 << k);
                if (InterlockedIncrement(&g_arenaSeenLogs) <= 20) LogLine("CAMPREPOP arena: %s valve turned (event %08X, %s)", k ? "right" : "left", t.valveOff[k], t.camp);
            }
    }
}
static void EventSend_hook(void* e, void* data) {
    unsigned a = 0, b = 0;
    __try { a = *(const unsigned*)((uint8_t*)e + 8); b = *(const unsigned*)((uint8_t*)e + 0xC); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    ArenaEventSeen(a, b);
    if (g_topDogsBack && TopDogCampHeldNear()) {
        for (unsigned h : g_topDogCompletion)
            if (a == h || b == h) {
                if (InterlockedIncrement(&g_topDogBlocked) <= 50) LogLine("CAMPREPOP Top Dog completion event %08X dropped (camp repopulated)", h);
                return;
            }
    }
    EventSend_orig(e, data);
}
static bool g_repopInit;
static float g_repopAttackIn;                 // game seconds to the next attack; stopped while REPOP_MAX_TAKEN camps are held
static char g_repopFirstCamp[8][16];          // test: third field onwards of repop_test.txt, parts of camp names: each attack takes the first listed camp that can fall
static int g_repopFirstN;                     // (r43d: a list, user 2026-11-18 "ajuste para esses acampamentos sejam atacados antes"); a name leaves the list once it fell
static int g_repopTotal;

static bool SafeSpawn(uint8_t* sp) { __try { ((LocFn)A_SpawnPointSpawn)(sp); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
// CSpawnPoint transform: the CMatrix4f spawned at (+0x128, see CSpawnPoint::Spawn), translation row at +0x30
static bool SpawnPointXZ(uint8_t* sp, float cx, float cz, float* x, float* z) {
    __try {
        float px = *(float*)(sp + 0x128 + 0x30), pz = *(float*)(sp + 0x128 + 0x38);
        if (!(fabsf(px - cx) < 700.f && fabsf(pz - cz) < 700.f)) return false;    // not a plausible position of this camp
        *x = px; *z = pz; return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
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
static float RandAttack() { return g_repopAttackMin + (g_repopAttackMax - g_repopAttackMin) * (rand() / (float)RAND_MAX); }

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

// Lookout towers. Gob Stone's lookout F408814D died from a fall 2 s after every spawn (autotests A, B, C and the user's r32 test;
// POINTDIAG: CPfxRigidBodyCharacterInstance::ProcessHitInfos -> CCharacter::DoReactToHit -> InflictDamage): it stands on a sniper tower
// of the base location (platform AA30074E 1.2 m under it, pylons, skull, chain, jaw), pulled down in the save, and spawns in the air.
// 16 of the 24 camps have such towers (tools/aibt/lookout_towers.py). Rebuilding the tower does not hold: its state lives in a
// CEventRouter of the base location (save=1; Gob Stone's 3381362438: in 9A30265F from the CCreationTrigger when the location is created,
// switch EB9E3BB3 = the tower fell, out C6B60C08 = the disable event of every tower part) that switches the parts off again on every
// load (r34, r35diag), and with the player near the game breaks a rebuilt tower again (r36 rebuilt it every second: a destruction
// animation in a loop, user test 2026-11-15). r37: the user saw the lookouts of Dead End and Hell's Grill (short towers) come back on
// the ground where the tower stood, so a lookout whose platform is down (not enabled, or broken) is moved to the ground before it
// spawns: its spawn point's transform (+0x128, read by CSpawnPoint::Spawn) gets the height of the first static surface under it, or the
// terrain's. Spawn points are not saved: a location loaded again has the file's position, moved again then.
#include "lookout_tower_table.inc"
static void TowersForCycle(int i) {
    RepopCamp& r = g_repop[i];
    r.towerCount = 0;
    for (auto& t : g_lookoutTowers)
        if (t.camp == i)
            for (unsigned p : t.parts) if (p && r.towerCount < 16) r.towerIds[r.towerCount++] = p;
}
typedef void (*DestructibleFn)(void*);
static bool SafeParts(uint8_t* obj) {
    __try { ((DestructibleFn)A_DestructibleDisableParts)(obj); ((DestructibleFn)A_DestructibleEnableParts)(obj); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// a game object or one of its children (m_Children +0x90, shared_ptr pairs) by runtime id, a few levels down
static uint8_t* FindInTree(uint8_t* go, unsigned id, int depth) {
    if (!go) return nullptr;
    __try {
        if (*(const unsigned*)(go + 0x14) == id) return go;
        if (depth <= 0) return nullptr;
        uint8_t* const* b = *(uint8_t* const* const*)(go + 0x90); uint8_t* const* e = *(uint8_t* const* const*)(go + 0x98);
        if (!b || e < b || e - b > 2 * 512) return nullptr;
        for (uint8_t* const* it = b; it < e; it += 2)
            if (uint8_t* f = FindInTree(*it, id, depth - 1)) return f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}
// a loaded object of the given locations (FindLocations) by runtime id: the location's object container +0x20 -> +0x20, shared_ptr pairs
static uint8_t* FindInLocations(uint8_t* const* l, int n, unsigned id) {
    for (int k = 0; k < n; k++) {
        __try {
            const uint8_t* cont = *(const uint8_t* const*)(l[k] + 0x20);
            const uint8_t* oc = cont ? *(const uint8_t* const*)(cont + 0x20) : nullptr;
            if (!oc) continue;
            uint8_t* const* b = *(uint8_t* const* const*)oc; uint8_t* const* e = *(uint8_t* const* const*)(oc + 8);
            if (!b || e < b || e - b > 2 * 5000) continue;
            for (uint8_t* const* it = b; it < e; it += 2)
                if (uint8_t* go = FindInTree(*it, id, 6)) return go;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return nullptr;
}
static const uintptr_t A_TerrainHeight = 0x14078BCF0;         // CTerrainMapBase::GetBilinearHeight(const CVector3f&)
static const uintptr_t A_GroundBelow = 0x14057FD40;           // CAirVehicle::GetGroundHeight_Expensive: a static-only ray from y + 1 down to the terrain
static const uintptr_t A_TerrainInstance = 0x141711D10;       // CAvaSingle<CTerrainMapBase>::Instance
static const int LOOKOUT_N = sizeof g_lookoutPlatforms / sizeof g_lookoutPlatforms[0];
static uint8_t* g_lookoutPlatGo[LOOKOUT_N];                   // where each platform was last found loaded (checked by its id before use)
static float g_lookoutLowY[LOOKOUT_N];                        // the height each point was moved to (0: not moved)
static bool g_lookoutNoPlat[LOOKOUT_N];                       // "platform not loaded" logged
// the ground under p: the first static surface below the platform if it is not under the terrain, else the terrain
static bool LookoutGround(const float* p, float* ground, float* terrain, float* ray) {
    __try {
        const void* t = *(const void* const*)A_TerrainInstance;
        if (!t) return false;
        float q[3] = { p[0], 0.f, p[2] };
        *terrain = ((float (*)(const void*, const float*))A_TerrainHeight)(t, q);
        q[1] = p[1] - 3.f;                                    // the ray starts 1 m higher: under the platform
        *ray = ((float (*)(const float*))A_GroundBelow)(q);
        *ground = *ray > *terrain - 1.f && *ray < p[1] - 2.5f ? *ray : *terrain;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// every second while the camp's enemy location is loaded (sp / ids: its spawn points); moved again if the ground found later is higher
// (static collision streamed in after the first look)
static void LookoutsToGround(int i, uint8_t* const* sp, const unsigned* ids, int n) {
    uint8_t* l[4]; int nl = -1;
    for (int e = 0; e < LOOKOUT_N; e++) {
        if (g_lookoutPlatforms[e].camp != i) continue;
        int k = 0;
        while (k < n && ids[k] != g_lookoutPlatforms[e].lookout) k++;
        if (k >= n) continue;
        float* p = (float*)(sp[k] + 0x128 + 0x30);
        float y;
        __try { y = p[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        uint8_t* go = g_lookoutPlatGo[e];
        bool same = false;
        if (go) __try { same = *(const unsigned*)(go + 0x14) == g_lookoutPlatforms[e].platform; } __except (EXCEPTION_EXECUTE_HANDLER) { same = false; }
        if (!same) {
            if (nl < 0) nl = FindLocations(g_campBase[i], l, 4);
            go = g_lookoutPlatGo[e] = nl > 0 ? FindInLocations(l, nl, g_lookoutPlatforms[e].platform) : nullptr;
            if (!go) {
                if (!g_lookoutNoPlat[e]) LogLine("CAMPREPOP %s: lookout %08X: its tower platform %08X is not loaded, the point is left where it is", g_camps[i].name, ids[k], g_lookoutPlatforms[e].platform);
                g_lookoutNoPlat[e] = true;
                continue;
            }
        }
        uint8_t flags;
        __try { flags = go[0x238]; } __except (EXCEPTION_EXECUTE_HANDLER) { g_lookoutPlatGo[e] = nullptr; continue; }
        if ((flags & 0x08) && !(flags & (0x01 | 0x10))) continue;   // the tower stands
        float ground, terrain, ray;
        if (!LookoutGround(p, &ground, &terrain, &ray)) continue;
        bool moved = g_lookoutLowY[e] != 0.f && fabsf(y - g_lookoutLowY[e]) < 0.01f;
        if (moved ? ground + 0.3f - y < 0.5f : y - ground < 2.f) continue;   // on the ground already (a short drop: not moved)
        p[1] = ground + 0.3f;
        g_lookoutLowY[e] = p[1];
        LogLine("CAMPREPOP %s: lookout %08X: its tower is down (platform %08X flags %02X), spawn point moved to the ground (y %.1f -> %.1f; terrain %.1f, static ray %.1f)",
            g_camps[i].name, ids[k], g_lookoutPlatforms[e].platform, flags, y, p[1], terrain, ray);
    }
}

// the camp's name in the game's language, "" if it cannot be read
static const char* CampTitle(int i) {
    const char* title = "";
    __try {
        const uint8_t* t = *(const uint8_t* const*)A_LocationInfoTable;
        const uint8_t* e = t ? *(const uint8_t* const*)t : nullptr; unsigned count = t ? *(const unsigned*)(t + 8) : 0;
        for (unsigned n = 0; e && n < count && n < 1024; n++, e += 0x60)
            if (*(const unsigned*)(e + 0x30) == g_campLocKey[i]) {
                title = ((const char* (*)(void*, unsigned))A_LocatorGetString)(*(void**)A_Locator, *(const unsigned*)(e + 0x14));
                break;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return title ? title : "";
}

// r39 (2026-11-15, r38diag autotests F: Finger Peak sd1240 completed by the game when the first scav of a repopulated camp died, 10
// alive; the same class as Gob Stone retaken with no fight, sd1270 / sd1290 on 2026-11-06 and Hollow Point): a scav's death event
// (Finger Peak's 8C0DC183) runs location_info_complete_objective_picked_location.gsr, which counts the objective's tracked items, finds
// them done (the save has the camp liberated) and sends 2939A218; that starts the camp's completion graph gsr.fuel_cam_control
// (fuel_center_cam_control.gsr), whose Timer despawns every enemy (72EDE02F) and switches the enemy location off (6E8F303A ->
// 673228D7) 1.8 s later. Every camp has one (tools/aibt/camp_completion_table.py). While a camp is held its completion graph's events
// are dropped: the mod's own count retakes the camp (CampRetaken)
#include "camp_completion_table.inc"
static const uintptr_t A_GraphScriptHandleEvent = 0x1402B2DA0;   // CGraphScriptGameObject::HandleEvent (this = object + 0xD0)
static volatile LONG g_completionLines;
DEFHOOK(void, GraphScriptHandleEvent, (uint8_t* self, const uint8_t* impl, const void* data)) {
    if (g_repopInit) {
        unsigned id = 0, idAlt = 0;
        __try { id = *(const unsigned*)(self - 0xD0 + 0x14); idAlt = *(const unsigned*)(self - 0xD0 + 8 + 0x14); } __except (EXCEPTION_EXECUTE_HANDLER) { id = idAlt = 0; }
        for (auto& g : g_campCompletion) {
            if (g.graph != id && g.graph != idAlt) continue;
            int st = g_repop[g.camp].state;
            if (st != RP_PENDING && st != RP_ACTIVE) break;
            if (InterlockedIncrement(&g_completionLines) <= 40) {
                unsigned h = 0;
                __try { h = *(const unsigned*)(impl + 0xC); } __except (EXCEPTION_EXECUTE_HANDLER) { h = 0; }
                LogLine("CAMPREPOP %s: the game's camp completion ignored (graph %08X%s, event %08X): the camp is held", g_camps[g.camp].name, g.graph,
                    g.graph == id ? "" : " at +8", h);
            }
            return;
        }
    }
    GraphScriptHandleEvent_orig(self, impl, data);
}

// the player beat the camp: the game's own completion screen does not run for a repopulated camp, the notice stands in for it
static void CampRetaken(int i) {
    const char* title = CampTitle(i);
    char line[96];
    snprintf(line, sizeof line, title[0] ? "%s retaken" : "Camp retaken", title);
    bool native = NativeMessage(line, "Camp retaken", title);
    if (!native) CampNoticeShow("Camp retaken", title, NOTICE_MS);
    LogLine("CAMPREPOP %s: notice shown (camp retaken, \"%s\"%s)", g_camps[i].name, title, native ? ", game message" : "");
    if (g_repop[i].topDog && !RepopIsDead(g_repop[i], g_repop[i].topDog)) TopDogArenaOff(i, "camp retaken without the Top Dog's death");
}

// the camp fell: the player reads which one while driving, without opening the map (user 2026-11-06)
static void CampAttacked(int i) {
    const char* title = CampTitle(i);
    if (!title[0]) return;
    char line[64];
    // not "by Scrotus": some camps are taken by Roadkill raiders (user 2026-11-13)
    snprintf(line, sizeof line, "Enemies attacked %s", title);
    bool native = NativeMessage(line, line, "Retake it");
    bool queued = native || CampNoticeQueue(line, "Retake it");
    LogLine("CAMPREPOP %s: notice %s (camp attacked, \"%s\"%s)", g_camps[i].name, queued ? "queued" : "dropped, queue full", title, native ? ", game message" : "");
}

// the camp's enemies are gone: its enemy location goes off now; the civilians come back later, when the player is away (RP_CLEARED)
static void RepopRestore(int i, const char* why) {
    const CampEntry& c = g_camps[i];
    SetLocBit(c.enemyId, false);
    uint8_t* bsp[48]; unsigned bids[48];                 // revived base points go off with the cycle, or the next save writes them on
    int bn = LocSpawnPoints(g_campBase[i], bsp, bids, 48);
    for (int k = 0; k < bn; k++)
        for (int j = 0; j < g_repop[i].baseCount; j++) if (bids[k] == g_repop[i].baseIds[j]) bsp[k][0x232] = 0;
    g_repop[i].state = RP_CLEARED; g_repop[i].revived = false; g_repop[i].revivedCount = 0; g_repop[i].forced = false; g_repop[i].cycleStarted = false;
    g_repop[i].baseCount = 0; g_repop[i].baseDone = false; g_repop[i].towerCount = 0; g_repop[i].deadCount = 0; g_repop[i].armed = false;
    g_repop[i].baseIgnored = 0; memset(g_repop[i].baseNoLive, 0, sizeof g_repop[i].baseNoLive); memset(g_repop[i].baseLost, 0, sizeof g_repop[i].baseLost);
    LogLine("CAMPREPOP %s: %s -> enemy location off, civilians return when the player is %.0f m away", c.name, why, REPOP_CIV_BACK);
}
static void RepopCiviliansBack(int i) {
    const CampEntry& c = g_camps[i];
    BlockCivilians(c.civilianId, false);
    SetLocBit(c.civilianId, true);
    g_repop[i].state = RP_IDLE;
    g_repop[i].cooldown = RandCooldown();
    LogLine("CAMPREPOP %s: civilians back (camp liberated again), safe for %.0f min", c.name, g_repop[i].cooldown / 60.f);
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
// The characters of a spawn point in the spawn system. One moves from alive to dead 5 s after dying (CCharacter::IsDead(5), CSpawnedEntity::AddDeadObject), whatever the point's m_Enabled does: 2B89002B of
// sd2060 stayed on after each kill and the lookouts go off with the sniper alive (r10 test). KillAll moves a healthy one too, but only with the player past the despawn radius (300 m or more): read it close by.
static bool SpawnEntityChars(const uint8_t* sp, int* live, int* dead) {
    bool found = false;
    *live = *dead = 0;
    __try {
        const uint8_t* h = *(const uint8_t* const*)A_SpawnHandlerPtr;
        if (!h) return false;
        unsigned id = *(const unsigned*)(sp + 0x198);
        uint8_t* const* end = *(uint8_t* const* const*)(h + 0x58);
        for (uint8_t* const* e = *(uint8_t* const* const*)(h + 0x50); e != end; e++)
            if (*(const unsigned*)(*e + 0x20) == id) { *live += (*e)[0xA0]; *dead += (*e)[0xA2]; found = true; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return found;
}
// 2B89002B of sd2060 spawns with god mode on (CCharacter +0xE9; who sets it is not known): CDamageable::SetHealth stops its health at 0.1, it lies on the ground for good, the combat music plays on and it
// never reaches the dead list (r15 test: 80 -> 0.100, god 1, three characters in a row). Switched off on every live character of the point, the CCharacter being 8 bytes before the object. Returns the last one cleared
static const uintptr_t VT_CHARACTER_GO = 0x1411BDBD8;      // CCharacter's CGameObject vftable
static uint8_t* SpawnEntityClearGod(const uint8_t* sp) {
    uint8_t* cleared = nullptr;
    __try {
        const uint8_t* h = *(const uint8_t* const*)A_SpawnHandlerPtr;
        if (!h) return nullptr;
        unsigned id = *(const unsigned*)(sp + 0x198);
        uint8_t* const* end = *(uint8_t* const* const*)(h + 0x58);
        for (uint8_t* const* e = *(uint8_t* const* const*)(h + 0x50); e != end; e++) {
            if (*(const unsigned*)(*e + 0x20) != id) continue;
            uint8_t* last = *(uint8_t* const*)(*e + 0x48);
            for (uint8_t* o = *(uint8_t* const*)(*e + 0x40); o != last; o += 16) {
                uint8_t* go = *(uint8_t* const*)o;
                if (*(const uintptr_t*)go == VT_CHARACTER_GO && go[0xE9 - 8]) { go[0xE9 - 8] = 0; cleared = go; }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return cleared;
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

// ---- minefields (2026-11-13) ------------------------------------------------------------------------------------------------------
// A minefield (aXX_minefield_*, .nl part only, no enemies) has 3 buried mines. Each mine has a saved CEventRouter that starts at 1
// (property B1A41FF8: mine active) and goes to 0 once the mine is disarmed or explodes; on creation the location asks it and the mine
// comes up active or removed. A fourth router goes 1 -> 0 when the CountTrigger counted the 3 mines (the field is cleared). The
// CountTrigger and the 3 loot bags (the scrap of a disarmed mine) are saved too. The world map's progress is kept elsewhere
// (LocationInfoCompleteTrackedItem counts down per item type and stops at 0), so only a field the mod saw cleared comes back:
// re-arming an unfinished one would let the player finish it by disarming the same mine again. Cycle: cleared seen (completion
// router 0 while loaded) -> cooldown (same as the camps) -> armed once the player is past the stream-out radius -> on the next
// stream-in the Load hooks keep the fresh values of the table objects: the mines are buried again and the dog has to find them.
// The map stays cleared.
#include "minefield_table.inc"
static const int MF_N = sizeof g_minefields / sizeof g_minefields[0];
static const uintptr_t A_RouterLoad = 0x1403E18F0;              // CEventRouter::Load(const CSaveContext&): one saved bit -> go+0xE8
static const uintptr_t A_CountTriggerLoad = 0x1405704F0;        // CCountTrigger::Load: count +0xE8, bit 0x02 of +0x160
static const uintptr_t A_LootBagLoad = 0x140310E70;             // CLootBag::Load: +0x170
#define MF_DIAG 0   // 1 = the r30 MINEDIAG lines (router bit changes with their callers, the bits saved on unload and loaded back)
struct MinefieldState { float cd; bool cleared, armed, seen; int reset; float loadedFor; };
static MinefieldState g_mf[MF_N];
static int g_mfRevivedTotal;
#if MM_DEV_TOOLS || MM_AUTOTEST
static int g_mfAutoClear = -1; static float g_mfAutoWait;       // autotest "mines": the nearest field is marked cleared once loaded
#endif
static float MinefieldDistance(const MinefieldEntry& e) { float dx = g_PlayerPos.x - e.x, dz = g_PlayerPos.z - e.z; return sqrtf(dx * dx + dz * dz); }
// the minefield an object belongs to (only an armed one if armedOnly): 0 completion router, 1 count trigger, 2 mine router, 3 loot bag
static int MinefieldObject(unsigned id, int* kind, bool armedOnly = true) {
    if (!id || !g_repopInit) return -1;
    for (int i = 0; i < MF_N; i++) {
        if (armedOnly && !g_mf[i].armed) continue;
        const MinefieldEntry& e = g_minefields[i];
        if (id == e.done) { *kind = 0; return i; }
        if (id == e.count) { *kind = 1; return i; }
        for (int k = 0; k < 3; k++) {
            if (id == e.mines[k]) { *kind = 2; return i; }
            if (id == e.loot[k]) { *kind = 3; return i; }
        }
    }
    return -1;
}
DEFHOOK(void, RouterLoad, (uint8_t* go, void* ctx)) {
    unsigned id = 0; uint8_t fresh = 0;
    __try { id = *(const unsigned*)(go + 0x14); fresh = go[0xE8]; } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
    RouterLoad_orig(go, ctx);
    int kind, i = MinefieldObject(id, &kind);
    if (i < 0) {   // r30 diagnostic: what the save storage holds when a field that is not armed streams in
#if MF_DIAG
        int any = MinefieldObject(id, &kind, false);
        if (any >= 0) LogLine("MINEDIAG %s: %s router %08X loaded %d from the save", g_minefields[any].name, kind == 0 ? "completion" : "mine", id, go[0xE8]);
#endif
        return;
    }
    uint8_t saved = go[0xE8];
    go[0xE8] = fresh;
    g_mf[i].reset++;
    LogLine("MINEREPOP %s: %s router %08X saved %d, kept %d", g_minefields[i].name, kind == 0 ? "completion" : "mine", id, saved, fresh);
}
DEFHOOK(void, CountTriggerLoad, (uint8_t* go, void* ctx)) {
    unsigned id = 0; int fresh = 0; uint8_t freshBits = 0;
    __try { id = *(const unsigned*)(go + 0x14); fresh = *(const int*)(go + 0xE8); freshBits = go[0x160]; } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
    CountTriggerLoad_orig(go, ctx);
    int kind, i = MinefieldObject(id, &kind);
    if (i < 0) return;
    int saved = *(const int*)(go + 0xE8); uint8_t savedBits = go[0x160];
    *(int*)(go + 0xE8) = fresh; go[0x160] = freshBits;
    g_mf[i].reset++;
    LogLine("MINEREPOP %s: count trigger %08X saved %d (flags %02X), kept %d (flags %02X)", g_minefields[i].name, id, saved, savedBits, fresh, freshBits);
}
DEFHOOK(void, LootBagLoad, (uint8_t* go, void* ctx)) {
    unsigned id = 0, fresh = 0;
    __try { id = *(const unsigned*)(go + 0x14); fresh = *(const unsigned*)(go + 0x170); } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
    LootBagLoad_orig(go, ctx);
    int kind, i = MinefieldObject(id, &kind);
    if (i < 0) return;
    unsigned saved = *(const unsigned*)(go + 0x170);
    *(unsigned*)(go + 0x170) = fresh;
    g_mf[i].reset++;
    LogLine("MINEREPOP %s: loot bag %08X saved %u, kept %u", g_minefields[i].name, id, saved, fresh);
}

#if MF_DIAG
// r30 diagnostic (Pablo's r29 tests 2026-11-13: the mines came back 3 of 3 active, then the field was cleared again once while he
// drove in and once while he was away). Logs every change of a table router's bit with the player's distance and the exe return
// addresses on the stack (resolve with pdb_symbols.json), and the bit each router has when its location unloads. Result 2026-11-14:
the reset bits are saved on unload and loaded back; every 1 -> 0 came from CInteractionGraph::SendOnLeaveEvent (the player's
pliers) and the completion router from CCountTrigger::OnUp, i.e. the r29 'cleared again' was the player clearing the field.
static const uintptr_t A_RouterHandleEvent = 0x1403EA8D0;       // CEventRouter::HandleEvent(event, data): this = go + 0xD0, bit at +0x18
static const uintptr_t A_SaveOnDestroy = 0x1403B1820;           // CGameWorld::SaveGameObjectOnDestroy(go): Save() as an object leaves the world
static void ExeCallers(char* out, size_t n) {
    void* f[16];
    USHORT c = RtlCaptureStackBackTrace(2, 16, f, NULL);
    size_t len = 0; out[0] = 0;
    for (USHORT k = 0; k < c && len + 12 < n; k++) {
        uintptr_t a = (uintptr_t)f[k];
        if (a >= 0x140000000ull && a < 0x141AA8000ull) len += snprintf(out + len, n - len, " %llX", (unsigned long long)a);
    }
}
DEFHOOK(void, RouterHandleEvent, (uint8_t* h, const void* ev, const void* data)) {
    unsigned id = 0; uint8_t before = 0;
    __try { id = *(const unsigned*)(h - 0xD0 + 0x14); before = h[0x18]; } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
    RouterHandleEvent_orig(h, ev, data);
    int kind, i = MinefieldObject(id, &kind, false);
    if (i < 0 || h[0x18] == before) return;
    char callers[200]; ExeCallers(callers, sizeof callers);
    LogLine("MINEDIAG %s: %s router %08X %d -> %d, player %.0f m away; callers%s", g_minefields[i].name, kind == 0 ? "completion" : "mine", id,
        before, h[0x18], MinefieldDistance(g_minefields[i]), callers);
}
DEFHOOK(void, SaveOnDestroy, (void* world, uint8_t* go)) {
    unsigned id = 0;
    __try { id = *(const unsigned*)(go + 0x14); } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
    int kind, i = MinefieldObject(id, &kind, false);
    if (i >= 0 && kind != 3)
        LogLine("MINEDIAG %s: %s %08X leaves the world with %d, player %.0f m away", g_minefields[i].name,
            kind == 0 ? "completion router" : kind == 1 ? "count trigger" : "mine router", id, kind == 1 ? *(const int*)(go + 0xE8) : go[0xE8],
            MinefieldDistance(g_minefields[i]));
    SaveOnDestroy_orig(world, go);
}
#endif

static void MinefieldTick(float step) {
    for (int i = 0; i < MF_N; i++) {
        const MinefieldEntry& e = g_minefields[i]; MinefieldState& s = g_mf[i];
        float d = MinefieldDistance(e);
        if (s.cd > 0.f) s.cd -= step;
        if (d > e.loadR + 30.f) { s.seen = false; s.loadedFor = 0.f; }
#if MM_DEV_TOOLS || MM_AUTOTEST
        // a field cleared in the save is not marked again (autotest 2026-11-13: the pending mark waited and hit the field once it came back)
        if (g_mfAutoClear == i && s.cleared) { g_mfAutoClear = -1; LogLine("AUTOTEST mines %s: already cleared, nothing to mark", e.name); }
#endif
        if (s.armed && d > e.loadR + 30.f) continue;
        if (!s.armed && s.cleared) {
            if (s.cd <= 0.f && d > e.unloadR + 50.f) {
                s.armed = true; s.reset = 0; s.loadedFor = 0.f;
                LogLine("MINEREPOP %s: armed (player %.0f m away): the mines come back when the field loads again", e.name, d);
            }
            continue;
        }
        if (d > e.loadR + 30.f) continue;
        const unsigned want[5] = { e.done, e.mines[0], e.mines[1], e.mines[2], e.count };
        uint8_t* o[5];
        if (LocObjects(e.id, want, 5, o) < 0 || !o[0]) { s.loadedFor = 0.f; continue; }   // not loaded
        s.loadedFor += step;
        if (s.loadedFor < 3.f) continue;                            // after the location's own creation events
        int active = 0, count = 0; uint8_t done = 1;
        __try {
            done = o[0][0xE8];
            for (int k = 1; k <= 3; k++) active += o[k] && o[k][0xE8];
            if (o[4]) count = *(const int*)(o[4] + 0xE8);
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
#if MM_DEV_TOOLS || MM_AUTOTEST
        if (g_mfAutoClear == i) {   // as if the 3 mines were disarmed: the routers and the counter as the game leaves them
            g_mfAutoClear = -1;
            __try { o[0][0xE8] = 0; for (int k = 1; k <= 3; k++) if (o[k]) o[k][0xE8] = 0; if (o[4]) *(int*)(o[4] + 0xE8) = 3; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            LogLine("AUTOTEST mines %s: marked cleared (was: completion router %d, %d of 3 mines active, count %d)", e.name, done, active, count);
            continue;
        }
#endif
        if (s.armed) {
            s.armed = false; s.cleared = false; s.seen = true; s.cd = RandCooldown();
            if (s.reset) g_mfRevivedTotal++;
            LogLine("MINEREPOP %s: %s (%d saved objects reset; now completion router %d, %d of 3 mines active, count %d), player %.0f m away; next in %.0f min at the earliest",
                e.name, s.reset ? "mines back" : "loaded with no saved state to reset", s.reset, done, active, count, d, s.cd / 60.f);
            continue;
        }
        bool firstSight = !s.seen;
        if (firstSight) {
            s.seen = true;
            LogLine("MINEREPOP %s: loaded, %s (completion router %d, %d of 3 mines active, count %d), player %.0f m away", e.name,
                done ? "not cleared yet" : "cleared", done, active, count, d);
        }
        if (!done && !s.cleared) {
            s.cleared = true;
            if (!firstSight) s.cd = RandCooldown();                 // cleared just now: the full cooldown from here
            LogLine("MINEREPOP %s: cleared; the mines come back after %.0f min at the earliest, once you are %.0f m away", e.name, s.cd > 0.f ? s.cd / 60.f : 0.f, e.unloadR + 50.f);
        }
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
            if (*(float*)(sp[k] + 0x168) > 100.f || sp[k][0x232] || IsTopDog(ids[k]) || InPoints(g_nonCharPoints, ids[k])) continue;   // characters that died only
            off++;
            if (SafeRevive(sp[k])) ok++;
        }
        g_lootArmed[i] = false; g_lootCd[i] = RandCooldown(); g_lootRevivedTotal += ok;
        if (off) LogLine("LOOTREPOP %s: player %.0f m away, %d spawn points, %d were off, %d revived; next in %.0f min", e.name, d, n, off, ok, g_lootCd[i] / 60.f);
    }
}

// ---- camp diagnostic (r3diag, 2026-11-05) ------------------------------------------------------------------------------------------
// User test 2026-11-05: enemy icon on the map, only allies at the camp. The log could not say whether Spawn() gave anything
// (SpawnSystemSpawn always returns 0) nor which camp the player stood in. Logs the player's track, each camp's location state and every
// change of a spawn point within 500 m. Two status reads: CSpawnPoint::Spawn writes the new spawn id at sp + 0x19C, SpawnStatus reads sp + 0x198.
#define MM_CAMP_DIAG 0
// What the game holds for a spawn point: CSpawnedEntity +0x40, shared_ptr<CGameObject>, the CCharacter 8 bytes before the object. +0xE9 is god mode, +0xEA the E3 invulnerability, and a character
// tied to a vehicle never reaches the dead list whatever its health (IsReadyForDespawn skips IsDead). 2B89002B of sd2060 stayed at 1 alive after being beaten, in three tests
struct EnemyVitals { uint8_t* go; uintptr_t vt; float hp, timer; int objects, vehicles, god, e3, flags; void* vehicle; };
static bool EnemyVitalsRead(const uint8_t* sp, EnemyVitals* v) {
    memset(v, 0, sizeof *v);
    __try {
        const uint8_t* h = *(const uint8_t* const*)A_SpawnHandlerPtr;
        if (!h) return false;
        unsigned id = *(const unsigned*)(sp + 0x198);
        uint8_t* const* end = *(uint8_t* const* const*)(h + 0x58);
        for (uint8_t* const* e = *(uint8_t* const* const*)(h + 0x50); e != end; e++) {
            uint8_t* first = *(uint8_t* const*)(*e + 0x40);
            uint8_t* last = *(uint8_t* const*)(*e + 0x48);
            if (*(const unsigned*)(*e + 0x20) != id || first == last) continue;
            uint8_t* go = *(uint8_t* const*)first;
            uint8_t* ch = go - 8;
            v->objects = (int)((last - first) / 16); v->vehicles = (*e)[0xA1];
            v->go = go; v->vt = *(const uintptr_t*)go;
            v->hp = *(const float*)(ch + 0x184); v->timer = *(const float*)(ch + 0x520);
            v->god = ch[0xE9]; v->e3 = ch[0xEA]; v->flags = ch[0x432];
            if (v->vt == VT_CHARACTER_GO && CharacterGetVehiclePtr) v->vehicle = CharacterGetVehiclePtr(ch);
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
typedef bool (*AiGoToHandleFn)(void* go, unsigned* handle);
static const AiGoToHandleFn AiGameObjectToEntityHandle = (AiGoToHandleFn)0x140038E30;

// ---- perch gimps (r25, user tests 2026-11-10) ---------------------------------------------------------------------------------------
// The gimp of 3B9F5107 (sd2090) waits on a perch whose navmesh has no link to the ground. With the player in its area of operations its behaviour tree asks
// AiMoveToPoint for a ground point next to the player and the pathfinder answers status 3 (hkaiCharacterUtil::processPathOutput search status 2, unreachable:
// name inferred from the enum strings), several times a second; it never leaps (it is in no interaction graph). r19 moved it to 467.2, still on the perch, and it
// was back up 2 s later; r25 put it on the ground point its own tree asked for (y 462.3): it stayed there, fought and died, and the camp was taken back (two runs).
// The trees are bytecode run by FragmentBundle_F.dll (callback::run_program); every game query goes through AiCallbackHandler(native id, mode, reg, args,
// context), the running entity at *(context + 0x2C0) + 0x228. AiMoveToPoint: mode 0 starts (args[0] = &target), mode 1 polls: 1 reached, 2 waiting for the
// pathfinder's reply, 0 failed. The reply is a 0x60-byte message (type 0x19, request id at +0x10 = the dword at reg, status at +0x14) found with FindIncomingMessage
typedef unsigned (*AiCallbackFn)(unsigned id, int mode, void* reg, void** args, uint8_t* ctx);
static AiCallbackFn AiCallback_orig;
static const uintptr_t FB_AI_CALLBACK = 0x1150, FB_FIND_INCOMING_MESSAGE = 0x4F420;   // RVAs in FragmentBundle_F.dll
typedef uint8_t* (*FindIncomingMessageFn)(void* box, unsigned entity, unsigned type, unsigned idOffset, unsigned id);
static FindIncomingMessageFn FindIncomingMessage;
static const unsigned NAT_MOVE_TO_POINT = 0x8CDA7383;
typedef void (*CharSetTransformFn)(void* go, const float* m);
static const CharSetTransformFn CharSetTransform = (CharSetTransformFn)0x1400CD5F0;   // CCharacter::SetTransform(const CMatrix4f&), this = the CGameObject (matrix at +0x1D0)
static const float PERCH_UNREACHABLE_FOR = 2.5f;   // s of "unreachable" replies, a gap over 1 s starting again (in the r25 test the tree stopped asking for 28 s)
struct AiUnreach { volatile unsigned entity; volatile ULONGLONG since, last; float tx, ty, tz, x, y, z; };   // t* the target started, x y z the one found unreachable
static AiUnreach g_aiUnreach[8];
static AiUnreach* AiUnreachSlot(unsigned h, bool create) {
    AiUnreach* old = &g_aiUnreach[0];
    for (AiUnreach& u : g_aiUnreach) {
        if (u.entity == h) return &u;
        if (u.last < old->last) old = &u;
    }
    if (!create) return nullptr;
    old->since = old->last = 0; old->entity = h;
    return old;
}
#if MM_CAMP_DIAG
static void AiCallbackDiag(unsigned id, int mode, void* reg, void** args, uint8_t* ctx, unsigned r);
#endif
static unsigned AiCallback_hook(unsigned id, int mode, void* reg, void** args, uint8_t* ctx) {
    unsigned r = AiCallback_orig(id, mode, reg, args, ctx);
    if (id == NAT_MOVE_TO_POINT && ctx) {
        __try {
            uint8_t* me = *(uint8_t* const*)(ctx + 0x2C0);
            unsigned h = me ? *(const unsigned*)(me + 0x228) : 0;
            if (h && mode == 0 && args) {
                AiUnreach* u = AiUnreachSlot(h, true);
                const float* p = (const float*)args[0];
                u->tx = p[0]; u->ty = p[1]; u->tz = p[2];
            } else if (h && mode == 1 && r == 1) {
                if (AiUnreach* u = AiUnreachSlot(h, false)) u->since = 0;
            } else if (h && mode == 1 && r == 0 && reg && FindIncomingMessage) {
                const uint8_t* msg = FindIncomingMessage(*(void* const*)(ctx + 0x2B8), *(const unsigned*)(me + 0x224), 0x19, 0x10, *(const unsigned*)reg);
                AiUnreach* u = AiUnreachSlot(h, msg && *(const unsigned*)(msg + 0x14) == 3);
                if (u && msg && *(const unsigned*)(msg + 0x14) == 3) {
                    ULONGLONG now = GetTickCount64();
                    u->x = u->tx; u->y = u->ty; u->z = u->tz;
                    if (!u->since || now - u->last > 1000) u->since = now;
                    u->last = now;
                } else if (u) u->since = 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
#if MM_CAMP_DIAG
    AiCallbackDiag(id, mode, reg, args, ctx, r);
#endif
    return r;
}
static void AiCallbackHookInstall() {
    static bool tried;
    if (tried) return;
    HMODULE fb = GetModuleHandleA("FragmentBundle_F.dll");
    if (!fb) return;
    tried = true;
    uint8_t* fn = (uint8_t*)fb + FB_AI_CALLBACK;
    static const uint8_t expect[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54 };
    if (memcmp(fn, expect, sizeof expect)) { LogLine("CAMPREPOP AiCallbackHandler at %p: unexpected bytes, not hooked (perch gimps stay up)", fn); return; }
    FindIncomingMessage = (FindIncomingMessageFn)((uint8_t*)fb + FB_FIND_INCOMING_MESSAGE);
    MH_STATUS c = MH_CreateHook(fn, (LPVOID)AiCallback_hook, (LPVOID*)&AiCallback_orig);
    MH_STATUS e = c == MH_OK ? MH_EnableHook(fn) : c;
    LogLine("CAMPREPOP AiCallbackHandler hook at %p: %s", fn, MH_StatusToString(e));
}
// once the gimp of a trigger point has been unreachable for PERCH_UNREACHABLE_FOR s: put it on the ground point its tree asked for, and say where it is 4 s later
struct PerchLeave { uint8_t* go; bool moved, logged; ULONGLONG t; };
static PerchLeave g_perchLeave;
static void PerchLeaveStep(const char* camp, unsigned id, uint8_t* go) {
    AiCallbackHookInstall();
    PerchLeave& s = g_perchLeave;
    if (s.go != go) { memset(&s, 0, sizeof s); s.go = go; }
    if (s.logged) return;
    __try {
        ULONGLONG now = GetTickCount64();
        float* p = CharacterPos(go);
        if (!p) return;
        if (s.moved) {
            if (now - s.t < 4000) return;
            s.logged = true;
            LogLine("CAMPREPOP %s: spawn point %08X: perch gimp 4 s later at %.1f %.1f %.1f, player %.1f %.1f %.1f", camp, id, p[0], p[1], p[2], g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z);
            return;
        }
        unsigned h = 0;
        if (!AiGameObjectToEntityHandle(go, &h) || !h) return;
        AiUnreach* u = AiUnreachSlot(h, false);
        ULONGLONG since = u ? u->since : 0, last = u ? u->last : 0;
        if (!since || now - last > 1000 || now - since < (ULONGLONG)(PERCH_UNREACHABLE_FOR * 1000)) return;
        float was[3] = { p[0], p[1], p[2] }, mt[16];
        memcpy(mt, go + 0x1D0, sizeof mt);                            // the world matrix, CharacterPos being its translation row
        mt[12] = u->x; mt[13] = u->y; mt[14] = u->z;
        CharSetTransform(go, mt);
        s.moved = true; s.t = now;
        float* q = CharacterPos(go);
        LogLine("CAMPREPOP %s: spawn point %08X: perch gimp could not reach the ground for %.1f s, moved from %.1f %.1f %.1f to %.1f %.1f %.1f, now at %.1f %.1f %.1f", camp, id,
                (now - since) / 1000.f, was[0], was[1], was[2], mt[12], mt[13], mt[14], q ? q[0] : 0.f, q ? q[1] : 0.f, q ? q[2] : 0.f);
    } __except (EXCEPTION_EXECUTE_HANDLER) { LogLine("CAMPREPOP %s: spawn point %08X: fault while bringing the perch gimp down", camp, id); s.moved = s.logged = true; }
}

#if MM_CAMP_DIAG
// Why a perch gimp never leaps (3B9F5107 of sd2090, tests 2026-11-07/08): CCharacter::PostInit runs the intro interaction (object +0x508, gimp_SetNode) of the intro context (+0x500,
// a context.traversal.high) and the gimp is put on the perch node. It comes down only when its path uses the context's user edge (useredge.down.gimp), and
// CInteractionGraph::UpdateUserEdgesEnabledStatus enables that edge in the pathfinder only if the graph, the context, the edge's entry and its exit are all enabled
// (SAiEntityInstance +0x133 bit 0; entries +0x80 stride 0x80 flag +0x5C; exits +0x88 stride 0x40 flag +0x38; edges +0x90 stride 0x38, count +0x58). What each one holds
typedef uint8_t* (*AiGetInstanceFn)(const void* aiObjectEntity);
static const AiGetInstanceFn AiGetInstance = (AiGetInstanceFn)0x140039000;   // CAiObjectEntity::GetInstance
static const uintptr_t A_AIFragmentManager = 0x141711A28;                     // CAvaSingle<CAIFragmentInstanceManager>::Instance; +0x1FCB0 = CAiPathfinder
static void GimpEdgeProbe(const char* camp, unsigned id, uint8_t* go, float t) {
    __try {
        uint64_t ctxId = *(const uint64_t*)(go + 0x500), interId = *(const uint64_t*)(go + 0x508);
        uint8_t* ctx = ctxId ? (uint8_t*)FindGameObjectByIdAndRelease(ctxId) : nullptr;
        uint8_t* ci = ctx ? AiGetInstance(ctx + 0x3C8) : nullptr;
        int graphs = ctx ? *(const uint16_t*)(ctx + 0x3C6) : 0;
        LogLine("CAMPDIAG %s: edges %08X after %.0f s: intro context %llX interaction %llX, context %p enabled %d, %d graph(s)", camp, id, t,
                (unsigned long long)ctxId, (unsigned long long)interId, ctx, ci ? ci[0x133] & 1 : -1, graphs);
        const uint8_t* fm = *(uint8_t* const*)A_AIFragmentManager;
        const uint8_t* queue = fm ? *(uint8_t* const*)(fm + 0x1FCB0 + 8) : nullptr;
        for (int g = 0; g < graphs && g < 8; g++) {
            uint8_t* gr = *(uint8_t* const*)(ctx + 0x128 + g * 0x10);
            uint8_t* gi = gr ? AiGetInstance(gr + 0x68) : nullptr;
            if (!gi) { LogLine("CAMPDIAG %s: edges %08X graph %d %p: no AI instance", camp, id, g, gr); continue; }
            unsigned ne = *(const unsigned*)(gi + 0x58);
            LogLine("CAMPDIAG %s: edges %08X graph %d %p enabled %d, context %p, %u edge(s)", camp, id, g, gr, gi[0x133] & 1, *(void* const*)(gr + 0x60), ne);
            const uint8_t* edges = *(uint8_t* const*)(gi + 0x90);
            for (unsigned e = 0; e < ne && e < 8; e++) {
                const uint8_t* ed = edges + e * 0x38;
                uint8_t* owner = *(uint8_t* const*)(ed + 8);
                uint8_t* oi = owner ? AiGetInstance(owner + 0x68) : nullptr;
                int en = *(const int*)(ed + 0x18), ex = *(const int*)(ed + 0x20);
                unsigned eid = *(const unsigned*)(ed + 0x28);
                int entryOn = oi ? (*(uint8_t* const*)(oi + 0x80))[en * 0x80 + 0x5C] : -1;
                int exitOn = oi ? (*(uint8_t* const*)(oi + 0x88))[ex * 0x40 + 0x38] : -1;
                int magic = -1, added = -1, disabled = -1;
                if (queue && (eid & 0x7FE0) <= 0x8000) {
                    const uint8_t* slot = queue + (((eid >> 5) & 0x3FF) << 8);
                    magic = *(const unsigned*)(slot + 0xF0) == eid >> 15; added = slot[0xEC]; disabled = slot[0xED];
                }
                LogLine("CAMPDIAG %s: edges %08X graph %d edge %u: valid %d, owner enabled %d, entry %d on %d, exit %d on %d, id %08X status %d, pathfinder slot ok %d added %d disabled %d",
                        camp, id, g, e, ed[0x10], oi ? oi[0x133] & 1 : -1, en, entryOn, ex, exitOn, eid, ed[0x2C], magic, added, disabled);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { LogLine("CAMPDIAG %s: edges %08X: fault while reading", camp, id); }
}
// CAIFragmentInstanceManager (*0x141711A28): +0x20 entity registry (AiEntityGet), +0x580 the 64 tree instances (stride 0x280: +0 instance id, +8 entity handle, +0x48 return value,
// GetTreeName), +0x1FE30 the entity blackboard (256 entries of 16 bytes: entity handle, key hash, type, value; the first with handle 0 ends it). The area of operations' own handle is
// CAreaOfOperationsObject +0x128 (its CAiObjectEntity at +0x120)
typedef uint8_t* (*AiEntityGetFn)(void* registry, unsigned handle, bool);
static const AiEntityGetFn AiEntityGetPtr = (AiEntityGetFn)0x1409AD720;
typedef const char* (*AiGetTreeNameFn)(void* manager, unsigned instanceId);
static const AiGetTreeNameFn AiGetTreeName = (AiGetTreeNameFn)0x140039780;
static void AiHexDump(const char* camp, unsigned id, const char* what, const uint8_t* p, int size) {
    for (int o = 0; o < size; o += 32) {
        char line[160]; int n = 0;
        for (int b = 0; b < 32; b += 4) n += sprintf_s(line + n, sizeof line - n, " %08X", *(const unsigned*)(p + o + b));
        LogLine("CAMPDIAG %s: ai %08X %s +%03X:%s", camp, id, what, o, line);
    }
}
// r20/r21 tests 2026-11-08: both gimps of sd2090 run universal_soldier.bts; giving the perch gimp the blackboard flag 60931C29 the other one gets did nothing.
// The trees are bytecode for a small VM (callback::run_program in FragmentBundle_F.dll, which ships with its PDB); every game query the tree makes goes through
// AiCallbackHandler(native id, mode, reg, args, context), the id selecting one of ~430 natives (AiPositionInsideAoo, AiIsEntityBlackboardFlagSet, AiMoveToPoint...).
// What the tree of each watched gimp asks and gets back, counted between two samples: the id, mode and result of every call; for the area test the position checked
#if MM_CAMP_DIAG
static const unsigned NAT_INSIDE_AOO = 0x6A48A86A, NAT_FLAG_IS_SET = 0x94C7D2C6, NAT_FLAG_SET = 0x466FAE71;
// r22 test 2026-11-09: under the perch the area test passes for both gimps; the perch gimp's AiMoveToPoint gets "failed" back from the game
// (~7 a second) where the ground gimp's runs. The reply is a 0x60-byte message (type 0x19, request id at +0x10, status at +0x14) that
// AiMoveToPoint looks up with FindIncomingMessage; the request id is the dword at reg
struct AiMoveSeen { volatile LONG starts, fails; float tx, ty, tz, a1, a2; unsigned reqId; uint32_t reply[24]; };
static AiMoveSeen g_aiMove[2];
static volatile unsigned g_aiWatch[2];                    // entity handles of g_aiProbePoints[0], [1]
struct AiNatCount { volatile LONG64 key; volatile LONG count; };   // key = id << 32 | mode << 8 | result
static AiNatCount g_aiNat[2][512];
struct AiAooSeen { volatile LONG n; float x, y, z; unsigned area, result; };
static AiAooSeen g_aiAoo[2][4][2];                        // [type][result != 0]
static volatile LONG g_aiCalls, g_aiMatched;
static volatile unsigned g_aiSeen[8];
static void AiNatAdd(int w, unsigned id, unsigned mode, unsigned result) {
    LONG64 k = (LONG64)id << 32 | (mode & 0xFF) << 8 | (result > 0xFF ? 0xFF : result);
    unsigned h = (unsigned)((k * 0x9E3779B97F4A7C15ull) >> 55);   // 9 bits
    for (int i = 0; i < 512; i++) {
        AiNatCount& e = g_aiNat[w][(h + i) & 511];
        LONG64 cur = e.key;
        if (cur == 0) cur = InterlockedCompareExchange64(&e.key, k, 0);
        if (cur == 0 || cur == k) { InterlockedIncrement(&e.count); return; }
    }
}
static void AiCallbackDiag(unsigned id, int mode, void* reg, void** args, uint8_t* ctx, unsigned r) {
    __try {
        InterlockedIncrement(&g_aiCalls);
        uint8_t* me = ctx ? *(uint8_t* const*)(ctx + 0x2C0) : nullptr;  // AiGetMyEntity: context +0x2C0, +0x228 = the running entity
        unsigned h = me ? *(const unsigned*)(me + 0x228) : 0;
        if (h) g_aiSeen[(h >> 3) & 7] = h;
        int w = h && h == g_aiWatch[0] ? 0 : h && h == g_aiWatch[1] ? 1 : -1;
        if (w < 0) return;
        InterlockedIncrement(&g_aiMatched);
        if ((id == NAT_FLAG_IS_SET || id == NAT_FLAG_SET) && args) {   // args: &handle, &key[, &value]: counted by key, mode E0/F0 + 1 when it is its own flag
            unsigned fh = *(const unsigned*)args[0], key = *(const unsigned*)args[1];
            AiNatAdd(w, key, (id == NAT_FLAG_IS_SET ? 0xE0 : 0xF0) | (fh == h), id == NAT_FLAG_SET ? (*(const unsigned*)args[2] != 0) : r);
            return;
        }
        AiNatAdd(w, id, (unsigned)mode, r);
        if (id == NAT_MOVE_TO_POINT && args && mode == 0) {           // args: &target, &float, &float, &bool, &bool
            AiMoveSeen& m = g_aiMove[w];
            const float* p = (const float*)args[0];
            m.tx = p[0]; m.ty = p[1]; m.tz = p[2]; m.a1 = *(const float*)args[1]; m.a2 = *(const float*)args[2];
            InterlockedIncrement(&m.starts);
        }
        if (id == NAT_MOVE_TO_POINT && mode == 1 && r == 0 && reg && me) {
            AiMoveSeen& m = g_aiMove[w];
            unsigned req = *(const unsigned*)reg;
            const uint8_t* msg = FindIncomingMessage(*(void* const*)(ctx + 0x2B8), *(const unsigned*)(me + 0x224), 0x19, 0x10, req);
            m.reqId = req;
            if (msg) memcpy(m.reply, msg, sizeof m.reply);
            else memset(m.reply, 0, sizeof m.reply);
            InterlockedIncrement(&m.fails);
        }
        if (id == NAT_INSIDE_AOO && args) {                            // args: &area handle, &position, &box type
            unsigned type = *(const unsigned*)args[2];
            const float* p = (const float*)args[1];
            AiAooSeen& a = g_aiAoo[w][type & 3][r != 0];
            a.x = p[0]; a.y = p[1]; a.z = p[2]; a.area = *(const unsigned*)args[0]; a.result = r;
            InterlockedIncrement(&a.n);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void AiNatDump(const char* camp, unsigned id, int w) {
    struct Row { LONG64 key; LONG n; } rows[512];
    int nr = 0;
    for (int i = 0; i < 512; i++) {
        LONG n = InterlockedExchange(&g_aiNat[w][i].count, 0);
        if (n) { rows[nr].key = g_aiNat[w][i].key; rows[nr].n = n; nr++; }
    }
    for (int i = 1; i < nr; i++)                              // by count, then id
        for (int j = i; j > 0 && (rows[j].n > rows[j - 1].n || (rows[j].n == rows[j - 1].n && rows[j].key < rows[j - 1].key)); j--) { Row t = rows[j]; rows[j] = rows[j - 1]; rows[j - 1] = t; }
    LogLine("CAMPDIAG %s: ai %08X natives: %d distinct (hook calls %ld, this gimp %ld)", camp, id, nr, g_aiCalls, g_aiMatched);
    for (int i = 0; i < nr; i += 8) {
        char line[400]; int n = 0;
        for (int k = i; k < nr && k < i + 8; k++)
            n += sprintf_s(line + n, sizeof line - n, " %08X.%X.%u x%ld", (unsigned)(rows[k].key >> 32), (unsigned)(rows[k].key >> 8) & 0xFF, (unsigned)rows[k].key & 0xFF, rows[k].n);
        LogLine("CAMPDIAG %s: ai %08X nat%s", camp, id, line);
    }
    for (int t = 0; t < 4; t++)
        for (int k = 0; k < 2; k++) {
            AiAooSeen& a = g_aiAoo[w][t][k];
            LONG n = InterlockedExchange(&a.n, 0);
            if (n) LogLine("CAMPDIAG %s: ai %08X inside-area type %d x%ld: area %08X position %.1f %.1f %.1f -> %u", camp, id, t, n, a.area, a.x, a.y, a.z, a.result);
        }
    AiMoveSeen& m = g_aiMove[w];
    LONG ms = InterlockedExchange(&m.starts, 0), mf = InterlockedExchange(&m.fails, 0);
    if (ms || mf) {
        LogLine("CAMPDIAG %s: ai %08X move-to-point started x%ld, failed x%ld: last target %.1f %.1f %.1f (%.2f %.2f)", camp, id, ms, mf, m.tx, m.ty, m.tz, m.a1, m.a2);
        if (mf) {
            const uint32_t* q = m.reply;
            LogLine("CAMPDIAG %s: ai %08X move-to-point failure reply (request %08X): %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X", camp, id, m.reqId,
                    q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11]);
            LogLine("CAMPDIAG %s: ai %08X move-to-point failure reply +30: %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X", camp, id,
                    q[12], q[13], q[14], q[15], q[16], q[17], q[18], q[19], q[20], q[21], q[22], q[23]);
        }
    }
    if (!g_aiMatched)
        LogLine("CAMPDIAG %s: ai %08X: no call matched; handles seen %08X %08X %08X %08X %08X %08X %08X %08X", camp, id,
                g_aiSeen[0], g_aiSeen[1], g_aiSeen[2], g_aiSeen[3], g_aiSeen[4], g_aiSeen[5], g_aiSeen[6], g_aiSeen[7]);
}
#endif
static void AiProbe(const char* camp, unsigned id, uint8_t* go, float t) {
    static uint8_t* dumped[8];
    static int nd;
    __try {
        uint8_t* mgr = *(uint8_t* const*)A_AIFragmentManager;
        unsigned h = 0;
        if (!mgr || !AiGameObjectToEntityHandle(go, &h)) { LogLine("CAMPDIAG %s: ai %08X after %.0f s: no entity handle", camp, id, t); return; }
        uint8_t* inst = AiEntityGetPtr(mgr + 0x20, h, false);
        float* p = CharacterPos(go);
        LogLine("CAMPDIAG %s: ai %08X after %.0f s: character %p at %.1f %.1f %.1f, player %.1f %.1f %.1f, entity %08X instance %p type %d flags %02X", camp, id, t, go,
                p ? p[0] : 0.f, p ? p[1] : 0.f, p ? p[2] : 0.f, g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z, h, inst, inst ? *(const int*)(inst + 0x128) : -1, inst ? inst[0x133] : 0);
        bool first = true;
        for (int i = 0; i < nd; i++) if (dumped[i] == go) first = false;
        if (first && nd < 8) dumped[nd++] = go;
        if (first && inst) AiHexDump(camp, id, "instance", inst, 0x140);
        for (int sl = 0; sl < 64; sl++) {
            const uint8_t* slot = mgr + 0x580 + sl * 0x280;
            if (*(const unsigned*)(slot + 8) != h) continue;
            unsigned tid = *(const unsigned*)slot;
            const char* name = AiGetTreeName(mgr, tid);
            LogLine("CAMPDIAG %s: ai %08X tree slot %d id %08X \"%s\" return %u", camp, id, sl, tid, name ? name : "?", *(const unsigned*)(slot + 0x48));
            AiHexDump(camp, id, "tree slot", slot, first ? 0x280 : 0x60);
        }
        uint8_t* aoo = (uint8_t*)FindGameObjectByIdAndRelease(0xD44CB6AA);
        unsigned aooH = aoo ? *(const unsigned*)(aoo + 0x128) : 0;
        const uint8_t* bb = mgr + 0x1FE30;
        int shown = 0;
        for (int e = 0; e < 256; e++) {
            const unsigned* en = (const unsigned*)(bb + e * 16);
            if (!en[0]) break;
            if (en[0] != h && (!aooH || en[0] != aooH)) continue;
            LogLine("CAMPDIAG %s: ai %08X blackboard %s key %08X type %u value %08X", camp, id, en[0] == h ? "own" : "area", en[1], en[2], en[3]);
            shown++;
        }
        LogLine("CAMPDIAG %s: ai %08X area of operations %p handle %08X, %d blackboard entries shown", camp, id, aoo, aooH, shown);
#if MM_CAMP_DIAG
        AiCallbackHookInstall();
        for (int w = 0; w < 2; w++)
            if (g_aiProbePoints[w] == id) {
                if (g_aiWatch[w] != h) { g_aiWatch[w] = h; for (int i = 0; i < 512; i++) { g_aiNat[w][i].count = 0; g_aiNat[w][i].key = 0; } }
                else AiNatDump(camp, id, w);
            }
#endif
    } __except (EXCEPTION_EXECUTE_HANDLER) { LogLine("CAMPDIAG %s: ai %08X: fault while reading", camp, id); }
}
// a line on every change and on every sample: a new character here means it died and came back, the same one with its health going down means it is being hit
struct LastEnemy { unsigned id; EnemyVitals v; };
static LastEnemy g_lastEnemy[4];
static void LastEnemyWatch(const char* camp, unsigned id, uint8_t* sp, int chars, int dead, bool sample) {
    static int next;
    LastEnemy* e = nullptr;
    for (LastEnemy& s : g_lastEnemy) if (s.id == id) e = &s;
    if (!e) { e = &g_lastEnemy[next++ & 3]; memset(e, 0, sizeof *e); e->id = id; }
    EnemyVitals v;
    EnemyVitalsRead(sp, &v);
    bool same = v.go == e->v.go && fabsf(v.hp - e->v.hp) < 0.001f && v.god == e->v.god && v.e3 == e->v.e3 && v.vehicle == e->v.vehicle;
    if (same && !sample) return;
    float* p = v.go ? CharacterPos(v.go) : nullptr;
    LogLine("CAMPDIAG %s: enemy %08X%s character %p vt %llX health %.3f god %d e3 %d flags %02X timer %.1f vehicle %p, %d objects %d vehicles, %d alive %d dead, at %.1f %.1f %.1f, player %.1f %.1f %.1f",
            camp, id, same ? "" : " CHANGED", v.go, (unsigned long long)v.vt, v.hp, v.god, v.e3, v.flags, v.timer, v.vehicle, v.objects, v.vehicles, chars, dead,
            p ? p[0] : 0.f, p ? p[1] : 0.f, p ? p[2] : 0.f, g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z);
    e->v = v;
}
static int SpawnStatusNewId(uint8_t* go) {
    __try { return (int)((unsigned (*)(const void*))A_SpawnSystemGetStatus)(go + 0x18C); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
// SLocationInfoEntry table (LocationInfoGetEntry 0x1403E0BE0): 0x60 bytes each, key +0x30, the whole entry goes to the save and comes back on load.
// CGameMapIcon::ShowIcon reads it: main mission done = +0x42 && !+0x43 (LocationInfoIsCampMainMissionCompleted), cleared = the 11 bytes +0x35, +0x37...
// are 0 (LocationInfoIsCampCleared). The wcdrum1 dev build (2026-10-16) set +0x43 on wc1020 (index 41) with no save handling.
static void CampDiagLocTable() {
    __try {
        const uint8_t* t = *(const uint8_t* const*)A_LocationInfoTable;
        if (!t) { LogLine("CAMPDIAG location table: not loaded"); return; }
        const uint8_t* e = *(const uint8_t* const*)t; unsigned count = *(const unsigned*)(t + 8);
        LogLine("CAMPDIAG location table: %u entries", count);
        for (unsigned i = 0; e && i < count && i < 1024; i++, e += 0x60) {
            int left = 0;
            for (int k = 0; k < 11; k++) if (e[0x35 + 2 * k]) left++;
            if (e[0x42] || e[0x43] || left)
                LogLine("CAMPDIAG entry %u key %08X +42 %d +43 %d, objectives left %d, main mission done %d", i, *(const unsigned*)(e + 0x30), e[0x42], e[0x43], left, e[0x42] && !e[0x43]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { LogLine("CAMPDIAG location table: exception"); }
}
static void CampDiagGroup(const char* camp, const char* what, unsigned locId) {
    uint8_t* sp[48]; unsigned ids[48];
    int n = LocSpawnPoints(locId, sp, ids, 48), on = 0, live = 0;
    for (int k = 0; k < n; k++) { if (sp[k][0x232]) on++; if (SpawnStatusNewId(sp[k] + 0x10) > 0) live++; }
    LogLine("CAMPDIAG %s: %s location %d spawn points, %d on, %d with a live spawn", camp, what, n, on, live);
}
static void CampDiagTick(float step) {
    static float acc, trackAcc;
    static struct { unsigned id; int on, st, stNew; } seen[sizeof g_camps / sizeof g_camps[0]][32];
    acc += step; trackAcc += step;
    if (acc < 3.f) return;
    acc = 0.f;
    const int N = sizeof g_camps / sizeof g_camps[0];
    bool anyNear = false;
    for (int i = 0; i < N; i++) {
        const CampEntry& c = g_camps[i]; RepopCamp& r = g_repop[i];
        float d = CampDistance(c);
        if (d > 500.f) continue;
        anyNear = true;
        int el, cl, en = LocEnabled(c.enemyId, &el), cv = LocEnabled(c.civilianId, &cl);
        LogLine("CAMPDIAG %s: %s, %.0f m, player %.0f %.0f %.0f, enemy on %d loaded %d, civilians on %d loaded %d, tracked %d, spawned mask %08X",
                c.name, RP_NAMES[r.state], d, g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z, en, el, cv, cl, r.revivedCount, (unsigned)r.spawnedMask);
        if (const uint8_t* h = *(const uint8_t* const*)A_SpawnHandlerPtr) LogLine("CAMPDIAG spawn budget: in use %d, limit %d, default %d", h[0x1667], h[0x1669], h[0x166B]);
        CampDiagGroup(c.name, "civilian", c.civilianId);
        CampDiagGroup(c.name, "base", g_campBase[i]);
        uint8_t* sp[32]; unsigned ids[32];
        int n = LocSpawnPoints(c.enemyId, sp, ids, 32);
        for (int k = 0; k < n; k++) {
            int on = sp[k][0x232], st = SpawnStatus(sp[k] + 0x10), stNew = SpawnStatusNewId(sp[k] + 0x10);
            auto& s = seen[i][k];
            if (s.id == ids[k] && s.on == on && s.st == st && s.stNew == stNew) continue;
            float px = 0.f, pz = 0.f; bool have = SpawnPointXZ(sp[k], c.x, c.z, &px, &pz);
            float dd = have ? sqrtf((g_PlayerPos.x - px) * (g_PlayerPos.x - px) + (g_PlayerPos.z - pz) * (g_PlayerPos.z - pz)) : -1.f;
            LogLine("CAMPDIAG %s: point %08X on %d status %d new-id status %d, spawn distance %.0f, %.0f m from the player", c.name, ids[k], on, st, stNew, *(float*)(sp[k] + 0x168), dd);
            s.id = ids[k]; s.on = on; s.st = st; s.stNew = stNew;
        }
    }
    if (!anyNear && trackAcc >= 15.f) { trackAcc = 0.f; LogLine("CAMPDIAG player %.0f %.0f %.0f", g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z); }
}
#endif

// One-off repair of Pablo's save (user 2026-11-14: Gasworks shown as taken, no enemies). The wcdrum1 dev build (2026-10-16) set the
// main mission's "left" byte (+0x43, pair 7 of the tracked items) of wc1020 Gasworks (key BABF660A, entry 41) to 1 while repopulating
// it, and the game saved it; before that the entry read +0x42 1 +0x43 0 (completed). Never in a release: in another player's save
// an unfinished Gasworks has the same bytes.
#define MM_SAVEFIX_GASWORKS 0
#if MM_SAVEFIX_GASWORKS
static void GasworksSaveFix() {
    __try {
        const uint8_t* t = *(const uint8_t* const*)A_LocationInfoTable;
        uint8_t* e = t ? *(uint8_t* const*)t : nullptr;
        unsigned count = t ? *(const unsigned*)(t + 8) : 0;
        for (unsigned i = 0; e && i < count && i < 1024; i++, e += 0x60) {
            if (*(const unsigned*)(e + 0x30) != 0xBABF660Au) continue;
            LogLine("SAVEFIX Gasworks (wc1020): entry %u, main mission total %d, left %d", i, e[0x42], e[0x43]);
            if (e[0x42] == 1 && e[0x43] == 1) { e[0x43] = 0; LogLine("SAVEFIX Gasworks: left set to 0 (main mission done); save the game to keep it"); }
            return;
        }
        LogLine("SAVEFIX Gasworks: entry BABF660A not found (%u entries)", count);
    } __except (EXCEPTION_EXECUTE_HANDLER) { LogLine("SAVEFIX Gasworks: exception"); }
}
#endif

// Diagnostic (user 2026-11-14: Gob Stone sd1010 retaken with no fight; log: its 15 points went off 11 s after the enemy location
// loaded, player 332 m away, nothing spawned). Every point of a held camp switched off, with the exe return addresses on the stack
// (resolve with pdb_symbols.json): the game's event chain that did it. Static: all 15 points listen to B156552B, sent by
// evt.scrotus_camp_enemies.despawn and by the CArenaSpawner (its 214B0328), like Dead End's 94204868.
// r33 (user 2026-11-14, after the r32 test: "faca testes automaticos para entender"): r32's stacks all start in
// CEventSystem::DispatchPostedEvents, a posted event whose sender is gone. So every post is remembered per event id: the event being
// dispatched when it was posted (its parent), the poster's exe return addresses, and for direct sends the event they ran inside; the
// POINTDIAG line then walks that chain back (EVDIAG lines). Static guess to prove or refute: Gob Stone's base location holds
// evr.check_if_camp_completed (saved 1 once liberated) -> 1EAB8947 -> gsr.fuel_cam_control (fuel_center_cam_control.gsr, the camp
// completion screen) sends 72EDE02F -> etc.camp_enemies_despawn sends B156552B -> all 15 points off. Those ids are logged live.
// CEventImpl (0x18 bytes, the system's array at +0x80): +0xC event hash, +8 scope (GetSortId, GetEventFromCache key scope:hash).
// A posted CEventBase points at a descriptor: u16 0, u16 n, then n (hash, scope) pairs; or a registered event: n = byte +1 & 0x7F,
// u16 indexes into that array from +2 (CEventSystem::DispatchSendEvents).
#define MM_POINT_DIAG 0
#if MM_POINT_DIAG
static const uintptr_t A_EventImplSend = 0x1409C25D0;     // NEvent::CEventImpl::Send(const CEventData&)
static const uintptr_t A_EventSystemPost = 0x1409C4600;   // CEventSystem::PostEvent(CEventBase*, const CEventData&)
struct EvRecord { unsigned hash, scope, parent, sendParent; DWORD postTick, sendTick; uintptr_t poster[8]; };
static EvRecord g_evRec[16384];
static thread_local unsigned t_evStack[16];
static thread_local int t_evDepth;
static const unsigned g_evWatch[] = { 0xB156552B, 0x72EDE02F, 0x1EAB8947, 0x2E0E9A22, 0xDAF47E0B, 0x42EEAA53, 0x49B93A7D, 0xEB9E3BB3, 0xC6B60C08,
                                      0xCE62E1E8, 0xC1A16ED8, 0x2939A218, 0x8500DE52, 0xA11B3196, 0xACC7E7F3 };
// r38 autotest F (2026-11-15 13:28): Finger Peak's enemy location went off 1.8 s after the third kill, sent by a Timer of its base
// location's gsr.fuel_cam_control (fuel_center_cam_control.gsr, the camp completion; 72EDE02F despawns the enemies, 6E8F303A ->
// 673228D7 switches the location off). Its three Start nodes take CE62E1E8, C1A16ED8 (evt.location_100_percent, sent by the
// location_info_tracked_item_completion graphs) and 2939A218 (the CPerimeterSystemObject BDAB949B, save=1, with 8500DE52, A11B3196,
// ACC7E7F3; disable.on.camp.completion listens to 8500DE52 + 2939A218): which one started it, and the chain behind it
static bool EvChainWatch(unsigned h) { return h == 0xCE62E1E8 || h == 0xC1A16ED8 || h == 0x2939A218 || h == 0x8500DE52 || h == 0xA11B3196 || h == 0xACC7E7F3; }
static void EvChain(const char* camp, unsigned h);
// r35: EB9E3BB3 fires a01_sd1010.blo's CForcePulse pulse_kill_sniper (and many tower objects'), C6B60C08 its constraints: generic hashes
// every tower uses, so these two are logged only near Gob Stone
static const float EV_NEAR_ONLY_R = 700.f;
static volatile LONG g_evWatchLines;
// r33 first autotest (17:32): the plugin log went silent from the loading screen on (STALL, no crash). Stack captures and log lines
// from inside these hooks on any thread are the suspects, so both now happen only on the game thread (the one RepopTick runs on, 0
// until the first frame in the world) and at most EV_CAPTURES_PER_S stack captures a second; elsewhere only the ids are recorded.
static volatile DWORD g_evMainThread;
static const LONG EV_CAPTURES_PER_S = 300;
static volatile LONG g_evCaptures; static volatile DWORD g_evCaptureSecond;
static bool EvOnMainThread() { return g_evMainThread && GetCurrentThreadId() == g_evMainThread; }
static bool EvMayCapture() {
    if (!EvOnMainThread()) return false;
    DWORD s = GetTickCount() / 1000;
    if (s != g_evCaptureSecond) { g_evCaptureSecond = s; g_evCaptures = 0; }
    return ++g_evCaptures <= EV_CAPTURES_PER_S;
}
static unsigned EvCurrent() { return t_evDepth > 0 ? t_evStack[(t_evDepth > 16 ? 16 : t_evDepth) - 1] : 0; }
static EvRecord& EvSlot(unsigned h) {
    EvRecord& s = g_evRec[(h ^ (h >> 14)) & 16383];
    if (s.hash != h) { memset(&s, 0, sizeof s); s.hash = h; }
    return s;
}
static float EvGobStoneDistance() {
    for (const CampEntry& c : g_camps)
        if (strstr(c.name, "sd1010")) { float dx = g_PlayerPos.x - c.x, dz = g_PlayerPos.z - c.z; return sqrtf(dx * dx + dz * dz); }
    return -1.f;
}
static void EvFormat(const uintptr_t* a, int n, char* out, size_t size) {
    size_t len = 0; out[0] = 0;
    for (int k = 0; k < n && a[k] && len + 12 < size; k++) len += snprintf(out + len, size - len, " %llX", (unsigned long long)a[k]);
}
static void EvWatchLog(const char* what, unsigned h, unsigned scope, unsigned parent, const uintptr_t* poster) {
    bool watched = false;
    for (unsigned w : g_evWatch) if (w == h) watched = true;
    if (watched && (h == 0xEB9E3BB3 || h == 0xC6B60C08)) { float d = EvGobStoneDistance(); watched = d >= 0.f && d < EV_NEAR_ONLY_R; }
    if (!watched || !EvOnMainThread() || InterlockedIncrement(&g_evWatchLines) > 400) return;
    char c[160] = "";
    if (poster) EvFormat(poster, 8, c, sizeof c);
    LogLine("EVDIAG %s %08X (scope %08X) inside %08X, player %.0f m from Gob Stone%s%s", what, h, scope, parent, EvGobStoneDistance(), c[0] ? "; poster" : "", c);
    if (EvChainWatch(h) && parent) EvChain("watch", parent);
}
static void EvRecordPost(unsigned h, unsigned scope, unsigned parent, const uintptr_t* poster) {
    EvRecord& s = EvSlot(h);
    s.scope = scope; s.parent = parent; s.postTick = GetTickCount();
    memcpy(s.poster, poster, sizeof s.poster);
    EvWatchLog("post", h, scope, parent, poster);
}
DEFHOOK(void, EventSystemPost, (uint8_t* sys, const uint8_t* const* ev, const void* data)) {
    __try {
        const uint8_t* d = ev ? ev[0] : nullptr;
        if (d) {
            uintptr_t poster[8] = { (uintptr_t)_ReturnAddress() }; void* f[16]; int n = 0;
            if (EvMayCapture()) {
                USHORT c = RtlCaptureStackBackTrace(1, 16, f, NULL);
                for (USHORT k = 0; k < c && n < 8; k++)
                    if ((uintptr_t)f[k] >= 0x140000000ull && (uintptr_t)f[k] < 0x141AA8000ull) poster[n++] = (uintptr_t)f[k];
            }
            unsigned parent = EvCurrent();
            if (*(const uint16_t*)d == 0) {
                unsigned cnt = *(const uint16_t*)(d + 2);
                for (unsigned k = 0; k < cnt && k < 8; k++)
                    EvRecordPost(*(const unsigned*)(d + 4 + 8 * k), *(const unsigned*)(d + 8 + 8 * k), parent, poster);
            } else {
                unsigned cnt = d[1] & 0x7F;
                for (unsigned k = 0; k < cnt && k < 8; k++) {
                    const uint8_t* impl = sys + 0x80 + (size_t)*(const uint16_t*)(d + 2 + 2 * k) * 0x18;
                    EvRecordPost(*(const unsigned*)(impl + 0xC), *(const unsigned*)(impl + 8), parent, poster);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    EventSystemPost_orig(sys, ev, data);
}
DEFHOOK(void, EventImplSend, (uint8_t* impl, const void* data)) {
    unsigned h = 0, scope = 0, parent = EvCurrent();
    __try { h = *(const unsigned*)(impl + 0xC); scope = *(const unsigned*)(impl + 8); } __except (EXCEPTION_EXECUTE_HANDLER) { h = 0; }
    if (h) {
        EvRecord& s = EvSlot(h);
        s.sendParent = parent; s.sendTick = GetTickCount();
        // r38b: 2939A218 came "sent inside 00000000", a direct send from code: who sent it
        uintptr_t sender[8] = {}; int n = 0;
        if (EvChainWatch(h) && EvOnMainThread()) {
            void* f[24];
            USHORT c = RtlCaptureStackBackTrace(1, 24, f, NULL);
            for (USHORT k = 0; k < c && n < 8; k++)
                if ((uintptr_t)f[k] >= 0x140000000ull && (uintptr_t)f[k] < 0x141AA8000ull) sender[n++] = (uintptr_t)f[k];
        }
        EvWatchLog("sent", h, scope, parent, n ? sender : nullptr);
    }
    if (t_evDepth < 16) t_evStack[t_evDepth] = h;
    t_evDepth++;
    EventImplSend_orig(impl, data);
    t_evDepth--;
}
// the chain behind the event now being dispatched: a direct send leads to the event it ran inside, a posted one to the event being
// dispatched when it was posted (0: posted from outside any event, e.g. a graph script's own update or a timer; the poster shows who)
static void EvChain(const char* camp, unsigned h) {
    DWORD now = GetTickCount();
    for (int n = 0; n < 8 && h; n++) {
        const EvRecord& s = g_evRec[(h ^ (h >> 14)) & 16383];
        if (s.hash != h) { LogLine("EVDIAG %s:   %08X: no record", camp, h); return; }
        char c[160]; EvFormat(s.poster, 8, c, sizeof c);
        LogLine("EVDIAG %s:   %08X (scope %08X): sent %.1f s ago inside %08X; posted %.1f s ago inside %08X, poster%s", camp, h, s.scope,
            s.sendTick ? (now - s.sendTick) / 1000.f : -1.f, s.sendParent, s.postTick ? (now - s.postTick) / 1000.f : -1.f, s.parent, c);
        unsigned next = s.sendParent ? s.sendParent : s.parent;
        h = next == h ? 0 : next;
    }
}
DEFHOOK(void, SpawnPointEnable, (uint8_t* go, bool on)) {
    unsigned id = 0; uint8_t was = 0;
    __try { id = *(const unsigned*)(go + 0x14); was = go[0x222]; } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
    SpawnPointEnable_orig(go, on);
    if (on || !was || !id || !g_repopInit) return;
    const int N = sizeof g_camps / sizeof g_camps[0];
    for (int i = 0; i < N; i++) {
        const RepopCamp& r = g_repop[i];
        if (r.state != RP_PENDING && r.state != RP_ACTIVE) continue;
        for (int j = 0; j < r.revivedCount; j++) {
            if (r.revivedIds[j] != id) continue;
            void* f[24]; char callers[320]; size_t len = 0; callers[0] = 0;
            USHORT c = RtlCaptureStackBackTrace(1, 24, f, NULL);
            for (USHORT k = 0; k < c && len + 12 < sizeof callers; k++) {
                uintptr_t a = (uintptr_t)f[k];
                if (a >= 0x140000000ull && a < 0x141AA8000ull) len += snprintf(callers + len, sizeof callers - len, " %llX", (unsigned long long)a);
            }
            float dx = g_PlayerPos.x - g_camps[i].x, dz = g_PlayerPos.z - g_camps[i].z;
            unsigned ev = EvCurrent();
            LogLine("POINTDIAG %s: spawn point %08X switched off by the game (event %08X), player %.0f m away; callers%s", g_camps[i].name, id, ev,
                sqrtf(dx * dx + dz * dz), callers);
            // the chain once per event and camp within 30 s (15 points go off on one despawn event)
            static unsigned lastEv; static int lastCamp = -1; static DWORD lastTick;
            if (ev != lastEv || i != lastCamp || GetTickCount() - lastTick > 30000) {
                lastEv = ev; lastCamp = i; lastTick = GetTickCount();
                EvChain(g_camps[i].name, ev);
            }
            return;
        }
    }
}
// r38 (user test 2026-11-15: Finger Peak sd1240 retaken by the game itself after 2 kills, 10 enemies alive: "enemy location switched
// off by the game"; the same as sd1270 / sd1290 on 2026-11-06 and Hollow Point): SLocationInfo::HandleEvent switches a location on with
// the event at +0x70 and off with the one at +0x78 (bit 0 of +0x67). Every held camp's enemy location switched off is logged with the
// event, the exe callers and the chain behind the event (EvChain)
static const uintptr_t A_LocInfoHandleEvent = 0x14066C430;   // SLocationInfo::HandleEvent(const CEventImpl*, const CEventData&)
DEFHOOK(void, LocInfoHandleEvent, (uint8_t* info, const uint8_t* impl, const void* data)) {
    uint8_t was = 0;
    __try { was = info[0x67]; } __except (EXCEPTION_EXECUTE_HANDLER) { was = 0; }
    LocInfoHandleEvent_orig(info, impl, data);
    uint8_t now = 1;
    __try { now = info[0x67]; } __except (EXCEPTION_EXECUTE_HANDLER) { now = 1; }
    if (!(was & 1) || (now & 1) || !g_repopInit) return;
    unsigned h = 0, scope = 0;
    __try { h = *(const unsigned*)(impl + 0xC); scope = *(const unsigned*)(impl + 8); } __except (EXCEPTION_EXECUTE_HANDLER) { h = 0; }
    const int N = sizeof g_camps / sizeof g_camps[0];
    for (int i = 0; i < N; i++) {
        uint8_t* l[4]; int n = FindLocations(g_camps[i].enemyId, l, 4);
        bool mine = false;
        for (int k = 0; k < n; k++) if (l[k] == info) mine = true;
        if (!mine) continue;
        void* f[24]; char callers[320]; size_t len = 0; callers[0] = 0;
        USHORT c = RtlCaptureStackBackTrace(1, 24, f, NULL);
        for (USHORT k = 0; k < c && len + 12 < sizeof callers; k++) {
            uintptr_t a = (uintptr_t)f[k];
            if (a >= 0x140000000ull && a < 0x141AA8000ull) len += snprintf(callers + len, sizeof callers - len, " %llX", (unsigned long long)a);
        }
        LogLine("LOCDIAG %s: enemy location switched off by the game (event %08X scope %08X, camp %s), player %.0f m away; callers%s", g_camps[i].name, h, scope,
            RP_NAMES[g_repop[i].state], CampDistance(g_camps[i]), callers);
        EvChain(g_camps[i].name, h);
        return;
    }
}
// r35 lookout diagnostic (2026-11-14, autotest B 20:39: lookout F408814D still died 2 s after spawning with its tower parts rebuilt, all
// 8 had loaded switched off, none broken): where the lookout is and its health every 0.1 s for 6 s after the repop tick first sees it,
// the tower parts' flags then, and every force pulse fired near Gob Stone with what it hits (a01_sd1010.blo has a CForcePulse
// pulse_kill_sniper 7804E333, ACT_HIT_BY_IMPACT, fired by EB9E3BB3: is it what kills the lookout?)
// r36: r35 read 0,0,0 and health 0 for 6 s (the object was not checked to be a character): now the spawn point's entity is read again
// at every sample (EnemyVitalsRead) and position / health only for a CCharacter
struct LookWatch { uint8_t* go; const uint8_t* sp; unsigned id; int camp; float t, next; };
static LookWatch g_lw;
static volatile LONG g_pulseLines;
static void TowerPartsDiag(int i) {
    const RepopCamp& r = g_repop[i];
    uint8_t* l[4]; int n = FindLocations(g_campBase[i], l, 4);
    char out[400]; size_t len = 0; out[0] = 0;
    for (int j = 0; j < r.towerCount && len + 16 < sizeof out; j++) {
        uint8_t* go = FindInLocations(l, n, r.towerIds[j]);
        int flags = -1;
        if (go) __try { flags = go[0x238]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        len += flags < 0 ? snprintf(out + len, sizeof out - len, " %08X=--", r.towerIds[j]) : snprintf(out + len, sizeof out - len, " %08X=%02X", r.towerIds[j], flags);
    }
    LogLine("LOOKDIAG %s: tower part flags (%d base locations):%s", g_camps[i].name, n, out);
}
static void LookWatchStart(int i, unsigned id, const uint8_t* sp, const EnemyVitals& v) {
    static uint8_t* seen[32]; static int seenN;                  // one watch at a time, each character once
    if (g_lw.go) return;
    for (int k = 0; k < seenN; k++) if (seen[k] == v.go) return;
    if (seenN < 32) seen[seenN++] = v.go;
    g_lw.go = v.go; g_lw.sp = sp; g_lw.id = id; g_lw.camp = i; g_lw.t = 0.f; g_lw.next = 0.f;
    LogLine("LOOKDIAG %s: lookout %08X entity object %p, vftable %llX (%s), %d objects", g_camps[i].name, id, v.go,
        (unsigned long long)v.vt, v.vt == VT_CHARACTER_GO ? "character" : "not a character", v.objects);
    TowerPartsDiag(i);
}
static void LookWatchTick(float dt) {
    if (!g_lw.go) return;
    g_lw.t += dt;
    if (g_lw.t < g_lw.next) return;
    g_lw.next = g_lw.t + 0.1f;
    EnemyVitals v;
    bool have = EnemyVitalsRead(g_lw.sp, &v);
    float* p = have && v.vt == VT_CHARACTER_GO ? CharacterPos(v.go) : nullptr;
    if (!have) LogLine("LOOKDIAG %s: lookout %08X after %.1f s: no entity", g_camps[g_lw.camp].name, g_lw.id, g_lw.t);
    else if (v.vt != VT_CHARACTER_GO) LogLine("LOOKDIAG %s: lookout %08X after %.1f s: object %p not a character (vftable %llX)", g_camps[g_lw.camp].name, g_lw.id, g_lw.t, v.go, (unsigned long long)v.vt);
    else LogLine("LOOKDIAG %s: lookout %08X after %.1f s: character %p at %.2f %.2f %.2f, health %.3f, vehicle %p", g_camps[g_lw.camp].name, g_lw.id, g_lw.t,
        v.go, p ? p[0] : 0.f, p ? p[1] : 0.f, p ? p[2] : 0.f, v.hp, v.vehicle);
    if (g_lw.t >= 6.f) g_lw.go = nullptr;
}
static const uintptr_t A_ForcePulseExplode = 0x14047F030;         // CForcePulse::Explode(hkLocalArray<CPhysicsGameObject*>*)
static const uintptr_t A_ForcePulseExplodeObject = 0x14047EA60;   // CForcePulse::ExplodeGameObject(CPhysicsGameObject*, float, hkLocalArray*, hkArray&)
static bool PulseMayLog() {
    float d = EvGobStoneDistance();
    return d >= 0.f && d < EV_NEAR_ONLY_R && InterlockedIncrement(&g_pulseLines) <= 150;
}
static unsigned PulseId(const uint8_t* pulse) {           // the CGameObject base is at +8 (StartExplode calls its vtable there), id at +0x14
    __try { return *(const unsigned*)(pulse + 8 + 0x14); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
DEFHOOK(void, ForcePulseExplode, (uint8_t* pulse, void* hits)) {
    if (PulseMayLog())
        LogLine("PULSEDIAG explode: pulse %08X (%p), player %.1f %.1f %.1f, %.0f m from Gob Stone; lookout watched %p", PulseId(pulse), pulse,
            g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z, EvGobStoneDistance(), g_lw.go);
    ForcePulseExplode_orig(pulse, hits);
}
DEFHOOK(void, ForcePulseExplodeObject, (uint8_t* pulse, uint8_t* target, float f, void* hits, void* bodies)) {
    if (PulseMayLog()) {
        unsigned tid = 0;
        __try { tid = *(const unsigned*)(target + 0x14); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        LogLine("PULSEDIAG   pulse %08X hits object %p (id at +0x14 %08X), %.3f; lookout watched %p", PulseId(pulse), target, tid, f, g_lw.go);
    }
    ForcePulseExplodeObject_orig(pulse, target, f, hits, bodies);
}
#endif

// the camp is held: the enemy location on and the civilians off; a new repopulation, no kills and no base guards yet
static void RepopHold(int i) {
    const CampEntry& c = g_camps[i]; RepopCamp& r = g_repop[i];
    SetLocBit(c.enemyId, true);
    SetLocBit(c.civilianId, false);
    BlockCivilians(c.civilianId, true);
    r.state = RP_PENDING; r.revived = false; r.revivedCount = 0; r.cycleStarted = false; r.deadCount = 0; r.armed = false;
    r.baseCount = 0; r.baseDone = false; r.baseIgnored = 0; memset(r.baseNoLive, 0, sizeof r.baseNoLive); memset(r.baseLost, 0, sizeof r.baseLost);
}
// the camp falls, out of the player's sight
static void RepopAttack(int i, float d) {
    const CampEntry& c = g_camps[i]; RepopCamp& r = g_repop[i];
    RepopHold(i);
    LogLine("CAMPREPOP %s: repopulating (player %.0f m away%s)", c.name, d, r.forced ? ", forced" : "");
    TowersForCycle(i);                                   // lookout towers (diagnostic)
    CampAttacked(i);
}

static float SpawnPointDistance(uint8_t* sp, const CampEntry& c, float campDistance) {
    float px, pz;
    if (!SpawnPointXZ(sp, c.x, c.z, &px, &pz)) return campDistance;
    return sqrtf((g_PlayerPos.x - px) * (g_PlayerPos.x - px) + (g_PlayerPos.z - pz) * (g_PlayerPos.z - pz));
}

static void RepopTick(float dt) {
#if MM_POINT_DIAG
    g_evMainThread = GetCurrentThreadId();
    LookWatchTick(dt);
#endif
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
            g_repop[i].state = RP_IDLE; g_repop[i].cooldown = 0.f;
            if (LocEnabled(g_camps[i].enemyId, &el) == 0) lib++;
        }
        g_repopAttackIn = RandAttack();
        LogLine("CAMPREPOP ready: %d of %d table camps liberated, one attack every %.0f-%.0f min (first in %.0f), at most %d held, a retaken camp is safe for %.0f-%.0f min", lib, N,
            g_repopAttackMin / 60.f, g_repopAttackMax / 60.f, g_repopAttackIn / 60.f, REPOP_MAX_TAKEN, g_repopCooldownMin / 60.f, g_repopCooldownMax / 60.f);
        for (int i = 0; i < (int)(sizeof g_loot / sizeof g_loot[0]); i++) { g_lootCd[i] = RandCooldown(); g_lootArmed[i] = false; }
        LogLine("LOOTREPOP ready: %d scavenging locations with enemies", (int)(sizeof g_loot / sizeof g_loot[0]));
        for (int i = 0; i < SN_N; i++) { g_sn[i].cd = RandCooldown(); g_sn[i].armed = false; }
        LogLine("SNIPEREPOP ready: %d sniper locations", SN_N);
        for (int i = 0; i < MF_N; i++) { g_mf[i] = MinefieldState(); g_mf[i].cd = RandCooldown(); }
        LogLine("MINEREPOP ready: %d minefields", MF_N);
#if MM_CAMP_DIAG
        CampDiagLocTable();
#endif
#if MM_SAVEFIX_GASWORKS
        GasworksSaveFix();
#endif
    }
    LootTick(step);
    SniperTick(step);
    MinefieldTick(step);
    ArenaTick(step);
#if MM_CAMP_DIAG
    CampDiagTick(step);
#endif
    bool anyActive = false;
    int taken = 0, attackable = 0, attackPick = -1, firstPick = -1, firstRank = -1;
    for (int i = 0; i < N; i++) {
        if (g_repop[i].state == RP_ACTIVE) anyActive = true;
        if (g_repop[i].state == RP_ACTIVE || g_repop[i].state == RP_PENDING) taken++;
    }
    RepopBudget(anyActive);
    int left = 0, leftCamp = -1; float leftD = 1e30f;            // enemies still to beat in the nearest active camp, for the counter
    for (int i = 0; i < N; i++) {
        const CampEntry& c = g_camps[i]; RepopCamp& r = g_repop[i];
        int el, cl; int en = LocEnabled(c.enemyId, &el), cv = LocEnabled(c.civilianId, &cl);
        float d = CampDistance(c);
        switch (r.state) {
        case RP_IDLE:
            if (en != 0) break;                                  // not liberated (or not in this game state)
            if (!r.forced) { r.cooldown -= step; if (r.cooldown > 0.f) break; }
            if (d < (r.forced ? REPOP_FORCED_FAR : REPOP_FAR)) break;   // wait until the player is far away (nothing changes on screen)
            if (r.forced) RepopAttack(i, d);
            else {
                if (rand() % ++attackable == 0) attackPick = i;         // an even pick among the camps that can fall, used after the loop
                for (int t = 0; t < g_repopFirstN; t++)
                    if (strstr(c.name, g_repopFirstCamp[t])) { if (firstPick < 0 || t < firstRank) { firstPick = i; firstRank = t; } break; }
            }
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
            uint8_t* sp[REPOP_TRACK]; unsigned ids[REPOP_TRACK];
            int n = LocSpawnPoints(c.enemyId, sp, ids, REPOP_TRACK);
            if (en == 0) {
#if MM_CAMP_DIAG
                int on = 0, entity = 0;                          // 2026-11-06: the game's own camp completion ran with points still on (sd1290: 12 of 21 dead)
                for (int k = 0; k < n; k++)
                    for (int j = 0; j < r.revivedCount; j++)
                        if (ids[k] == r.revivedIds[j] && sp[k][0x232]) { on++; if (SpawnStatus(sp[k] + 0x10) == 4) entity++; }
                LogLine("CAMPDIAG %s: switched off with %d of %d tracked points on, %d with an entity, %d dead, player %.0f m away", c.name, on, r.revivedCount, entity, r.deadCount, d);
#endif
                RepopRestore(i, "enemy location switched off by the game");
                break;
            }
            if (n < 0) {                                         // not loaded (yet, or unloaded because the player left)
                if (d > REPOP_LOAD + 150.f && r.revived) { r.revived = false; r.cycleStarted = false; r.baseDone = false; LogLine("CAMPREPOP %s: player left, enemies will respawn on the next approach", c.name); r.state = RP_PENDING; }
                break;
            }
            LookoutsToGround(i, sp, ids, n);                     // before the cycle below spawns them
            if (!r.cycleStarted) {
                // once per repopulation. Only character spawn points (spawn distance <= 100 m, the camp's spp.*) are tracked: the
                // others (370 m, the lookouts: 47 points, one sniper each) switch themselves off right after spawning, which is not a death: they count too, through the spawn system (lookoutMask). Every character spawn point of the camp is switched on and spawned, and all of them are tracked: some were left
                // on by earlier cycles or by the save and spawn enemies too (user test 2026-10-14: tracking only the revived ones
                // declared the camp cleared with enemies alive); the others are revived but not tracked
                int off = 0, ok = 0, kept = 0;
                r.revivedCount = 0; r.lookoutMask = 0; r.topDog = 0;
                for (int k = 0; k < n; k++) {
                    if (InPoints(g_nonCharPoints, ids[k])) continue;
                    if (InPoints(g_vanishPoints, ids[k])) { sp[k][0x232] = 0; continue; }
                    bool character = *(float*)(sp[k] + 0x168) <= 100.f;
                    bool wasOff = !sp[k][0x232];
                    if (wasOff) off++;
                    if (RepopIsDead(r, ids[k])) { sp[k][0x232] = 0; kept++; continue; }   // killed before the player left: stays dead
                    float lx, lz;
                    if (!character && SpawnPointXZ(sp[k], c.x, c.z, &lx, &lz) && (lx - c.x) * (lx - c.x) + (lz - c.z) * (lz - c.z) > REPOP_LOOKOUT_FAR * REPOP_LOOKOUT_FAR) {
                        LogLine("CAMPREPOP %s: lookout %08X %.0f m from the camp centre, left to the game", c.name, ids[k], sqrtf((lx - c.x) * (lx - c.x) + (lz - c.z) * (lz - c.z)));
                        continue;
                    }
                    if (!character && (!wasOff || r.armed)) {
                        int chars, dead;
                        SpawnEntityChars(sp[k], &chars, &dead);
                        if (!wasOff || chars) RepopTrackLookout(r, ids[k]);
                        continue;
                    }
                    if (IsTopDog(ids[k])) {                       // r47: warchiefs come back too, the mod opens their arena gate (g_warchiefGates)
                        LogLine("CAMPREPOP %s: Top Dog spawn point %08X left off", c.name, ids[k]);
                        continue;
                    }
                    if (character && r.revivedCount < REPOP_TRACK) {
                        if (TopDogPoint(ids[k])) {
                            r.topDog = ids[k];
                            LogLine("CAMPREPOP %s: Top Dog spawn point %08X back, one more enemy to beat; its completion events are dropped while the camp is held", c.name, ids[k]);
                        }
                        sp[k][0x232] = 1; ok++;                   // enabled now, spawned when the player gets near it (below)
                        r.revivedIds[r.revivedCount++] = ids[k];
                        if (InPoints(g_triggerPoints, ids[k])) LogLine("CAMPREPOP %s: spawn point %08X left to its own trigger", c.name, ids[k]);
                    } else if (SafeRevive(sp[k])) { ok++; if (!character) RepopTrackLookout(r, ids[k]); }   // the 370 m kind: as before
                }
                r.spawnedMask = r.lookoutMask; r.liveMask = 0; r.ignoredMask = 0; memset(r.noLive, 0, sizeof r.noLive); r.armed = true;
                memset(r.liveTime, 0, sizeof r.liveTime); memset(r.lost, 0, sizeof r.lost);
                // r40: the base guards of the earlier cycle are kept (user test 2026-11-15, r37: Hell's Grill sd2060 retaken on the return with
                // its base guard 2C5141C7 alive, the only one left: its 29 camp points were dead and the base count started again from 0)
                r.revived = true; r.cycleStarted = true; r.baseDone = false; r.baseWait = 0.f;
                g_repopTotal++;
                int guards = 0;
                for (int j = 0; j < r.baseCount; j++) if (!RepopIsDead(r, r.baseIds[j]) && !(r.baseIgnored >> j & 1)) guards++;
                LogLine("CAMPREPOP %s: %d spawn points, %d were off, %d enabled, %d character spawn points tracked, %d stay dead", c.name, n, off, ok, r.revivedCount, kept);
                if (guards) LogLine("CAMPREPOP %s: %d base guard(s) alive when the player left still count", c.name, guards);
                if (!r.revivedCount && !guards) { if (kept) CampRetaken(i); RepopRestore(i, kept ? "all enemies defeated" : "nothing to revive"); }
                break;
            }
            if (!r.revived) { r.revived = true; LogLine("CAMPREPOP %s: back in range, %d tracked spawn points", c.name, r.revivedCount); }
            {   // proximity spawn: the nearest waiting points, each once, while fewer than REPOP_MAX_LIVE are alive
                int now = 0, left = 0, live = 0;
                for (int k = 0; k < n; k++)
                    for (int j = 0; j < r.revivedCount; j++) {
                        if (ids[k] != r.revivedIds[j]) continue;
                        int chars, dead;
                        bool entity = SpawnEntityChars(sp[k], &chars, &dead);
                        float dd = SpawnPointDistance(sp[k], c, d);
                        if (uint8_t* god = chars ? SpawnEntityClearGod(sp[k]) : nullptr) {
                            static uint8_t* lastGod;
                            static int again;
                            again = god == lastGod ? again + 1 : 0;
                            lastGod = god;
                            if (again < 2) LogLine("CAMPREPOP %s: spawn point %08X: god mode %s on character %p", c.name, ids[k], again ? "came back, switched off again" : "switched off", god);
                        }
                        if (r.lookoutMask >> j & 1) {             // the game spawns these by itself: counted until the spawn system shows the sniper dead, or gone for good
#if MM_POINT_DIAG
                            if (chars && r.towerCount) { EnemyVitals v; if (EnemyVitalsRead(sp[k], &v) && v.go) LookWatchStart(i, ids[k], sp[k], v); }
#endif
                            if ((r.ignoredMask >> j & 1) || chars) { r.noLive[j] = 0.f; continue; }
                            if (dead && dd <= REPOP_LOST_RANGE) {
                                sp[k][0x232] = 0; r.ignoredMask |= 1ull << j; RepopRememberDead(r, ids[k]);
                                LogLine("CAMPREPOP %s: lookout %08X dead", c.name, ids[k]);
                                continue;
                            }
                            if (dd <= REPOP_POINT) r.noLive[j] += step;   // far from the point there is just nothing yet, or the sniper despawned alive
                            if (r.noLive[j] >= REPOP_NO_SPAWN) {          // no enemy is not a death (8F7310D0 of sd2060 left the count at 25 m and was back on the next approach): only a point that gives none is dropped
                                r.noLive[j] = 0.f;
                                if (r.lost[j] < REPOP_MAX_LOST) {
                                    r.lost[j]++;
                                    SafeRevive(sp[k]);
                                    LogLine("CAMPREPOP %s: lookout %08X has no enemy, asked again (%d of %d, %.0f m from the player)", c.name, ids[k], r.lost[j], REPOP_MAX_LOST, dd);
                                } else {
                                    r.ignoredMask |= 1ull << j;
                                    LogLine("CAMPREPOP %s: lookout %08X not counted any more (point on %d, gave no enemy, %.0f m from the player)", c.name, ids[k], sp[k][0x232], dd);
                                }
                            }
                            continue;
                        }
                        if (!sp[k][0x232]) { RepopRememberDead(r, ids[k]); continue; }
                        if (entity && !chars && dead && dd <= REPOP_LOST_RANGE) {
                            sp[k][0x232] = 0; RepopRememberDead(r, ids[k]);
                            LogLine("CAMPREPOP %s: spawn point %08X still on with its enemy dead, switched off", c.name, ids[k]);
                            continue;
                        }
                        if (SpawnStatus(sp[k] + 0x10) == 4) {     // also one the game spawned by itself (within 50 m): Spawn() again would double it
                            if (InPoints(g_triggerPoints, ids[k])) {
                                if (!(r.spawnedMask >> j & 1)) LogLine("CAMPREPOP %s: spawn point %08X spawned by the game (its trigger), %.0f m from the player", c.name, ids[k], dd);
#if MM_CAMP_DIAG
                                float every = r.liveTime[j] < 20.f ? 2.f : 10.f;      // did it leap: where it is and what its jump edge holds, every 2 s for 20 s, then every 10 s
                                if ((int)(r.liveTime[j] / every) != (int)((r.liveTime[j] + step) / every)) {
                                    EnemyVitals v;
                                    EnemyVitalsRead(sp[k], &v);
                                    float* p = v.go ? CharacterPos(v.go) : nullptr;
                                    LogLine("CAMPDIAG %s: trigger point %08X after %.0f s: character %p health %.3f at %.1f %.1f %.1f, player %.1f %.1f %.1f", c.name, ids[k], r.liveTime[j] + step,
                                            v.go, v.hp, p ? p[0] : 0.f, p ? p[1] : 0.f, p ? p[2] : 0.f, g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z);
                                    if (v.go && v.vt == VT_CHARACTER_GO) GimpEdgeProbe(c.name, ids[k], v.go, r.liveTime[j] + step);
                                }
#endif
                                EnemyVitals pv;
                                EnemyVitalsRead(sp[k], &pv);
                                if (pv.go && pv.vt == VT_CHARACTER_GO && pv.hp > 0.f) PerchLeaveStep(c.name, ids[k], pv.go);
                            }
#if MM_CAMP_DIAG
                            if (InPoints(g_aiProbePoints, ids[k]) && (r.liveTime[j] == 0.f || (int)(r.liveTime[j] / 4.f) != (int)((r.liveTime[j] + step) / 4.f))) {
                                EnemyVitals v;
                                EnemyVitalsRead(sp[k], &v);
                                if (v.go && v.vt == VT_CHARACTER_GO) AiProbe(c.name, ids[k], v.go, r.liveTime[j] + step);
                            }
#endif
                            r.spawnedMask |= 1ull << j; r.liveMask |= 1ull << j; r.ignoredMask &= ~(1ull << j); r.noLive[j] = 0.f; r.liveTime[j] += step; live++; continue;
                        }
                        if (!(r.spawnedMask >> j & 1) || (r.ignoredMask >> j & 1)) continue;
                        r.noLive[j] += step;
                        if (r.noLive[j] < REPOP_REQUEST_WAIT) live++;
                        if (r.noLive[j] < REPOP_NO_SPAWN) continue;
                        if (r.liveMask >> j & 1) {                // despawned alive: the game only brings it back within 50 m of the point
                            if (r.liveTime[j] < REPOP_NO_SPAWN && dd <= REPOP_LOST_RANGE && ++r.lost[j] >= REPOP_MAX_LOST) {
                                sp[k][0x232] = 0; r.ignoredMask |= 1ull << j;   // off, or the game spawns it again by itself; remembered as dead until the next repopulation
                                LogLine("CAMPREPOP %s: spawn point %08X lost its enemy %d times right after spawning, switched off", c.name, ids[k], r.lost[j]);
                                continue;
                            }
                            r.spawnedMask &= ~(1ull << j); r.liveMask &= ~(1ull << j); r.noLive[j] = 0.f; r.liveTime[j] = 0.f;
                            LogLine("CAMPREPOP %s: spawn point %08X lost its enemy, asked again", c.name, ids[k]);
                        } else {
                            r.ignoredMask |= 1ull << j;
                            LogLine("CAMPREPOP %s: spawn point %08X gave no enemy in %.0f s, ignored", c.name, ids[k], REPOP_NO_SPAWN);
                        }
                    }
                for (;;) {
                    int best = -1, bestJ = -1; float bestD = 1e30f; left = 0;
                    for (int k = 0; k < n; k++)
                        for (int j = 0; j < r.revivedCount; j++) {
                            if (ids[k] != r.revivedIds[j] || (r.spawnedMask >> j & 1) || (r.ignoredMask >> j & 1) || !sp[k][0x232] || InPoints(g_triggerPoints, ids[k])) continue;
                            left++;
                            float px, pz; bool have = SpawnPointXZ(sp[k], c.x, c.z, &px, &pz);
                            float dd = have ? sqrtf((g_PlayerPos.x - px) * (g_PlayerPos.x - px) + (g_PlayerPos.z - pz) * (g_PlayerPos.z - pz)) : d;
                            if (dd <= (have ? REPOP_POINT : REPOP_SPAWN) && dd < bestD) { bestD = dd; best = k; bestJ = j; }
                        }
                    if (best < 0 || live >= REPOP_MAX_LIVE) break;
                    r.spawnedMask |= 1ull << bestJ;                 // also when SafeSpawn fails: no retry every tick
                    r.noLive[bestJ] = 0.f;
                    if (SafeSpawn(sp[best])) { live++; now++; }
                }
                if (now) LogLine("CAMPREPOP %s: %d spawn point(s) spawned near the player, %d alive, %d still waiting, %d dead", c.name, now, live, left, r.deadCount);
            }
            // the base location's own characters (War Crier etc.): once its container is there (it streams on its own; given up after 60 s)
            uint8_t* bsp[48]; unsigned bids[48];
            int bn = LocSpawnPoints(g_campBase[i], bsp, bids, 48);
            if (!r.baseDone && d <= REPOP_SPAWN) {          // the base guards spawn only when the player is inside their despawn range
                r.baseWait += step;
                if (bn >= 0) {
                    r.baseDone = true;
                    int boff = 0, lootOff = 0;
                    bool itemsDone = CampItemsDone(i);
                    for (int k = 0; k < bn; k++) {
#if MM_CAMP_DIAG
                        if (bsp[k][0x232]) LogLine("CAMPDIAG %s: base point %08X on at the start of the cycle", c.name, bids[k]);
#endif
                        if (InPoints(g_baseLootPoints, bids[k])) {
                            if (bsp[k][0x232] && itemsDone) { bsp[k][0x232] = 0; lootOff++; }
                            continue;
                        }
                        bool warCrier = IsWarCrierSpawnPoint(i, bids[k]);
                        if (warCrier) continue;                          // user 2026-10-16: no structures (towers, drummers) come back
                        int known = -1;
                        for (int j = 0; j < r.baseCount; j++) if (r.baseIds[j] == bids[k]) known = j;
                        if (known >= 0) {                                // tracked before the player left: back on if it went off with the location
                            if (!bsp[k][0x232] && !RepopIsDead(r, bids[k]) && !(r.baseIgnored >> known & 1) && SafeRevive(bsp[k])) {
                                r.baseNoLive[known] = 0.f;
                                LogLine("CAMPREPOP %s: base spawn point %08X revived again (alive when the player left)", c.name, bids[k]);
                            }
                            continue;
                        }
                        if (!InPoints(g_baseCharPoints, bids[k]) || bsp[k][0x232]) continue;   // characters only; ones already on are left alone
                        if (IsTopDog(bids[k]) || RepopIsDead(r, bids[k])) continue;
                        boff++;
                        if (SafeRevive(bsp[k]) && r.baseCount < 16) { r.baseNoLive[r.baseCount] = 0.f; r.baseLost[r.baseCount] = 0; r.baseIds[r.baseCount++] = bids[k]; LogLine("CAMPREPOP %s: base spawn point %08X revived%s", c.name, bids[k], warCrier ? " (War Crier)" : ""); }
                    }
                    LogLine("CAMPREPOP %s: base location %d spawn points, %d character ones were off, %d revived and tracked, %d loot points left on switched off", c.name, bn, boff, r.baseCount, lootOff);
                } else if (r.baseWait > 60.f) { r.baseDone = true; LogLine("CAMPREPOP %s: base location not loaded, its characters skipped", c.name); }
            }
            int alive = 0;                                       // revived spawn points still on = their enemies not dead yet
            for (int k = 0; k < n; k++)
                for (int j = 0; j < r.revivedCount; j++) if (ids[k] == r.revivedIds[j] && (sp[k][0x232] || (r.lookoutMask >> j & 1)) && !(r.ignoredMask >> j & 1)) alive++;
            // r40: a base guard is counted while its point is on; one that lost its enemy alive is asked again when the player is within
            // REPOP_SPAWN of its point, as the camp points are (user test 2026-11-15, r37: Hell's Grill stayed taken with "1 left" and nobody
            // there: its guard 2C5141C7 is 86 m from the camp centre, the player stood 130 m from it, the game spawns it again only within 50 m)
            for (int j = 0; j < r.baseCount; j++) {
                if (RepopIsDead(r, r.baseIds[j]) || (r.baseIgnored >> j & 1)) continue;
                int k = 0;
                while (k < bn && bids[k] != r.baseIds[j]) k++;
                if (!r.baseDone || k >= bn) { alive++; continue; }   // not looked at yet in this cycle, or its location is not loaded: alive
                if (!bsp[k][0x232]) { RepopRememberDead(r, bids[k]); continue; }
                int chars, dead;
                bool entity = SpawnEntityChars(bsp[k], &chars, &dead);
                float dd = SpawnPointDistance(bsp[k], c, d);
                if (entity && !chars && dead && dd <= REPOP_LOST_RANGE) {
                    bsp[k][0x232] = 0; RepopRememberDead(r, bids[k]);
                    LogLine("CAMPREPOP %s: base spawn point %08X still on with its enemy dead, switched off", c.name, bids[k]);
                    continue;
                }
                alive++;
                if (chars || SpawnStatus(bsp[k] + 0x10) == 4) { r.baseNoLive[j] = 0.f; continue; }
                if (dd > REPOP_SPAWN || (r.baseNoLive[j] += step) < REPOP_NO_SPAWN) continue;
                r.baseNoLive[j] = 0.f;
                if (r.baseLost[j] < REPOP_MAX_LOST) {
                    r.baseLost[j]++;
                    SafeRevive(bsp[k]);
                    LogLine("CAMPREPOP %s: base spawn point %08X has no enemy, asked again (%d of %d, %.0f m from the player)", c.name, bids[k], r.baseLost[j], REPOP_MAX_LOST, dd);
                } else {
                    r.baseIgnored |= 1u << j; alive--;
                    LogLine("CAMPREPOP %s: base spawn point %08X not counted any more (gave no enemy, %.0f m from the player)", c.name, bids[k], dd);
                }
            }
            // a point left to its own trigger (sd2090's perch gimp 3B9F5107) may never fire: when only such points are left, the player near them
            // brings them in (autotest 2026-11-10/11: sd2090 stayed taken with 21 of 22 dead; the player on the point did not trip it either)
            int waitTrig = 0;
            for (int k = 0; k < n; k++)
                for (int j = 0; j < r.revivedCount; j++)
                    if (ids[k] == r.revivedIds[j] && sp[k][0x232] && !((r.spawnedMask | r.ignoredMask | r.lookoutMask) >> j & 1) && InPoints(g_triggerPoints, ids[k])) waitTrig++;
            if (alive && alive == waitTrig)
                for (int k = 0; k < n; k++)
                    for (int j = 0; j < r.revivedCount; j++) {
                        if (ids[k] != r.revivedIds[j] || !sp[k][0x232] || ((r.spawnedMask | r.ignoredMask | r.lookoutMask) >> j & 1) || !InPoints(g_triggerPoints, ids[k])) continue;
                        float dd = SpawnPointDistance(sp[k], c, d);
                        if (dd > REPOP_POINT) { r.noLive[j] = 0.f; continue; }
                        if ((r.noLive[j] += step) < REPOP_TRIGGER_WAIT) continue;
                        r.spawnedMask |= 1ull << j; r.noLive[j] = 0.f;   // from here on like any spawned point (no entity in REPOP_NO_SPAWN: ignored)
                        bool ok = SafeSpawn(sp[k]);
                        LogLine("CAMPREPOP %s: spawn point %08X: its trigger never fired and it is the last one, spawned by the mod (%s, %.0f m from the player)", c.name, ids[k], ok ? "ok" : "failed", dd);
                    }
            if (alive && d <= REPOP_LOAD && d < leftD) { leftD = d; left = alive; leftCamp = i; }
#if MM_CAMP_DIAG
            static float lastAcc;
            if (alive && alive <= 2 && d <= REPOP_LOAD) {   // the end of a camp is where the count goes wrong: what the game holds for the last ones
                bool sample = (lastAcc += step) >= 5.f;
                if (sample) lastAcc = 0.f;
                for (int k = 0; k < n; k++)
                    for (int j = 0; j < r.revivedCount; j++) {
                        if (ids[k] != r.revivedIds[j] || (r.ignoredMask >> j & 1) || !(sp[k][0x232] || (r.lookoutMask >> j & 1))) continue;
                        int chars, dead;
                        SpawnEntityChars(sp[k], &chars, &dead);
                        LastEnemyWatch(c.name, ids[k], sp[k], chars, dead, sample);
                        if (sample) LogLine("CAMPDIAG %s: left %08X%s on %d status %d, %d alive %d dead in the spawn system, %.0f m from the player", c.name, ids[k], (r.lookoutMask >> j & 1) ? " (lookout)" : "", sp[k][0x232], SpawnStatus(sp[k] + 0x10), chars, dead, SpawnPointDistance(sp[k], c, d));
                    }
            }
#endif
            if (!alive) { CampRetaken(i); RepopRestore(i, "all enemies defeated"); }
            break;
        }
        case RP_CLEARED:
            if (d > REPOP_CIV_BACK) RepopCiviliansBack(i);
            break;
        default: break;
        }
    }
    if (taken < REPOP_MAX_TAKEN) {
        g_repopAttackIn -= step;
        if (firstPick >= 0) attackPick = firstPick;
        if (g_repopAttackIn <= 0.f && attackPick >= 0) {
            RepopAttack(attackPick, CampDistance(g_camps[attackPick]));
            g_repopAttackIn = RandAttack();
            if (attackPick == firstPick) {
                for (int t = firstRank; t + 1 < g_repopFirstN; t++) memcpy(g_repopFirstCamp[t], g_repopFirstCamp[t + 1], sizeof g_repopFirstCamp[t]);
                g_repopFirstN--;
            }
        }
    }
    static int nameCamp = -1;
    if (leftCamp != nameCamp) { nameCamp = leftCamp; if (leftCamp >= 0) strncpy_s(g_campLeftName, CampTitle(leftCamp), _TRUNCATE); }
    g_campLeft = left; g_campLeftStamp = GetTickCount64();
    CampNoticeNext();
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
    int mfCleared = 0, mfArmed = 0;
    for (int i = 0; i < MF_N; i++) { mfCleared += g_mf[i].cleared; mfArmed += g_mf[i].armed; }
    LogLine("MINEREPOP %d minefields, %d seen cleared, %d armed, %d brought back this session", MF_N, mfCleared, mfArmed, g_mfRevivedTotal);
}

DEFHOOK(void, DestructibleLoad, (uint8_t* go, void* ctx)) {
    DestructibleLoad_orig(go, ctx);
    if (!g_repopInit) return;
    unsigned id = 0;
    __try { id = *(const unsigned*)(go + 0x14); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    for (int i = 0; i < SN_N; i++) {
        const SniperEntry& e = g_snipers[i];
        if (!g_sn[i].armed || !id || (id != e.platform && id != e.pylon)) continue;
        uint8_t* obj = go - 8;
        uint8_t before = obj[0x240];
        // flags (CDestructibleObject::Load/Save/EnableParts): 0x08 enabled (no part shown without it), 0x01 broken, 0x10 broken on
        // load, 0x20 saved as broken. A pulled-down tower's pylon -- its lower half -- is saved disabled, not broken (no "pylon loaded
        // intact" line in any log, Pablo saw only the platform, 2026-10-18 and 2026-11-11): enabled again here too
        if ((before & 0x08) && !(before & (0x01 | 0x10))) {
            LogLine("SNIPEREPOP %s: tower %s %08X loaded intact already (flags %02X)", e.name, id == e.platform ? "platform" : "pylon", id, before);
            return;
        }
        obj[0x240] = (uint8_t)((before & ~(0x01 | 0x10)) | 0x08 | ((before & (0x01 | 0x10)) ? 0x20 : 0));   // not broken, enabled; saved as broken if it was
        bool ok = SafeParts(obj);
        g_sn[i].towerRestored = true;
        LogLine("SNIPEREPOP %s: tower %s %08X loaded %s, rebuilt (flags %02X -> %02X)%s", e.name, id == e.platform ? "platform" : "pylon", id,
            (before & (0x01 | 0x10)) ? "broken" : "switched off", before, obj[0x240], ok ? "" : " FAULTED");
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

// r41: the game reloads the save when the player dies, and the save has every camp liberated (above): user test 2026-11-16 19:41,
// "eu morri, o jogo recarregou" -- Hell's Grill, Dead End and Wreck Hill were all freed, two of them never visited. The camps held
// before the load are held again, with all their enemies (the user's choice: the kills are lost, as the game's own progress is); only
// those the loaded save has liberated: one it has in enemy hands is the game's camp again. GameLocationSystemLoadStatus (called only by
// CGameWorld::LoadWorldProgress) sets the locations' flags from the save and starts the listed ones loading; it leaves the civilians'
// stream block (+0x66, DisableLoading) alone
static const uintptr_t A_LocLoadStatus = 0x140667E70;
DEFHOOK(void, LocLoadStatus, (const void* save)) {
    InterlockedExchange(&g_repopSaving, 1);              // RepopTick waits: until the camps are held again the flags are the save's
    LocLoadStatus_orig(save);
    InterlockedIncrement(&g_saveLoads);
    if (g_repopInit) {
        const int N = sizeof g_camps / sizeof g_camps[0];
        int again = 0, game = 0;
        for (int i = 0; i < N; i++) {
            RepopCamp& r = g_repop[i];
            if (r.state != RP_PENDING && r.state != RP_ACTIVE) continue;
            int el, en = LocEnabled(g_camps[i].enemyId, &el);
            if (en == 0) {
                RepopHold(i); again++;
                LogLine("CAMPREPOP %s: save loaded, held again with all its enemies", g_camps[i].name);
            } else if (en > 0) {
                BlockCivilians(g_camps[i].civilianId, false);
                r.state = RP_IDLE; r.cooldown = RandCooldown(); game++;
                LogLine("CAMPREPOP %s: save loaded with the camp in enemy hands, left to the game", g_camps[i].name);
            }
        }
        LogLine("CAMPREPOP save loaded: %d held camp(s) held again, %d left to the game", again, game);
        if (g_arenaWait.camp >= 0) {   // r48: the loaded save may be from before the Top Dog's death
            LogLine("CAMPREPOP %s: save loaded, arena flames fallback cancelled", g_camps[g_arenaWait.camp].name);
            g_arenaWait.camp = -1;
        }
    }
    InterlockedExchange(&g_repopSaving, 0);
}

#if MM_DEV_TOOLS || MM_AUTOTEST
// ---- autotest camp commands (plugin.cpp AutotestTick, 2026-11-10) ---------------------------------------------------------------------
// "repop <camp>": the camp falls like numpad 5 (forced: no cooldown, once the player is REPOP_FORCED_FAR away). "kill": every live character
// of the nearest camp's enemy spawn points dies the game's own way, CCharacterModule::InstantDeath's (god mode and E3 invulnerability
// cleared first). "log": every camp's state
typedef void (*DamageableSetHealthFn)(void* damageable, float health, const unsigned* damageType);
static const DamageableSetHealthFn DamageableSetHealth = (DamageableSetHealthFn)0x1401BE6C0;   // CDamageable::SetHealth(float, CHashString): the CCharacter, 8 bytes before its CGameObject
static const uintptr_t A_DamageUnknown = 0x141712A84;                                        // CDamageInflictor::DAMAGE_UNKNOWN
static int AutotestKillPoint(const uint8_t* sp) {
    uint8_t* chars[16]; int n = 0;
    __try {
        const uint8_t* h = *(const uint8_t* const*)A_SpawnHandlerPtr;
        if (!h) return 0;
        unsigned id = *(const unsigned*)(sp + 0x198);
        uint8_t* const* end = *(uint8_t* const* const*)(h + 0x58);
        for (uint8_t* const* e = *(uint8_t* const* const*)(h + 0x50); e != end; e++) {
            if (*(const unsigned*)(*e + 0x20) != id) continue;
            uint8_t* last = *(uint8_t* const*)(*e + 0x48);
            for (uint8_t* o = *(uint8_t* const*)(*e + 0x40); o != last && n < 16; o += 16) {
                uint8_t* go = *(uint8_t* const*)o;
                if (go && *(const uintptr_t*)go == VT_CHARACTER_GO && *(const float*)(go - 8 + 0x184) > 0.f) chars[n++] = go - 8;
            }
        }
        unsigned dmg = *(const unsigned*)A_DamageUnknown;
        for (int k = 0; k < n; k++) { chars[k][0xE9] = 0; chars[k][0xEA] = 0; DamageableSetHealth(chars[k], 0.f, &dmg); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { LogLine("AUTOTEST kill: fault"); }
    return n;
}
// "die" (r41): the player dies the game's way; the game then reloads the save, and AutotestTick waits for that load
static void AutotestDie(CCharacter* ch) {
    ch->m_Invulnerable = false; ((uint8_t*)ch)[0xE9] = 0; ((uint8_t*)ch)[0xEA] = 0;
    if (CVehicle* v = ch->GetVehiclePtr()) v->m_Invulnerable = false;
    g_autoDiedAt = g_saveLoads;
    __try { unsigned dmg = *(const unsigned*)A_DamageUnknown; DamageableSetHealth(ch, 0.f, &dmg); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_autoDiedAt = -1; LogLine("AUTOTEST die: fault"); return; }
    LogLine("AUTOTEST die: player killed at %.1f %.1f %.1f (%d loads so far)", g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z, (int)g_saveLoads);
}
static void AutotestCampCommand(const char* cmd) {
    if (!g_worldRepopOn || !g_repopInit) { LogLine("AUTOTEST \"%s\": camp repopulation is not running", cmd); return; }
    const int N = sizeof g_camps / sizeof g_camps[0];
    char verb[16] = "", arg[24] = "";
    sscanf_s(cmd, "%15s %23s", verb, (unsigned)sizeof verb, arg, (unsigned)sizeof arg);
    if (!strcmp(verb, "repop")) {
        for (int i = 0; i < N; i++) {
            if (!arg[0] || !strstr(g_camps[i].name, arg)) continue;
            int el, en = LocEnabled(g_camps[i].enemyId, &el);
            if (g_repop[i].state != RP_IDLE || en) { LogLine("AUTOTEST repop %s: not a liberated idle camp (%s, enemy location on %d)", g_camps[i].name, RP_NAMES[g_repop[i].state], en); return; }
            g_repop[i].forced = true;
            LogLine("AUTOTEST repop %s: forced, %.0f m away", g_camps[i].name, CampDistance(g_camps[i]));
            return;
        }
        LogLine("AUTOTEST repop: no camp \"%s\"", arg);
        return;
    }
    if (!strcmp(verb, "baseoff")) {   // the nearest camp's base guard points off, as the user's save had them (r37 test: sd2060's 2C5141C7 revived)
        int best = 0;
        for (int i = 1; i < N; i++) if (CampDistance(g_camps[i]) < CampDistance(g_camps[best])) best = i;
        uint8_t* sp[48]; unsigned ids[48];
        int bn = LocSpawnPoints(g_campBase[best], sp, ids, 48), off = 0;
        for (int k = 0; k < bn; k++)
            if (InPoints(g_baseCharPoints, ids[k]) && sp[k][0x232]) { sp[k][0x232] = 0; off++; LogLine("AUTOTEST baseoff %s: base point %08X off", g_camps[best].name, ids[k]); }
        LogLine("AUTOTEST baseoff %s (%.0f m): %d base guard points switched off, base location %d spawn points", g_camps[best].name, CampDistance(g_camps[best]), off, bn);
        return;
    }
    if (!strcmp(verb, "kill")) {   // "kill": every enemy of the nearest camp; "kill lookout": the lookouts only; "kill one": one enemy, not a lookout
        int best = 0;
        for (int i = 1; i < N; i++) if (CampDistance(g_camps[i]) < CampDistance(g_camps[best])) best = i;
        uint8_t* sp[48]; unsigned ids[48];
        bool lookouts = !strcmp(arg, "lookout"), one = !strcmp(arg, "one"), campOnly = !strcmp(arg, "camp");
        int n = LocSpawnPoints(g_camps[best].enemyId, sp, ids, 48), killed = 0, points = 0;
        for (int k = 0; k < n; k++) {
            if (lookouts || one) {
                bool lookout = *(float*)(sp[k] + 0x168) > 100.f;
                if (lookout != lookouts || (one && killed)) continue;
            }
            int c = AutotestKillPoint(sp[k]); killed += c; points += c > 0;
            if (c) LogLine("AUTOTEST kill %s: point %08X, %d", g_camps[best].name, ids[k], c);
        }
        if (lookouts || one || campOnly) {
            LogLine("AUTOTEST kill %s %s: %d enemies killed at %d of %d spawn points", arg, g_camps[best].name, killed, points, n);
            return;
        }
        // the revived base guards count too (autotest 2026-11-10: sd2060 stayed taken with its 29 points dead and base point 2C5141C7 alive)
        int baseKilled = 0, bn = LocSpawnPoints(g_campBase[best], sp, ids, 48);
        for (int k = 0; k < bn; k++)
            for (int j = 0; j < g_repop[best].baseCount; j++) if (ids[k] == g_repop[best].baseIds[j]) baseKilled += AutotestKillPoint(sp[k]);
        LogLine("AUTOTEST kill %s (%s, %.0f m): %d enemies killed at %d of %d spawn points, %d base guards of %d tracked", g_camps[best].name, RP_NAMES[g_repop[best].state],
                CampDistance(g_camps[best]), killed, points, n, baseKilled, g_repop[best].baseCount);
        return;
    }
    if (!strcmp(verb, "mines")) {   // the nearest minefield is marked cleared (MinefieldTick, once it has been loaded 3 s)
        int best = 0;
        for (int i = 1; i < MF_N; i++) if (MinefieldDistance(g_minefields[i]) < MinefieldDistance(g_minefields[best])) best = i;
        g_mfAutoClear = best;
        LogLine("AUTOTEST mines %s (%.0f m): to be marked cleared once loaded", g_minefields[best].name, MinefieldDistance(g_minefields[best]));
        return;
    }
    if (!strcmp(verb, "log")) { RepopLog(); return; }
    LogLine("AUTOTEST: unknown command \"%s\"", cmd);
}
#endif
