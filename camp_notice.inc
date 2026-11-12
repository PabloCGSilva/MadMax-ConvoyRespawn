// On-screen camp notices (user 2026-11-06): the game has no enemy counter and its completion screen does not run for a repopulated camp, so the player sees how many enemies are left
// and a line when the last one falls. Drawn through the overlay's ImGui with the game's own UI font (gamefont.inc, read from the player's archives, same code as Wasteland Storms).
#include <d3d11.h>
#include <vector>
#include "gamefont.inc"

// Jenkins lookup3 hashlittle, seed 0: gamefont.inc finds the font's container with it
uint32_t WsJenkinsHash(const char* str) {
    const uint8_t* k = (const uint8_t*)str; uint32_t length = (uint32_t)strlen(str);
    uint32_t a, b, c; a = b = c = 0xDEADBEEF + length;
    #define ROT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
    while (length > 12) {
        a += k[0] | (k[1] << 8) | (k[2] << 16) | ((uint32_t)k[3] << 24);
        b += k[4] | (k[5] << 8) | (k[6] << 16) | ((uint32_t)k[7] << 24);
        c += k[8] | (k[9] << 8) | (k[10] << 16) | ((uint32_t)k[11] << 24);
        a -= c; a ^= ROT(c, 4); c += b;  b -= a; b ^= ROT(a, 6);  a += c;
        c -= b; c ^= ROT(b, 8); b += a;  a -= c; a ^= ROT(c, 16); c += b;
        b -= a; b ^= ROT(a, 19); a += c; c -= b; c ^= ROT(b, 4);  b += a;
        length -= 12; k += 12;
    }
    switch (length) {
    case 12: c += (uint32_t)k[11] << 24; [[fallthrough]];
    case 11: c += k[10] << 16; [[fallthrough]];
    case 10: c += k[9] << 8; [[fallthrough]];
    case 9:  c += k[8]; [[fallthrough]];
    case 8:  b += (uint32_t)k[7] << 24; [[fallthrough]];
    case 7:  b += k[6] << 16; [[fallthrough]];
    case 6:  b += k[5] << 8; [[fallthrough]];
    case 5:  b += k[4]; [[fallthrough]];
    case 4:  a += (uint32_t)k[3] << 24; [[fallthrough]];
    case 3:  a += k[2] << 16; [[fallthrough]];
    case 2:  a += k[1] << 8; [[fallthrough]];
    case 1:  a += k[0]; break;
    case 0:  return c;
    }
    c ^= b; c -= ROT(b, 14); a ^= c; a -= ROT(c, 11); b ^= a; b -= ROT(a, 25);
    c ^= b; c -= ROT(b, 16); a ^= c; a -= ROT(c, 4);  b ^= a; b -= ROT(a, 14);
    c ^= b; c -= ROT(b, 24);
    #undef ROT
    return c;
}

static const uintptr_t A_Locator = 0x1417127E0;              // CLocator*
static const uintptr_t A_LocatorGetString = 0x1402117A0;     // CLocator::GetLocalizedString(id): "" for an unknown id
static GameFont g_noticeFont;
static ID3D11Device* g_noticeDev = nullptr;                  // the device g_noticeSrv belongs to: a new one means the renderer was rebuilt
static ID3D11ShaderResourceView* g_noticeSrv = nullptr;

// written by the game thread, read by the render thread: a torn title only shows for one frame
static char g_noticeTitle[64], g_noticeDetail[96];
static volatile ULONGLONG g_noticeStart = 0, g_noticeUntil = 0;
static volatile int g_noticeSerial = 0;                      // one per notice shown: the render thread logs each one it draws
static volatile int g_campLeft = 0;
static volatile ULONGLONG g_campLeftStamp = 0;               // the tick stops in menus and pauses, and the counter goes with it
static char g_campLeftName[64];                              // the camp the counter is about

static void CampNoticeInit() {
    std::vector<uint8_t> file;
    if (LoadGameFontFile(file) && !ParseGameFont(file, g_noticeFont)) LogLine("font: game font file not understood, using the built-in font");
}

static void CampNoticeShow(const char* title, const char* detail, int milliseconds) {
    strncpy_s(g_noticeTitle, title, _TRUNCATE);           // a camp name in another language can be long: strcpy_s would end the process
    strncpy_s(g_noticeDetail, detail, _TRUNCATE);
    ULONGLONG now = GetTickCount64();
    g_noticeStart = now; g_noticeUntil = now + milliseconds;
    g_noticeSerial++;
}

// a notice that must not cut another one short waits here, game thread only. Camps can fall close together (the test cycle turns most of them in a minute): what does not fit is dropped
static const int NOTICE_MS = 10000;
static struct { char title[64], detail[96]; } g_noticeQueue[3];
static int g_noticeQueued = 0;
static bool CampNoticeQueue(const char* title, const char* detail) {
    if (g_noticeQueued >= (int)(sizeof g_noticeQueue / sizeof g_noticeQueue[0])) return false;
    strncpy_s(g_noticeQueue[g_noticeQueued].title, title, _TRUNCATE);
    strncpy_s(g_noticeQueue[g_noticeQueued].detail, detail, _TRUNCATE);
    g_noticeQueued++;
    return true;
}
static void CampNoticeNext() {
    if (!g_noticeQueued || GetTickCount64() < g_noticeUntil) return;
    CampNoticeShow(g_noticeQueue[0].title, g_noticeQueue[0].detail, NOTICE_MS);
    g_noticeQueued--;
    memmove(&g_noticeQueue[0], &g_noticeQueue[1], g_noticeQueued * sizeof g_noticeQueue[0]);
}

// the atlas is a distance field: cut at the glyph edge with a narrow smoothstep, as in Wasteland Storms
static void NoticeUploadFont(ID3D11Device* dev) {
    g_noticeDev = dev; g_noticeSrv = nullptr;
    if (!g_noticeFont.ok) return;
    std::vector<uint32_t> rgba((size_t)g_noticeFont.atlasW * g_noticeFont.atlasH);
    uint8_t lut[256];
    for (int v = 0; v < 256; v++) {
        float x = ((float)v / 255.0f - 0.44f) / 0.12f;
        x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
        lut[v] = (uint8_t)(255.0f * x * x * (3.0f - 2.0f * x));
    }
    for (size_t i = 0; i < rgba.size(); i++) rgba[i] = 0x00FFFFFFu | ((uint32_t)lut[g_noticeFont.alpha[i]] << 24);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = g_noticeFont.atlasW; td.Height = g_noticeFont.atlasH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = { rgba.data(), (UINT)(g_noticeFont.atlasW * 4), 0 };
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, &sd, &tex)) || !tex) { LogLine("font: texture upload failed"); return; }
    D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
    vd.Format = td.Format; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(tex, &vd, &g_noticeSrv))) g_noticeSrv = nullptr;
    tex->Release();
    LogLine("font: game font %s", g_noticeSrv ? "ready" : "view creation failed");
}

static const GameGlyph* NoticeGlyph(unsigned char c) {
    int i = (int)c - g_noticeFont.firstChar;
    if (i < 0 || i >= (int)g_noticeFont.map.size()) i = '?' - g_noticeFont.firstChar;
    int gi = g_noticeFont.map[i];
    return gi < (int)g_noticeFont.glyphs.size() ? &g_noticeFont.glyphs[gi] : nullptr;
}

// one centred line, white, no background (user preference, 2026-09-23), with a dark edge: plain white was missed over the bright sky (user test 2026-11-07, three attacks drawn, none seen); capitals like the game's HUD headers
static void NoticeLine(ImDrawList* dl, float cx, float y, float scale, float alpha, const char* text) {
    char t[96]; size_t n = strlen(text); if (n > sizeof t - 1) n = sizeof t - 1;
    for (size_t i = 0; i < n; i++) t[i] = (char)toupper((unsigned char)text[i]);
    t[n] = 0;
    static const float edge[5][2] = { { -1.f, 0.f }, { 1.f, 0.f }, { 0.f, -1.f }, { 0.f, 1.f }, { 0.f, 0.f } };   // the text itself last
    float e = 57.f * scale * 0.05f;
    if (!g_noticeSrv) {
        float size = g_noticeFont.ok ? g_noticeFont.lineHeight * scale : 57.f * scale;
        ImVec2 w = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.f, t);
        for (int i = 0; i < 5; i++)
            dl->AddText(ImGui::GetFont(), size, ImVec2(cx - w.x * 0.5f + edge[i][0] * e, y + edge[i][1] * e), i < 4 ? IM_COL32(0, 0, 0, (int)(230 * alpha)) : IM_COL32(255, 255, 255, (int)(255 * alpha)), t);
        return;
    }
    float w = 0.f;
    for (const char* p = t; *p; p++) { const GameGlyph* g = NoticeGlyph((unsigned char)*p); if (g) w += (g->w + g_noticeFont.advanceAdjust) * scale; }
    for (int i = 0; i < 5; i++) {
        ImU32 col = i < 4 ? IM_COL32(0, 0, 0, (int)(230 * alpha)) : IM_COL32(255, 255, 255, (int)(255 * alpha));
        float x = cx - w * 0.5f + edge[i][0] * e, ty = y + edge[i][1] * e;
        for (const char* p = t; *p; p++) {
            const GameGlyph* g = NoticeGlyph((unsigned char)*p);
            if (!g) continue;
            dl->AddImage((ImTextureID)g_noticeSrv, ImVec2(x, ty + g->yoff * scale), ImVec2(x + g->w * scale, ty + (g->yoff + g->h) * scale), ImVec2(g->u0, g->v0), ImVec2(g->u1, g->v1), col);
            x += (g->w + g_noticeFont.advanceAdjust) * scale;
        }
    }
}

class CampNoticeOverlay : public ImGuiRenderer {
    void Render() override {
        static ULONGLONG lastFrame;
        static int drawnSerial, frames;
        ULONGLONG now = GetTickCount64();
        bool playing = now - g_campLeftStamp < 2500;         // the tick stops in menus, pauses and loading screens
        if (!playing && lastFrame && now < g_noticeUntil) { g_noticeStart += now - lastFrame; g_noticeUntil += now - lastFrame; }   // a notice waits for the player instead of running out unseen
        lastFrame = now;
        bool notice = now < g_noticeUntil;
        if (!notice && frames) { LogLine("notice finished: %d frames drawn", frames); frames = 0; }
        if (!playing) return;
        int left = g_campLeft;
        if (!notice && !left) return;
        ImGuiIO& io = ImGui::GetIO();
        ID3D11ShaderResourceView* atlas = (ID3D11ShaderResourceView*)io.Fonts->TexID;
        if (atlas) {
            ID3D11Device* dev = nullptr;
            atlas->GetDevice(&dev);
            if (dev) { if (dev != g_noticeDev) NoticeUploadFont(dev); dev->Release(); }
        }
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        float unit = io.DisplaySize.y / 1080.0f, cx = io.DisplaySize.x * 0.5f, y = io.DisplaySize.y * 0.085f;
        if (notice) {                                        // fade in over 0.25 s, out over the last 0.6 s
            float alpha = 1.0f, since = (float)(now - g_noticeStart) / 1000.0f, toEnd = (float)(g_noticeUntil - now) / 1000.0f;
            if (since < 0.25f) alpha = since / 0.25f;
            if (toEnd < 0.6f) alpha = toEnd / 0.6f;
            if (alpha < 0.0f) alpha = 0.0f;
            if (drawnSerial != g_noticeSerial) { drawnSerial = g_noticeSerial; LogLine("notice on screen: \"%s\" (%.0f x %.0f, game font %d)", g_noticeTitle, io.DisplaySize.x, io.DisplaySize.y, g_noticeSrv != nullptr); }
            frames++;
            NoticeLine(dl, cx, y, 0.62f * unit, alpha, g_noticeTitle);
            if (g_noticeDetail[0]) NoticeLine(dl, cx, y + 57.f * 0.62f * unit, 0.48f * unit, alpha, g_noticeDetail);
        } else {
            char line[96];
            if (g_campLeftName[0]) snprintf(line, sizeof line, "%s - Enemies left: %d", g_campLeftName, left);
            else snprintf(line, sizeof line, "Enemies left: %d", left);
            NoticeLine(dl, cx, y, 0.48f * unit, 1.0f, line);
        }
    }
};
static CampNoticeOverlay g_campNoticeOverlay;
