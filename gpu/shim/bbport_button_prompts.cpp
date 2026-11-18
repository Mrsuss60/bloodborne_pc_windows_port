// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_button_prompts.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <miniz.h>
#include <xxhash.h>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

extern "C" {
// runtime_file.c / runtime_pad.c
int runtime_file_translate(const char* guest, char* out, std::size_t size);
int runtime_pad_prompt(const char* name, int prefer_mouse, char* label, int size);
unsigned runtime_pad_prompt_generation(void);
int runtime_pad_prompt_device(void);
int runtime_pad_mouse_look(void);
// bbport_overlay.cpp: DejaVu Sans, embedded.
extern const unsigned char bb_font_ttf[];
}

namespace BbPrompts {
namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ---- The game's icons (menu/common.tpf.dcx) and the inputs they stand for. ----
enum class Layout {
    Single, ///< one input
    Column, ///< two inputs, one over the other (the d-pad's up and down)
    Row,    ///< two inputs side by side (left and right)
    Cross,  ///< up, left, down, right: a key cluster (W over A S D)
    Stick,  ///< a stick: its four keys, the mouse (look) or the pad's stick
};
struct IconDef {
    const char* name;
    Layout layout;
    std::array<const char*, 4> inputs; ///< runtime_pad.c input names
};
// The icons are known by what they show, not by their names: KG_OK is the circle and KG_Cancel
// the cross (the Japanese layout; a release whose confirm button is the cross shows KG_Cancel for
// OK). KG_R_U and KG_R_L are the triangle and the square (the right cluster's up and left).
constexpr IconDef Icons[] = {
    {"KG_OK", Layout::Single, {"circle"}},
    {"KG_Cancel", Layout::Single, {"cross"}},
    {"KG_R_U", Layout::Single, {"triangle"}},
    {"KG_R_L", Layout::Single, {"square"}},
    {"KG_L1", Layout::Single, {"l1"}},
    {"KG_L2", Layout::Single, {"l2"}},
    {"KG_R1", Layout::Single, {"r1"}},
    {"KG_R2", Layout::Single, {"r2"}},
    {"KG_L3", Layout::Single, {"l3"}},
    {"KG_R3", Layout::Single, {"r3"}},
    {"KG_Start", Layout::Single, {"options"}},
    {"KG_TP_L", Layout::Single, {"touchpad"}},
    {"KG_TP_R", Layout::Single, {"touchpad_right"}},
    {"KG_L_U", Layout::Single, {"up"}},
    {"KG_L_D", Layout::Single, {"down"}},
    {"KG_L_L", Layout::Single, {"left"}},
    {"KG_L_R", Layout::Single, {"right"}},
    {"KG_L_UD", Layout::Column, {"up", "down"}},
    {"KG_L_LR", Layout::Row, {"left", "right"}},
    {"KG_L_UDLR", Layout::Cross, {"up", "left", "down", "right"}},
    {"KG_LS", Layout::Stick, {"move_up", "move_left", "move_down", "move_right"}},
    {"KG_RS", Layout::Stick, {"look_up", "look_left", "look_down", "look_right"}},
};
constexpr std::size_t IconCount = std::size(Icons);
constexpr std::size_t GameIconBytes = 1024; // 32x32 BC7, one level

// Atlases of the HUD (ingametop.gfx) with icons in them, the same as the icons' textures (the
// drawing 3 pixels in from the cell's edges): MENU_Common_00091 has the circle, the cross and L3
// in a row (the lamps', gravestones' and items' "Examine" and "Pick up" prompts).
struct AtlasDef {
    const char* name;
    u32 width, height, bytes; ///< BC7, one level
    std::array<std::pair<u32, u32>, 3> cells;
    std::array<const char*, 3> icons;
};
constexpr AtlasDef Atlases[] = {
    {"MENU_Common_00091", 1024, 512, 1024 * 512, {{{658, 120}, {691, 120}, {723, 120}}},
     {"KG_OK", "KG_Cancel", "KG_L3"}},
};
constexpr std::size_t AtlasCount = std::size(Atlases);

std::array<u64, IconCount> icon_hashes{}; // 0: not in the game's file
std::array<u64, AtlasCount> atlas_hashes{}; // 0: not in the game's file
std::array<std::vector<AtlasCell>, AtlasCount> atlas_cells{};
std::array<std::array<u8, GameIconBytes>, IconCount> icon_bytes{}; // for BB_PROMPT_TRACE
std::once_flag loader_started;
std::thread loader;
std::once_flag loader_joined;
std::atomic<bool> table_ready{false};
std::atomic<u32> table_generation{0};

// A joinable std::thread calls std::terminate when it is destroyed. The loader is never waited
// for by the renderer, so join it here at exit (defined after `loader`: destroyed before it).
struct LoaderJoin {
    ~LoaderJoin() {
        if (loader.joinable()) {
            loader.join();
        }
    }
} loader_join;

u32 Be32(const u8* p) {
    return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3];
}
u32 Le32(const u8* p) {
    return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24;
}

/// The icons' hashes from the game's menu/common.tpf.dcx (DCX: zlib "DFLT"; TPF of the PS4).
void LoadTable() {
    char path[1024];
    constexpr const char* Guest = "/app0/dvdroot_ps4/menu/common.tpf.dcx";
    if (runtime_file_translate(Guest, path, sizeof(path)) != 0) {
        std::printf("Button prompts: %s not found: the game's icons stay\n", Guest);
        return;
    }
    std::vector<u8> file;
    if (FILE* f = std::fopen(path, "rb")) {
        std::fseek(f, 0, SEEK_END);
        file.resize(std::size_t(std::max(0L, std::ftell(f))));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(file.data(), 1, file.size(), f) != file.size()) {
            file.clear();
        }
        std::fclose(f);
    }
    // DCX header: "DCX\0", "DCS\0" at 0x18 with the sizes (big endian), "DCP\0" "DFLT", "DCA\0"
    // and its size at 0x44, then the zlib stream.
    if (file.size() < 0x50 || std::memcmp(file.data(), "DCX\0", 4) != 0 ||
        std::memcmp(file.data() + 0x18, "DCS\0", 4) != 0 ||
        std::memcmp(file.data() + 0x28, "DFLT", 4) != 0 ||
        std::memcmp(file.data() + 0x44, "DCA\0", 4) != 0) {
        std::printf("Button prompts: %s is not a DCX (DFLT) file\n", path);
        return;
    }
    const u32 unpacked = Be32(file.data() + 0x1c);
    const std::size_t stream = 0x44 + Be32(file.data() + 0x48);
    if (unpacked == 0 || unpacked > (256u << 20) || stream >= file.size()) {
        return;
    }
    std::vector<u8> tpf(unpacked);
    mz_ulong length = unpacked;
    if (mz_uncompress(tpf.data(), &length, file.data() + stream, mz_ulong(file.size() - stream)) !=
            MZ_OK ||
        length != unpacked) {
        std::printf("Button prompts: %s does not unpack\n", path);
        return;
    }
    // TPF (PS4): "TPF\0", count at 8, platform at 0xc, name encoding at 0xe (1: UTF-16), then
    // 36-byte entries from 0x10: data offset, size, format, type, levels, flags, width, height,
    // two words, the name's offset, a word, the DXGI format.
    if (tpf.size() < 0x10 || std::memcmp(tpf.data(), "TPF\0", 4) != 0) {
        return;
    }
    const u32 count = Le32(tpf.data() + 8);
    const bool utf16 = tpf[0xe] == 1 || tpf[0xe] == 2;
    constexpr std::size_t Stride = 36;
    u32 found = 0;
    for (u32 i = 0; i < count && 0x10 + (i + 1) * Stride <= tpf.size(); ++i) {
        const u8* e = tpf.data() + 0x10 + i * Stride;
        const u32 offset = Le32(e), size = Le32(e + 4), name_offset = Le32(e + 24);
        if (name_offset >= tpf.size()) {
            continue;
        }
        std::string name;
        for (std::size_t p = name_offset; p + (utf16 ? 1 : 0) < tpf.size(); p += utf16 ? 2 : 1) {
            const u32 c = utf16 ? u32(tpf[p]) | u32(tpf[p + 1]) << 8 : tpf[p];
            if (c == 0 || name.size() > 64) {
                break;
            }
            name.push_back(c < 0x80 ? char(c) : '?');
        }
        for (std::size_t k = 0; k < AtlasCount; ++k) {
            if (name == Atlases[k].name && size == Atlases[k].bytes &&
                u64(offset) + size <= tpf.size()) {
                atlas_hashes[k] = XXH3_64bits(tpf.data() + offset, size);
            }
        }
        for (std::size_t k = 0; k < IconCount; ++k) {
            if (name == Icons[k].name && size == GameIconBytes && u64(offset) + size <= tpf.size()) {
                icon_hashes[k] = XXH3_64bits(tpf.data() + offset, size);
                std::memcpy(icon_bytes[k].data(), tpf.data() + offset, GameIconBytes);
                ++found;
                if (const char* trace = std::getenv("BB_PROMPT_TRACE"); trace && trace[0] == '1') {
                    std::printf("Prompt trace: %s at %#x in the TPF, format %#x %ux%u, hash %016llx\n",
                                Icons[k].name, offset, e[8], u32(e[12]) | u32(e[13]) << 8,
                                u32(e[14]) | u32(e[15]) << 8, (unsigned long long)icon_hashes[k]);
                }
            }
        }
    }
    // The atlases' cells whose icons are known.
    u32 atlases = 0;
    for (std::size_t k = 0; k < AtlasCount; ++k) {
        for (std::size_t c = 0; atlas_hashes[k] && c < Atlases[k].cells.size(); ++c) {
            for (std::size_t i = 0; i < IconCount; ++i) {
                if (icon_hashes[i] && std::string_view{Icons[i].name} == Atlases[k].icons[c]) {
                    atlas_cells[k].push_back(
                        {Atlases[k].cells[c].first, Atlases[k].cells[c].second, u32(i + 1)});
                }
            }
        }
        atlases += !atlas_cells[k].empty();
    }
    std::printf("Button prompts: %u of %zu HUD icon atlases found\n", atlases, AtlasCount);
    // Not Enabled(): it waits for this thread.
    std::printf("Button prompts: %u of the game's %zu key guide icons found (%s)\n", found, IconCount,
                found != 0 ? "drawn for the keyboard and other gamepads; BB_BUTTON_PROMPTS=0: off"
                           : "off");
}

bool Switch() {
    static const bool on = [] {
        const char* env = std::getenv("BB_BUTTON_PROMPTS");
        return !(env && env[0] == '0');
    }();
    return on;
}

void WaitTable() {
    std::call_once(loader_joined, [] {
        Start();
        if (loader.joinable()) {
            loader.join();
        }
    });
}

// ---- Drawing: premultiplied float RGBA, shapes by signed distance (1 pixel of smoothing). ----
struct Color {
    float r, g, b, a = 1.0f;
};
constexpr Color Fill{0.075f, 0.07f, 0.065f, 0.93f};
constexpr Color Frame{0.78f, 0.74f, 0.64f};
constexpr Color Ink{0.95f, 0.92f, 0.85f};
constexpr Color Accent{0.82f, 0.64f, 0.36f};

struct Canvas {
    static constexpr int N = int(IconSize);
    std::vector<float> px = std::vector<float>(std::size_t(N) * N * 4, 0.0f);

    /// Paints `c` where `coverage(x, y)` (pixel centres) is above 0.
    template <typename F>
    void Paint(Color c, F&& coverage) {
        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                float k = coverage(float(x) + 0.5f, float(y) + 0.5f);
                if (k <= 0.0f) {
                    continue;
                }
                k = std::min(k, 1.0f) * c.a;
                float* p = &px[(std::size_t(y) * N + x) * 4];
                p[0] = c.r * k + p[0] * (1.0f - k);
                p[1] = c.g * k + p[1] * (1.0f - k);
                p[2] = c.b * k + p[2] * (1.0f - k);
                p[3] = k + p[3] * (1.0f - k);
            }
        }
    }
    /// Straight-alpha RGBA8, as the game's own (TGA-made) icons.
    void Store(u32* out) const {
        for (std::size_t i = 0; i < std::size_t(N) * N; ++i) {
            const float a = px[i * 4 + 3];
            const auto unit = [&](float v) {
                const float straight = a > 0.0f ? std::clamp(v / a, 0.0f, 1.0f) : 0.0f;
                return u32(std::lround(straight * 255.0f));
            };
            out[i] = unit(px[i * 4]) | unit(px[i * 4 + 1]) << 8 | unit(px[i * 4 + 2]) << 16 |
                     u32(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f)) << 24;
        }
    }
};

float Cover(float distance) {
    return std::clamp(0.5f - distance, 0.0f, 1.0f);
}
float RoundRect(float x, float y, float cx, float cy, float hw, float hh, float r) {
    const float qx = std::abs(x - cx) - (hw - r), qy = std::abs(y - cy) - (hh - r);
    const float outside = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f));
    return outside + std::min(std::max(qx, qy), 0.0f) - r;
}
float Circle(float x, float y, float cx, float cy, float r) {
    return std::hypot(x - cx, y - cy) - r;
}
float Segment(float x, float y, float ax, float ay, float bx, float by, float half_width) {
    const float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
    const float t = std::clamp((px * dx + py * dy) / std::max(dx * dx + dy * dy, 1e-6f), 0.0f, 1.0f);
    return std::hypot(px - dx * t, py - dy * t) - half_width;
}

/// A filled shape with a frame of `stroke` pixels on its edge.
template <typename D>
void Plate(Canvas& c, float stroke, D&& distance, Color fill = Fill, Color frame = Frame) {
    c.Paint(fill, [&](float x, float y) { return Cover(distance(x, y)); });
    c.Paint(frame, [&](float x, float y) { return Cover(std::abs(distance(x, y) + stroke * 0.5f) - stroke * 0.5f); });
}

// Text through stb_truetype (DejaVu Sans: Latin, Cyrillic, arrows).
stbtt_fontinfo font;
bool font_ready = false;

std::u32string Decode(std::string_view utf8) {
    std::u32string out;
    for (std::size_t i = 0; i < utf8.size();) {
        u32 c = u8(utf8[i]);
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        c &= extra == 3 ? 0x07 : extra == 2 ? 0x0f : extra == 1 ? 0x1f : 0x7f;
        ++i;
        for (; extra > 0 && i < utf8.size(); --extra, ++i) {
            c = c << 6 | (u8(utf8[i]) & 0x3f);
        }
        out.push_back(char32_t(c));
    }
    return out;
}

/// `text` centred on (cx, cy), at most `max_w` wide and `max_h` tall (its ink, not the font's
/// line height: key caps hold capitals and digits).
void Text(Canvas& c, std::string_view text, float cx, float cy, float max_w, float max_h, Color ink) {
    if (!font_ready || text.empty()) {
        return;
    }
    const std::u32string glyphs = Decode(text);
    // The ink box at 1 pixel per unit of a reference size, then the scale that fits it.
    const auto measure = [&](float scale, int& x0, int& y0, int& x1, int& y1) {
        float pen = 0.0f;
        x0 = y0 = 1 << 20;
        x1 = y1 = -(1 << 20);
        for (std::size_t i = 0; i < glyphs.size(); ++i) {
            int advance = 0, bearing = 0, gx0, gy0, gx1, gy1;
            stbtt_GetCodepointHMetrics(&font, int(glyphs[i]), &advance, &bearing);
            stbtt_GetCodepointBitmapBoxSubpixel(&font, int(glyphs[i]), scale, scale, pen - std::floor(pen),
                                                0.0f, &gx0, &gy0, &gx1, &gy1);
            const int ox = int(std::floor(pen));
            if (gx1 > gx0) {
                x0 = std::min(x0, ox + gx0);
                x1 = std::max(x1, ox + gx1);
                y0 = std::min(y0, gy0);
                y1 = std::max(y1, gy1);
            }
            pen += advance * scale;
            if (i + 1 < glyphs.size()) {
                pen += stbtt_GetCodepointKernAdvance(&font, int(glyphs[i]), int(glyphs[i + 1])) * scale;
            }
        }
        return x1 > x0;
    };
    float scale = stbtt_ScaleForPixelHeight(&font, 100.0f);
    int x0, y0, x1, y1;
    if (!measure(scale, x0, y0, x1, y1)) {
        return;
    }
    scale *= std::min(max_w / float(x1 - x0), max_h / float(y1 - y0));
    measure(scale, x0, y0, x1, y1);
    const float left = std::round(cx - float(x1 - x0) * 0.5f) - float(x0);
    const float base = std::round(cy - float(y1 - y0) * 0.5f) - float(y0);
    float pen = 0.0f;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        int advance = 0, bearing = 0;
        stbtt_GetCodepointHMetrics(&font, int(glyphs[i]), &advance, &bearing);
        const float shift = pen - std::floor(pen);
        int w = 0, h = 0, gx = 0, gy = 0;
        unsigned char* bitmap = stbtt_GetCodepointBitmapSubpixel(&font, scale, scale, shift, 0.0f,
                                                                 int(glyphs[i]), &w, &h, &gx, &gy);
        if (bitmap) {
            const int ox = int(left + std::floor(pen)) + gx, oy = int(base) + gy;
            c.Paint(ink, [&](float x, float y) {
                const int bx = int(x) - ox, by = int(y) - oy;
                return bx >= 0 && by >= 0 && bx < w && by < h ? bitmap[by * w + bx] / 255.0f : 0.0f;
            });
            stbtt_FreeBitmap(bitmap, nullptr);
        }
        pen += advance * scale;
        if (i + 1 < glyphs.size()) {
            pen += stbtt_GetCodepointKernAdvance(&font, int(glyphs[i]), int(glyphs[i + 1])) * scale;
        }
    }
}

/// A key cap with its label.
void Key(Canvas& c, std::string_view label, float cx, float cy, float hw, float hh) {
    const float stroke = std::max(3.0f, hw * 0.09f);
    const float radius = std::min(hw, hh) * 0.32f;
    Plate(c, stroke, [&](float x, float y) { return RoundRect(x, y, cx, cy, hw, hh, radius); });
    const std::size_t glyphs = Decode(label).size();
    // One character fills the cap; longer names get smaller.
    const float max_h = (hh - stroke) * (glyphs <= 1 ? 1.15f : glyphs <= 3 ? 0.9f : 0.7f);
    Text(c, label, cx, cy, (hw - stroke) * 1.55f, max_h, Ink);
}

/// A mouse, the bound button lit: Left, Right, Middle, X1, X2, Wheel Up, Wheel Down; Move: none
/// lit, arrows at its sides (the camera).
void Mouse(Canvas& c, std::string_view button) {
    constexpr float cx = 64, cy = 66, hw = 33, hh = 50, r = 31, stroke = 5;
    const auto body = [&](float x, float y) { return RoundRect(x, y, cx, cy, hw, hh, r); };
    c.Paint(Fill, [&](float x, float y) { return Cover(body(x, y)); });
    constexpr float split = 58; // the buttons' lower edge
    const auto lit = [&](auto&& region) {
        c.Paint(Accent, [&](float x, float y) { return std::min(Cover(body(x, y) + stroke), region(x, y)); });
    };
    if (button == "Left") {
        lit([&](float x, float y) { return std::min(Cover(x - cx), Cover(y - split)); });
    } else if (button == "Right") {
        lit([&](float x, float y) { return std::min(Cover(cx - x), Cover(y - split)); });
    }
    // The buttons' lines.
    c.Paint(Frame, [&](float x, float y) { return Cover(Segment(x, y, cx, cy - hh + 2, cx, split, 2.0f)); });
    c.Paint(Frame, [&](float x, float y) { return Cover(Segment(x, y, cx - hw, split, cx + hw, split, 2.0f)); });
    // The wheel.
    const bool wheel = button == "Middle" || button == "Wheel Up" || button == "Wheel Down";
    c.Paint(Fill, [&](float x, float y) { return Cover(Segment(x, y, cx, 28, cx, 46, 7.5f)); });
    c.Paint(wheel ? Accent : Frame, [&](float x, float y) {
        const float d = Segment(x, y, cx, 28, cx, 46, 5.5f);
        return wheel ? Cover(d) : Cover(std::abs(d + 1.25f) - 1.25f);
    });
    if (button == "Wheel Up" || button == "Wheel Down") {
        const bool up = button == "Wheel Up";
        Text(c, up ? "\xe2\x96\xb2" : "\xe2\x96\xbc", cx, 90, 26, 20, Accent);
    }
    if (button == "X1" || button == "X2") {
        const float y = button == "X1" ? 92 : 74; // X1 back (lower), X2 forward
        c.Paint(Accent, [&](float x, float yy) { return Cover(RoundRect(x, yy, cx - hw + 1, y, 6, 8, 4)); });
    }
    c.Paint(Frame, [&](float x, float y) { return Cover(std::abs(body(x, y) + stroke * 0.5f) - stroke * 0.5f); });
    if (button == "Move") {
        Text(c, "\xe2\x86\x90", 14, cy, 24, 22, Ink);
        Text(c, "\xe2\x86\x92", 114, cy, 24, 22, Ink);
    }
}

/// A round button of another gamepad: A B X Y in their colours, else its name (LS, RS) or sign
/// (Menu, View).
void Round(Canvas& c, std::string_view label, bool face) {
    constexpr float cx = 64, cy = 64, radius = 56, stroke = 5;
    Plate(c, stroke, [&](float x, float y) { return Circle(x, y, cx, cy, radius); });
    if (face) {
        const Color ink = label == "A"   ? Color{0.40f, 0.76f, 0.30f}
                          : label == "B" ? Color{0.88f, 0.27f, 0.23f}
                          : label == "X" ? Color{0.27f, 0.55f, 0.92f}
                          : label == "Y" ? Color{0.95f, 0.78f, 0.18f}
                                         : Ink;
        Text(c, label, cx, cy, 70, 58, ink);
        return;
    }
    if (label == "Menu") {
        for (const float y : {48.0f, 64.0f, 80.0f}) {
            c.Paint(Ink, [&](float x, float yy) { return Cover(Segment(x, yy, 44, y, 84, y, 4.0f)); });
        }
        return;
    }
    if (label == "View") {
        const auto box = [&](float bx, float by) {
            return [=](float x, float y) { return RoundRect(x, y, bx, by, 15, 13, 4); };
        };
        const auto back = box(56, 56), front = box(72, 72);
        c.Paint(Ink, [&](float x, float y) { return Cover(std::abs(back(x, y) + 2.0f) - 2.0f); });
        c.Paint(Fill, [&](float x, float y) { return Cover(front(x, y)); });
        c.Paint(Ink, [&](float x, float y) { return Cover(std::abs(front(x, y) + 2.0f) - 2.0f); });
        return;
    }
    Text(c, label, cx, cy, 78, label.size() <= 2 ? 46 : 34, Ink);
}

/// A shoulder button or a trigger of another gamepad.
void Shoulder(Canvas& c, std::string_view label) {
    const bool trigger = label == "LT" || label == "RT" || label == "ZL" || label == "ZR";
    const float hw = trigger ? 48 : 58, hh = trigger ? 54 : 34, r = trigger ? 26 : 16;
    Plate(c, 5, [&](float x, float y) { return RoundRect(x, y, 64, 64, hw, hh, r); });
    Text(c, label, 64, 64, hw * 1.4f, 42, Ink);
}

/// One icon for the device in use; false: the game's own stays.
bool Draw(const IconDef& icon, Canvas& c) {
    char labels[4][32] = {};
    int kinds[4] = {};
    const bool single = icon.layout == Layout::Single;
    for (int i = 0; i < 4 && icon.inputs[i]; ++i) {
        kinds[i] = runtime_pad_prompt(icon.inputs[i], single ? 1 : 0, labels[i], sizeof(labels[i]));
    }
    const int device = runtime_pad_prompt_device();
    if (icon.layout == Layout::Stick && device == 2) {
        if (icon.inputs[0] == std::string_view{"look_up"} && runtime_pad_mouse_look()) {
            Mouse(c, "Move");
            return true;
        }
    } else if (icon.layout == Layout::Stick || single) {
        switch (kinds[0]) {
        case 'K':
            Key(c, labels[0], 64, 64, 56, 48);
            return true;
        case 'M':
            Mouse(c, labels[0]);
            return true;
        case 'F':
            Round(c, labels[0], true);
            return true;
        case 'S':
            Shoulder(c, labels[0]);
            return true;
        case 'C':
            Round(c, labels[0], false);
            return true;
        default:
            return false;
        }
    }
    // Key clusters (the keyboard only: on a gamepad the d-pad's icons are the game's).
    if (device != 2) {
        return false;
    }
    bool any = false;
    for (int i = 0; i < 4; ++i) {
        any = any || kinds[i] == 'K';
    }
    if (!any) {
        return false;
    }
    const auto key = [&](int i, float cx, float cy, float hw, float hh) {
        Key(c, kinds[i] == 'K' ? labels[i] : "", cx, cy, hw, hh);
    };
    switch (icon.layout) {
    case Layout::Column:
        key(0, 64, 34, 34, 28);
        key(1, 64, 94, 34, 28);
        break;
    case Layout::Row:
        key(0, 34, 64, 28, 34);
        key(1, 94, 64, 28, 34);
        break;
    default: // Cross, Stick: the first over the other three
        key(0, 64, 30, 20, 21);
        key(1, 21, 86, 20, 21);
        key(2, 64, 86, 20, 21);
        key(3, 107, 86, 20, 21);
        break;
    }
    return true;
}

} // namespace

void Start() {
    std::call_once(loader_started, [] {
        if (!Switch()) {
            return;
        }
        font_ready = stbtt_InitFont(&font, bb_font_ttf, stbtt_GetFontOffsetForIndex(bb_font_ttf, 0)) != 0;
        loader = std::thread{[] {
            LoadTable();
            table_ready.store(true, std::memory_order_release);
            table_generation.fetch_add(1, std::memory_order_release);
        }};
    });
}

bool Ready() {
    return table_ready.load(std::memory_order_acquire);
}

std::uint32_t TableGeneration() {
    return table_generation.load(std::memory_order_acquire);
}

bool Enabled() {
    if (!Switch()) {
        return false;
    }
    WaitTable();
    return std::any_of(icon_hashes.begin(), icon_hashes.end(), [](u64 h) { return h != 0; });
}

const std::uint8_t* IconBytes(std::uint32_t icon) {
    if (!Ready() || icon < 1 || icon > IconCount || !icon_hashes[icon - 1]) {
        return nullptr;
    }
    return icon_bytes[icon - 1].data();
}

u32 IconOf(const void* data, std::size_t size) {
    if (!Switch() || size < GameIconBytes || !Ready()) {
        return 0;
    }
    const u64 hash = XXH3_64bits(data, GameIconBytes);
    for (std::size_t i = 0; i < IconCount; ++i) {
        if (icon_hashes[i] != 0 && icon_hashes[i] == hash) {
            return u32(i + 1);
        }
    }
    return 0;
}

u32 AtlasOf(const void* data, std::size_t size, u32 width, u32 height) {
    if (!Switch() || !Ready()) {
        return 0;
    }
    for (std::size_t k = 0; k < AtlasCount; ++k) {
        if (Atlases[k].width != width || Atlases[k].height != height || size < Atlases[k].bytes) {
            continue;
        }
        if (atlas_hashes[k] && !atlas_cells[k].empty() &&
            XXH3_64bits(data, Atlases[k].bytes) == atlas_hashes[k]) {
            return u32(k + 1);
        }
    }
    return 0;
}

std::span<const AtlasCell> AtlasCells(u32 atlas) {
    if (atlas == 0 || atlas > AtlasCount || !Ready()) {
        return {};
    }
    return atlas_cells[atlas - 1];
}

bool Update(u32 generation, u32 table_generation_seen, Set& out) {
    if (!Switch() || !Ready()) {
        return false; // the table is still being read: its generation changes when it is done
    }
    const u32 now = runtime_pad_prompt_generation();
    const u32 table = TableGeneration();
    if (now == generation && table == table_generation_seen) {
        return false;
    }
    out.generation = now;
    out.count = u32(IconCount);
    out.drawn.assign(IconCount, false);
    out.pixels.assign(IconCount * IconSize * IconSize, 0);
    for (std::size_t i = 0; i < IconCount; ++i) {
        Canvas canvas;
        if (icon_hashes[i] != 0 && Draw(Icons[i], canvas)) {
            out.drawn[i] = true;
            canvas.Store(out.pixels.data() + i * IconSize * IconSize);
        }
    }
    static const char* const devices[] = {"the game's (PlayStation pad)", "the gamepad's", "the keyboard's and mouse's"};
    const int device = runtime_pad_prompt_device();
    if (device >= 0 && device <= 2) {
        std::printf("Button prompts: %s\n", devices[device]);
    }
    return true;
}

} // namespace BbPrompts
