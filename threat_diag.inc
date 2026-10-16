// ---- threat diagnostic (dev, 2026-10-15) -----------------------------------------------------------------------------------------
// Which game code asks for the region / territory threat, how often and with what result -- to find what gates the world's
// random enemy spawns before making them threat-independent. Every function below only gets a counting pass-through hook:
//   NGSONodes::GetRegionThreat                     0x1402D52D0  graph-script node (key = the calling graph's path hash)
//   CGameModule::GetRegionThreat                   0x1404B92C0  xvm script function
//   CGameModule::GetTerritoryThreat                0x1404CBAC0  xvm script function
//   NAIConstantsProfilesHelpers::ComputeRegionThreatRatioForPosition 0x14002D480  float(const CVector3f&) (value = the ratio)
//   RegionInfoGetThreatLevelSummaryInRegion(uint)  0x1404ADFF0  (key = region)
//   RegionInfoGetThreatSummaryInRegion(uchar)      0x140491800  (key = internal region index, value = amount / capacity).
//     The uint overload 0x1404918D0 maps the region id to that index and tail-jumps here, so its callers show up too.
//     A region's threat is the sum over 5 threat types of economy resources: capacity (all) and amount (still standing).
// Counts per (function, caller, key) are written as THREATDIAG lines every 2 minutes and the first calls of each function
// immediately. Pass-throughs forward rcx/rdx/r8/r9 untouched (works whether a struct comes back in rax or through a hidden
// pointer); none of these functions takes float arguments in registers.
enum { TD_GSO = 0, TD_XVM_REGION, TD_XVM_TERRITORY, TD_AI_RATIO, TD_LEVEL_SUMMARY, TD_SUMMARY, TD_N };
static const char* TD_NAMES[TD_N] = { "GSO GetRegionThreat", "XVM GetRegionThreat", "XVM GetTerritoryThreat", "AI ThreatRatioForPosition",
                                      "RegionInfo ThreatLevelSummary", "RegionInfo ThreatSummary" };
struct TdStat { uintptr_t caller; unsigned key; unsigned n; double sum, mn, mx; };
static TdStat g_td[TD_N][128]; static int g_tdN[TD_N]; static unsigned g_tdTotal[TD_N];
static CRITICAL_SECTION g_tdLock; static volatile LONG g_tdLockInit;
static unsigned g_encRaised;                                     // encounter threat raises (below)

static void TdCount(int f, uintptr_t caller, unsigned key, double val) {
    if (InterlockedCompareExchange(&g_tdLockInit, 1, 0) == 0) InitializeCriticalSection(&g_tdLock);
    while (g_tdLockInit != 1) {}
    EnterCriticalSection(&g_tdLock);
    g_tdTotal[f]++;
    int k = 0;
    for (; k < g_tdN[f]; k++) if (g_td[f][k].caller == caller && g_td[f][k].key == key) break;
    if (k == g_tdN[f] && k < 128) { g_td[f][k] = { caller, key, 0, 0.0, 1e30, -1e30 }; g_tdN[f]++; }
    bool first = false;
    if (k < 128) {
        TdStat& s = g_td[f][k]; s.n++; s.sum += val; if (val < s.mn) s.mn = val; if (val > s.mx) s.mx = val; first = s.n == 1;
    }
    LeaveCriticalSection(&g_tdLock);
    if (first) LogLine("THREATDIAG first %s: caller exe+%llX key %08X value %.3f", TD_NAMES[f], (unsigned long long)(caller - 0x140000000), key, val);
}

static void TdDump(const char* why) {
    if (g_tdLockInit != 1) { LogLine("THREATDIAG %s: no threat query seen yet", why); return; }
    EnterCriticalSection(&g_tdLock);
    LogLine("THREATDIAG %s: player at %.0f %.0f %.0f", why, g_PlayerPos.x, g_PlayerPos.y, g_PlayerPos.z);
    for (int f = 0; f < TD_N; f++) {
        LogLine("THREATDIAG   %-30s %u calls%s", TD_NAMES[f], g_tdTotal[f], f == TD_GSO && g_encRaised ? " (encounter threat raised on some)" : "");
        for (int k = 0; k < g_tdN[f]; k++) {
            const TdStat& s = g_td[f][k];
            LogLine("THREATDIAG     caller exe+%-8llX key %08X calls %6u value avg %.3f min %.3f max %.3f", (unsigned long long)(s.caller - 0x140000000),
                s.key, s.n, s.n ? s.sum / s.n : 0.0, s.mn, s.mx);
        }
    }
    LeaveCriticalSection(&g_tdLock);
}

typedef uintptr_t (*TdFn4)(void*, void*, void*, void*);
static TdFn4 g_tdOrig[TD_N];

// ---- encounters keep the early-game rate (2026-10-15) ------------------------------------------------------------------------------
// graphs/open_world/encounter_system.gsrc is the only graph that reads the region threat for spawning (THREAT1 autotest): its region
// handler sets the road-encounter interval to 40 s * (2 - threat) and the faction handler the Scrotus share to 0.25 + 0.25 * threat.
// After its GetRegionThreat node ran, the node's ratio output (pin 55DB2ADC, a float graph variable) is raised to g_encMinThreat, for
// that graph only: the HUD, map, strongholds and threat_transfer (which ends the region on threat 0) keep reading the real value.
// scripts\encounter_min_threat.txt holds the minimum (0 = game behaviour, 1 = as on a new game; default 1).
static const unsigned GRAPH_ENCOUNTER_SYSTEM = 0x99BD2CEA;      // jenkins("graphs/open_world/encounter_system.gsrc")
static const uintptr_t A_GsGetFirstVariableData = 0x140F77060; // NGraphScript::CProcessor::GetFirstVariableData(const SGSNode*, unsigned) const
static const uintptr_t A_GsGetGraph = 0x140F761E0;             // NGraphScript::CProcessor::GetGraph() const
static const uintptr_t A_GsIsTypeFloat = 0x140F72270;          // NGraphScript::CGraph::IsTypeMatch<float>(const SGSData*)
static const uintptr_t A_GsGetDataWritable = 0x140F72140;      // NGraphScript::CGraph::GetInternalDataWritable(const SGSData*)
static const uintptr_t A_GsConvertSetFloat = 0x140F72A30;      // NGraphScript::CGraph::ConvertSet<float>(const SGSData*, const float&)
static float g_encMinThreat = 1.0f;
static void EncRaiseThreat(void* proc, void* node) {
    __try {
        const void* data = ((const void* (*)(void*, const void*, unsigned))A_GsGetFirstVariableData)(proc, node, 0x55DB2ADC);
        if (!data) return;
        void* graph = ((void* (*)(void*))A_GsGetGraph)(proc);
        if (!graph) return;
        if (!((bool (*)(const void*))A_GsIsTypeFloat)(data)) {   // not a float variable: the node converted; only a full minimum is safe
            if (g_encMinThreat < 0.999f) return;
            ((bool (*)(void*, const void*, const float*))A_GsConvertSetFloat)(graph, data, &g_encMinThreat);
            if (g_encRaised++ < 5) LogLine("ENCOUNTER threat for the encounter system set to %.3f (converted variable)", g_encMinThreat);
            return;
        }
        float* v = (float*)((uint8_t* (*)(void*, const void*))A_GsGetDataWritable)(graph, data);
        if (!v || *v >= g_encMinThreat) return;
        if (g_encRaised++ < 5) LogLine("ENCOUNTER threat for the encounter system raised %.3f -> %.3f (player at %.0f %.0f)", *v, g_encMinThreat, g_PlayerPos.x, g_PlayerPos.z);
        *v = g_encMinThreat;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static unsigned TdGraphHash(void* processor) {
    __try { void* obj = *(void**)((uintptr_t)processor + 0xA0); return obj ? *(unsigned*)((uintptr_t)obj + 0xF8) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFF; }
}
static uintptr_t TdGso(void* a, void* b, void* c, void* d) {
    unsigned graph = TdGraphHash(a);
    TdCount(TD_GSO, (uintptr_t)_ReturnAddress(), graph, 0.0);
    uintptr_t r = g_tdOrig[TD_GSO](a, b, c, d);
    if (g_encMinThreat > 0.f && graph == GRAPH_ENCOUNTER_SYSTEM && (unsigned)(uintptr_t)c == 0xC94BA74A) EncRaiseThreat(a, b);
    return r;
}
static uintptr_t TdXvmRegion(void* a, void* b, void* c, void* d) { TdCount(TD_XVM_REGION, (uintptr_t)_ReturnAddress(), 0, 0.0); return g_tdOrig[TD_XVM_REGION](a, b, c, d); }
static uintptr_t TdXvmTerritory(void* a, void* b, void* c, void* d) { TdCount(TD_XVM_TERRITORY, (uintptr_t)_ReturnAddress(), 0, 0.0); return g_tdOrig[TD_XVM_TERRITORY](a, b, c, d); }
static uintptr_t TdLevelSummary(void* a, void* b, void* c, void* d) { TdCount(TD_LEVEL_SUMMARY, (uintptr_t)_ReturnAddress(), (unsigned)(uintptr_t)a, 0.0); return g_tdOrig[TD_LEVEL_SUMMARY](a, b, c, d); }
static uintptr_t TdSummary(void* a, void* b, void* c, void* d) {
    uintptr_t r = g_tdOrig[TD_SUMMARY](a, b, c, d);
    const float* sum = (const float*)b;                          // { capacity, amount }
    TdCount(TD_SUMMARY, (uintptr_t)_ReturnAddress(), (unsigned)(uintptr_t)a & 0xFF, sum[0] > 0.f ? sum[1] / sum[0] : -1.0);
    return r;
}
typedef float (*TdRatioFn)(const float*);
static TdRatioFn g_tdRatioOrig;
static float TdRatio(const float* pos) {
    float r = g_tdRatioOrig(pos);
    TdCount(TD_AI_RATIO, (uintptr_t)_ReturnAddress(), 0, r);
    return r;
}

static void ThreatDiagInstall() {
    struct { uintptr_t addr; void* hook; void** orig; } hooks[] = {
        { 0x1402D52D0, (void*)TdGso, (void**)&g_tdOrig[TD_GSO] },
        { 0x1404B92C0, (void*)TdXvmRegion, (void**)&g_tdOrig[TD_XVM_REGION] },
        { 0x1404CBAC0, (void*)TdXvmTerritory, (void**)&g_tdOrig[TD_XVM_TERRITORY] },
        { 0x14002D480, (void*)TdRatio, (void**)&g_tdRatioOrig },
        { 0x1404ADFF0, (void*)TdLevelSummary, (void**)&g_tdOrig[TD_LEVEL_SUMMARY] },
        { 0x140491800, (void*)TdSummary, (void**)&g_tdOrig[TD_SUMMARY] },
    };
    int ok = 0;
    for (auto& h : hooks) {
        MH_STATUS c = MH_CreateHook((LPVOID)h.addr, h.hook, (LPVOID*)h.orig);
        if (c == MH_OK) c = MH_EnableHook((LPVOID)h.addr);
        if (c == MH_OK) ok++; else LogLine("THREATDIAG hook %llX: %s", (unsigned long long)h.addr, MH_StatusToString(c));
    }
    LogLine("THREATDIAG %d of %d threat hooks installed", ok, (int)(sizeof hooks / sizeof hooks[0]));
    FILE* f = nullptr; char path[MAX_PATH]; GetModuleFileNameA(NULL, path, MAX_PATH);
    char* sl = strrchr(path, '\\'); if (sl) sl[1] = 0;
    strcat_s(path, "scripts\\encounter_min_threat.txt");
    if (fopen_s(&f, path, "r") == 0 && f) {
        float m = -1.f;
        if (fscanf_s(f, "%f", &m) == 1 && m >= 0.f && m <= 1.f) g_encMinThreat = m;
        fclose(f);
    }
    LogLine("ENCOUNTER minimum region threat for random encounters: %.2f%s", g_encMinThreat, g_encMinThreat > 0.f ? "" : " (game behaviour)");
}

static void ThreatDiagTick(float step) {
    static float acc; acc += step;
    if (acc >= 600.f) { acc = 0.f; TdDump("periodic"); }
}
