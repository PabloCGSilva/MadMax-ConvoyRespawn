// Loads the game's own UI font (gui/fonts/mad_max.font) straight from the
// player's installed archives, so the mod never has to ship a game file.
//
// Where it lives: inside small GUI containers such as auto_save_indication.bl
// (a SARC), which sit in archives_win64\game*.arc as single raw-deflate
// streams (zlib without header, window 15). The .tab next to each .arc
// indexes entries by Jenkins(lowercase file name).
//
// Font format (worked out 2026-09-23 and checked by rendering a test phrase):
//   u32 0x00080080-ish prefix, then a DDS: 1024x512, 8-bit single channel
//   holding a DISTANCE FIELD (a stroke reads 13..201..13 across; the glyph
//   edge is the 0.5 level), not plain coverage
//   then: f32 ?, f32 advanceAdjust (-4.0), u16 lineHeight (57), u16 firstChar (32),
//         u16 glyphCount (224), u16 0x2103
//   glyphCount records of 24 bytes: f32 u0,v0,u1,v1 (atlas UVs), u8 width,
//         u8 height, u8 yOffset (from the line top), u8 0, u16 kerningStart,
//         u16 kerningCount
//   u16 glyphIndex[glyphCount], indexed by (char - firstChar)
//   (kerning pairs follow; not used)

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>

void LogLine(const char* fmt, ...);
uint32_t WsJenkinsHash(const char* str);

// ------------------------------------------------------------- inflate --
// Raw DEFLATE (RFC 1951) decoder, written for this mod. Small and slow is
// fine: it runs once, on about 1.6 MB.
namespace {
struct BitReader {
    const uint8_t* src; size_t len, pos; uint32_t bitbuf; int bitcnt; bool error;
    int bits(int need) {
        uint32_t val = bitbuf;
        while (bitcnt < need) {
            if (pos >= len) { error = true; return 0; }
            val |= (uint32_t)src[pos++] << bitcnt;
            bitcnt += 8;
        }
        bitbuf = val >> need;
        bitcnt -= need;
        return (int)(val & ((1u << need) - 1));
    }
};
struct Huffman { short count[16]; short symbol[288]; };

int Decode(BitReader& s, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= s.bits(1);
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
        if (s.error) return -1;
    }
    return -1;
}

void Build(Huffman& h, const short* length, int n) {
    memset(h.count, 0, sizeof(h.count));
    for (int i = 0; i < n; i++) h.count[length[i]]++;
    short offs[16]; offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h.count[len];
    for (int i = 0; i < n; i++) if (length[i]) h.symbol[offs[length[i]]++] = (short)i;
    h.count[0] = 0;
}

const short kLenBase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
const short kLenExtra[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
const short kDistBase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
const short kDistExtra[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

bool Codes(BitReader& s, std::vector<uint8_t>& out, const Huffman& lencode, const Huffman& distcode) {
    for (;;) {
        int sym = Decode(s, lencode);
        if (sym < 0 || s.error) return false;
        if (sym < 256) { out.push_back((uint8_t)sym); continue; }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) return false;
        int len = kLenBase[sym] + s.bits(kLenExtra[sym]);
        int dsym = Decode(s, distcode);
        if (dsym < 0 || dsym >= 30) return false;
        size_t dist = kDistBase[dsym] + s.bits(kDistExtra[dsym]);
        if (s.error || dist > out.size()) return false;
        size_t from = out.size() - dist;
        for (int i = 0; i < len; i++) out.push_back(out[from + i]);
    }
}

bool Inflate(const uint8_t* src, size_t len, std::vector<uint8_t>& out, size_t expected) {
    BitReader s = { src, len, 0, 0, 0, false };
    out.clear(); out.reserve(expected);
    int last;
    do {
        last = s.bits(1);
        int type = s.bits(2);
        if (s.error) return false;
        if (type == 0) {
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.pos + 4 > s.len) return false;
            unsigned n = s.src[s.pos] | (s.src[s.pos + 1] << 8);
            s.pos += 4;
            if (s.pos + n > s.len) return false;
            out.insert(out.end(), s.src + s.pos, s.src + s.pos + n);
            s.pos += n;
        } else if (type == 1) {
            static Huffman fl, fd; static bool built = false;
            if (!built) {
                short l[288];
                int i = 0;
                for (; i < 144; i++) l[i] = 8;
                for (; i < 256; i++) l[i] = 9;
                for (; i < 280; i++) l[i] = 7;
                for (; i < 288; i++) l[i] = 8;
                Build(fl, l, 288);
                for (i = 0; i < 30; i++) l[i] = 5;
                Build(fd, l, 30);
                built = true;
            }
            if (!Codes(s, out, fl, fd)) return false;
        } else if (type == 2) {
            static const short order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            int nlen = s.bits(5) + 257, ndist = s.bits(5) + 1, ncode = s.bits(4) + 4;
            if (nlen > 286 || ndist > 30) return false;
            short lengths[320] = { 0 };
            for (int i = 0; i < ncode; i++) lengths[order[i]] = (short)s.bits(3);
            Huffman lencode, distcode;
            Build(lencode, lengths, 19);
            int index = 0;
            while (index < nlen + ndist) {
                int sym = Decode(s, lencode);
                if (sym < 0 || s.error) return false;
                if (sym < 16) { lengths[index++] = (short)sym; continue; }
                short len2 = 0; int rep;
                if (sym == 16) { if (index == 0) return false; len2 = lengths[index - 1]; rep = 3 + s.bits(2); }
                else if (sym == 17) rep = 3 + s.bits(3);
                else rep = 11 + s.bits(7);
                if (index + rep > nlen + ndist) return false;
                while (rep--) lengths[index++] = len2;
            }
            Build(lencode, lengths, nlen);
            Build(distcode, lengths + nlen, ndist);
            if (!Codes(s, out, lencode, distcode)) return false;
        } else {
            return false;
        }
    } while (!last);
    return true;
}
} // namespace

// ------------------------------------------------------------ archives --
static bool ReadFileBytes(const char* path, uint64_t offset, size_t size, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)offset;
    bool ok = SetFilePointerEx(h, li, NULL, FILE_BEGIN) != 0;
    if (ok && size == 0) { LARGE_INTEGER sz; ok = GetFileSizeEx(h, &sz) != 0; size = (size_t)sz.QuadPart; }
    if (ok) {
        out.resize(size);
        DWORD got = 0;
        ok = ReadFile(h, out.data(), (DWORD)size, &got, NULL) && got == size;
    }
    CloseHandle(h);
    return ok;
}

struct TabEntry { uint32_t offset, compSize, uncompSize; bool chunked; };

static bool FindInTab(const std::vector<uint8_t>& tab, uint32_t hash, TabEntry& e) {
    if (tab.size() < 8 || *(const uint32_t*)tab.data() != 0x800) return false;
    size_t off = 4;
    uint32_t chunkLists = *(const uint32_t*)(tab.data() + off); off += 4;
    bool chunked = false;
    for (uint32_t i = 0; i < chunkLists && off + 8 <= tab.size(); i++) {
        uint32_t h = *(const uint32_t*)(tab.data() + off), n = *(const uint32_t*)(tab.data() + off + 4);
        if (h == hash) chunked = true;
        off += 8 + (size_t)n * 8;
    }
    for (; off + 16 <= tab.size(); off += 16) {
        const uint32_t* r = (const uint32_t*)(tab.data() + off);
        if (r[0] == hash) { e = { r[1], r[2], r[3], chunked }; return true; }
    }
    return false;
}

static bool ExtractFromSarc(const std::vector<uint8_t>& sarc, const char* name, std::vector<uint8_t>& out) {
    if (sarc.size() < 16 || memcmp(sarc.data() + 4, "SARC", 4) != 0) return false;
    size_t end = 16 + *(const uint32_t*)(sarc.data() + 12), off = 16;
    while (off + 4 <= end && off + 4 <= sarc.size()) {
        uint32_t n = *(const uint32_t*)(sarc.data() + off); off += 4;
        if (n == 0 || off + n + 8 > sarc.size()) break;
        std::string entry((const char*)sarc.data() + off, strnlen((const char*)sarc.data() + off, n)); off += n;
        uint32_t o = *(const uint32_t*)(sarc.data() + off), sz = *(const uint32_t*)(sarc.data() + off + 4); off += 8;
        if (entry == name && (size_t)o + sz <= sarc.size()) { out.assign(sarc.begin() + o, sarc.begin() + o + sz); return true; }
    }
    return false;
}

// Any of these GUI containers carries the English UI font.
static const char* kFontContainers[] = { "auto_save_indication.bl", "bookmarks_menu.bl", "ask_for_user_age.bl", "capture_mode.bl", "camp_completion.bl" };

bool LoadGameFontFile(std::vector<uint8_t>& font) {
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\'); if (slash) slash[1] = 0;
    for (const char* container : kFontContainers) {
        uint32_t hash = WsJenkinsHash(container);
        for (int i = 0; i < 64; i++) {
            char tabPath[MAX_PATH], arcPath[MAX_PATH];
            snprintf(tabPath, MAX_PATH, "%sarchives_win64\\game%d.tab", exe, i);
            snprintf(arcPath, MAX_PATH, "%sarchives_win64\\game%d.arc", exe, i);
            std::vector<uint8_t> tab;
            if (!ReadFileBytes(tabPath, 0, 0, tab)) break;
            TabEntry e;
            if (!FindInTab(tab, hash, e)) continue;
            if (e.chunked) { LogLine("font: %s is chunked in game%d, skipped", container, i); continue; }
            std::vector<uint8_t> packed, sarc;
            if (!ReadFileBytes(arcPath, e.offset, e.compSize, packed)) continue;
            if (e.compSize == e.uncompSize) sarc.swap(packed);
            else if (!Inflate(packed.data(), packed.size(), sarc, e.uncompSize) || sarc.size() != e.uncompSize) {
                LogLine("font: could not unpack %s from game%d", container, i);
                continue;
            }
            if (ExtractFromSarc(sarc, "gui/fonts/mad_max.font", font)) {
                LogLine("font: game font read from %s in game%d.arc (%u bytes)", container, i, (unsigned)font.size());
                return true;
            }
        }
    }
    LogLine("font: game font not found in the archives, using the built-in font");
    return false;
}

// ---------------------------------------------------------------- parse --
struct GameGlyph { float u0, v0, u1, v1; int w, h, yoff; };
struct GameFont {
    bool ok = false;
    int atlasW = 0, atlasH = 0, lineHeight = 0, firstChar = 0;
    float advanceAdjust = -4.0f;           // header float #2: added to every glyph width
    std::vector<uint8_t> alpha;            // atlasW * atlasH, 8-bit coverage
    std::vector<GameGlyph> glyphs;
    std::vector<uint16_t> map;             // (char - firstChar) -> glyph
};

bool ParseGameFont(const std::vector<uint8_t>& f, GameFont& out) {
    if (f.size() < 4 + 128 || memcmp(f.data() + 4, "DDS ", 4) != 0) return false;
    const uint8_t* dds = f.data() + 4;
    uint32_t h = *(const uint32_t*)(dds + 12), w = *(const uint32_t*)(dds + 16);
    uint32_t bits = *(const uint32_t*)(dds + 88);
    if (bits != 8 || w == 0 || h == 0 || w > 4096 || h > 4096) return false;
    size_t pix = 4 + 128, tbl = pix + (size_t)w * h;
    if (f.size() < tbl + 16) return false;
    const uint8_t* t = f.data() + tbl;
    int lineHeight = *(const uint16_t*)(t + 8), first = *(const uint16_t*)(t + 10), count = *(const uint16_t*)(t + 12);
    if (count <= 0 || count > 4096 || f.size() < tbl + 16 + (size_t)count * 24 + (size_t)count * 2) return false;
    out.atlasW = (int)w; out.atlasH = (int)h; out.lineHeight = lineHeight; out.firstChar = first;
    // The glyph boxes include the distance-field margin; the header's second
    // float (-4.0 in the English font) is what the game adds to each width to
    // get the real advance -- checked by rendering: it gives the game's own
    // spacing, 0 is too wide and -8 too tight.
    out.advanceAdjust = *(const float*)(t + 4);
    if (out.advanceAdjust < -16.0f || out.advanceAdjust > 16.0f) out.advanceAdjust = -4.0f;
    out.alpha.assign(f.begin() + pix, f.begin() + tbl);
    out.glyphs.resize(count);
    for (int i = 0; i < count; i++) {
        const uint8_t* r = t + 16 + i * 24;
        GameGlyph& g = out.glyphs[i];
        memcpy(&g.u0, r, 16);
        g.w = r[16]; g.h = r[17]; g.yoff = r[18];
    }
    out.map.resize(count);
    memcpy(out.map.data(), t + 16 + count * 24, count * 2);
    out.ok = true;
    return true;
}
