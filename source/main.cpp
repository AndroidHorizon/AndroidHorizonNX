#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <sys/stat.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

#include "apk.h"
#include "compat/loader.h"
#include "build_number.h"
#include "avatar.h"

extern void compatLogFmt(const char* fmt, ...);

static const char* APK_DIR  = "sdmc:/AndroidHorizonNX/apks";
static const char* LOG_FILE = "sdmc:/AndroidHorizonNX/log.txt";

// ---------------------------------------------------------------------------
// Layout (1280×720)
// ---------------------------------------------------------------------------
static const int SW       = 1280;
static const int SH       = 720;
static const int HEADER_H = 72;
static const int FOOTER_H = 48;
static const int LIST_Y   = HEADER_H;
static const int LIST_H   = SH - HEADER_H - FOOTER_H;
static const int ITEM_H   = 108;
static const int ICON_SZ  = 84;
static const int VISIBLE  = LIST_H / ITEM_H;

// Colors — matched to the app icon: deep-space indigo sky + horizon green planet
static const SDL_Color C_BG     = {13,  12,  46,  255};   // space navy
static const SDL_Color C_HEADER = {24,  22,  86,  255};   // icon sky indigo
static const SDL_Color C_FOOTER = {9,   8,   34,  255};
static const SDL_Color C_SEL    = {41,  37,  128, 255};   // indigo highlight
static const SDL_Color C_DIV    = {40,  37,  108, 255};
static const SDL_Color C_WHITE  = {255, 255, 255, 255};
static const SDL_Color C_GRAY   = {168, 170, 205, 255};
static const SDL_Color C_DIM    = {116, 116, 168, 255};
static const SDL_Color C_OK     = {52,  230, 134, 255};   // planet-rim green
static const SDL_Color C_ERR    = {235, 90,  90,  255};
static const SDL_Color C_WARN   = {230, 190, 70,  255};
static const SDL_Color C_INST   = {47,  223, 124, 255};
static const SDL_Color C_RIM    = {52,  230, 134, 255};   // horizon accent line

// ---------------------------------------------------------------------------
static FILE* g_log = nullptr;
static void logOpen()  { g_log = fopen(LOG_FILE, "w"); }
static void logClose() { if (g_log) { fclose(g_log); g_log = nullptr; } }
static void logMsg(const char* msg) {
    if (g_log) { fputs(msg, g_log); fputc('\n', g_log); fflush(g_log); }
}
static void logSDL(const char* prefix) {
    if (!g_log) return;
    fputs(prefix, g_log); fputs(": ", g_log);
    fputs(SDL_GetError(), g_log); fputc('\n', g_log); fflush(g_log);
}

// ---------------------------------------------------------------------------
static const int BTN_A     = 0;
static const int BTN_B     = 1;
static const int BTN_X     = 2;
static const int BTN_Y     = 3;
static const int BTN_PLUS  = 10;
static const int BTN_MINUS = 11;

// ---------------------------------------------------------------------------
// Shared loader state — written by loader thread, read by main thread.
// ---------------------------------------------------------------------------
// Ring buffers defined in loader.cpp
extern char g_ui_log[20][128];   // throttled UI messages (every 512 entries etc.)
extern int  g_ui_head;
extern int  g_ui_pct;
// Full-detail log: every compatLog line written here — read by render thread without file I/O
extern char g_detail_log[28][164];
extern int  g_detail_head;

// Current high-level stage string, set by progressCallback on the loader thread.
static char g_ui_stage[80] = "Working...";

// ---------------------------------------------------------------------------
// Loader thread plumbing
// ---------------------------------------------------------------------------
struct LoaderCtx {
    std::string       apk_path;
    std::string       pkg_name;
    bool              skip_install = false;
    LaunchResult      result;
    std::atomic<bool> done{false};
};

static LoaderCtx* g_loader_ctx = nullptr;

// Progress callback — called from loader thread.
// Updates shared state only; never touches SDL (wrong thread).
static void progressCallback(const char* stage, const char* /*detail*/) {
    if (stage) {
        strncpy(g_ui_stage, stage, sizeof(g_ui_stage) - 1);
        g_ui_stage[sizeof(g_ui_stage) - 1] = '\0';
    }
}

static void loaderThreadFn(void*) {
    g_loader_ctx->result = launchApk(
        g_loader_ctx->apk_path,
        g_loader_ctx->pkg_name,
        progressCallback,
        g_loader_ctx->skip_install
    );
    g_loader_ctx->done.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
struct App {
    SDL_Window*    win  = nullptr;
    SDL_Renderer*  rdr  = nullptr;
    TTF_Font*      fLg  = nullptr;
    TTF_Font*      fMd  = nullptr;  // 20px — monospace-ish for log lines
    TTF_Font*      fSm  = nullptr;
    SDL_Joystick*  joy  = nullptr;

    std::vector<ApkInfo>      apks;
    std::vector<SDL_Texture*> icons;
    int selected = 0;
    int scroll   = 0;

    SDL_Texture* avatarTex = nullptr;

    // ── Scenery: cached sky/planet texture + animated starfield ─────────
    SDL_Texture* bgTex = nullptr;
    TTF_Font*    fBtn  = nullptr;  // NintendoExt shared font: HOS button glyphs
    struct Star { float x, y; int sz; float phase, speed; };
    std::vector<Star> stars;
    float selAnimY = -1.0f;        // eased focus-card position (borealis-style)

    // One-shot README screenshot flags (each screen captured once per run)
    bool shotMenu = false, shotLoading = false, shotResult = false, shotAbout = false;

    // A game session leaves JIT regions and worker threads behind that we
    // can't fully unload yet — a second launch reads garbage ("not an ARM
    // binary"). Block relaunch until the app is restarted.
    bool   gameRanOnce = false;
    Uint32 noticeUntil = 0;
    std::string noticeText = "One game session per launch for now — restart Android Horizon to play again";

    // Save the composed frame (call just before SDL_RenderPresent) as a PNG in
    // sdmc:/AndroidHorizonNX/screenshots/ — showcase material for the README.
    void saveScreenshot(const char* name) {
        mkdir("sdmc:/AndroidHorizonNX/screenshots", 0777);
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(
            0, SW, SH, 32, SDL_PIXELFORMAT_ABGR8888);
        if (!s) return;
        if (SDL_RenderReadPixels(rdr, nullptr, s->format->format,
                                 s->pixels, s->pitch) == 0) {
            char path[128];
            snprintf(path, sizeof(path), "sdmc:/AndroidHorizonNX/screenshots/%s", name);
            if (IMG_SavePNG(s, path) == 0) logMsg(path);
        }
        SDL_FreeSurface(s);
    }

    // ------------------------------------------------------------------
    TTF_Font* openFont(int ptsize) {
        plInitialize(PlServiceType_User);
        PlFontData fd = {};
        if (plGetSharedFontByType(&fd, PlSharedFontType_Standard) == 0 && fd.size > 8) {
            SDL_RWops* rw = SDL_RWFromConstMem(
                (const uint8_t*)fd.address + 8, (int)fd.size - 8);
            TTF_Font* f = TTF_OpenFontRW(rw, 1, ptsize);
            if (f) { logMsg("  font: system BFTTF"); return f; }
            logSDL("  BFTTF open failed");
        }
        romfsInit();
        TTF_Font* f = TTF_OpenFont("romfs:/fonts/DejaVuSans.ttf", ptsize);
        if (f) { logMsg("  font: romfs DejaVuSans"); return f; }
        logSDL("  romfs font open failed");
        return nullptr;
    }

    // NintendoExt shared font: circled A/B/X/Y/+/- button glyphs (U+E0E0…).
    // Returns nullptr on failure — callers fall back to plain-text hints.
    TTF_Font* openExtFont(int ptsize) {
        PlFontData fd = {};
        if (plGetSharedFontByType(&fd, PlSharedFontType_NintendoExt) == 0 && fd.size > 8) {
            SDL_RWops* rw = SDL_RWFromConstMem(
                (const uint8_t*)fd.address + 8, (int)fd.size - 8);
            TTF_Font* f = TTF_OpenFontRW(rw, 1, ptsize);
            if (f) { logMsg("  font: NintendoExt glyphs"); return f; }
        }
        logMsg("  NintendoExt font unavailable — text hints");
        return nullptr;
    }

    // ------------------------------------------------------------------
    bool init() {
        mkdir("sdmc:/AndroidHorizonNX", 0777);
        logOpen();
        logMsg("Android Horizon starting");
        socketInitializeDefault();

        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
            logSDL("SDL_Init failed"); logClose(); return false;
        }
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        if (IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP) == 0)
            logSDL("IMG_Init warning");
        if (TTF_Init() != 0) {
            logSDL("TTF_Init failed"); logClose(); return false;
        }

        win = SDL_CreateWindow("Android Horizon",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            SW, SH, SDL_WINDOW_SHOWN);
        if (!win) { logSDL("CreateWindow failed"); logClose(); return false; }

        rdr = SDL_CreateRenderer(win, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!rdr) {
            logSDL("Accelerated renderer failed, trying software");
            rdr = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        }
        if (!rdr) { logSDL("CreateRenderer failed"); logClose(); return false; }

        SDL_SetRenderDrawBlendMode(rdr, SDL_BLENDMODE_BLEND);

        fLg = openFont(28);
        fMd = openFont(20);
        fSm = openFont(17);
        if (!fLg || !fSm) { logMsg("Font load failed"); logClose(); return false; }
        if (!fMd) fMd = fSm;
        fBtn = openExtFont(22);

        buildBackground();

        if (SDL_NumJoysticks() > 0) {
            joy = SDL_JoystickOpen(0);
            if (!joy) logSDL("JoystickOpen warning");
        }
        logMsg("init complete");
        return true;
    }

    // ------------------------------------------------------------------
    void cleanup() {
        avatarStop();
        if (avatarTex) SDL_DestroyTexture(avatarTex);
        if (bgTex) SDL_DestroyTexture(bgTex);
        for (auto* t : icons) if (t) SDL_DestroyTexture(t);
        if (fBtn) TTF_CloseFont(fBtn);
        if (fLg)  TTF_CloseFont(fLg);
        if (fMd && fMd != fSm) TTF_CloseFont(fMd);
        if (fSm)  TTF_CloseFont(fSm);
        if (joy)  SDL_JoystickClose(joy);
        if (rdr)  SDL_DestroyRenderer(rdr);
        if (win)  SDL_DestroyWindow(win);
        socketExit();
        romfsExit(); plExit();
        TTF_Quit(); IMG_Quit(); SDL_Quit();
        logMsg("cleanup done");
        logClose();
    }

    // ------------------------------------------------------------------
    void fill(int x, int y, int w, int h, SDL_Color c) {
        SDL_SetRenderDrawColor(rdr, c.r, c.g, c.b, c.a);
        SDL_Rect r = {x, y, w, h};
        SDL_RenderFillRect(rdr, &r);
    }

    int drawText(TTF_Font* f, const std::string& s, SDL_Color col, int x, int y) {
        if (s.empty() || !f) return 0;
        SDL_Surface* surf = TTF_RenderUTF8_Blended(f, s.c_str(), col);
        if (!surf) return 0;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(rdr, surf);
        int w = surf->w;
        SDL_FreeSurface(surf);
        if (!tex) return 0;
        int tw, th;
        SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
        SDL_Rect dst = {x, y, tw, th};
        SDL_RenderCopy(rdr, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
        return w;
    }

    // ── Scenery ─────────────────────────────────────────────────────────
    // The icon look: deep-space gradient sky, twinkling stars, and a green
    // planet horizon rising from the bottom edge with a glowing rim.
    // Sky + planet are static → rendered once into bgTex; stars animate live.

    static SDL_Color lerpCol(SDL_Color a, SDL_Color b, float t) {
        return { (Uint8)(a.r + (b.r - a.r) * t), (Uint8)(a.g + (b.g - a.g) * t),
                 (Uint8)(a.b + (b.b - a.b) * t), 255 };
    }

    void fillCircle(int cx, int cy, int r, SDL_Color c) {
        SDL_SetRenderDrawColor(rdr, c.r, c.g, c.b, c.a);
        for (int dy = -r; dy <= r; dy++) {
            int hw = (int)sqrtf((float)(r * r - dy * dy));
            SDL_Rect row = {cx - hw, cy + dy, hw * 2, 1};
            SDL_RenderFillRect(rdr, &row);
        }
    }

    static constexpr float PLANET_R    = 2200.0f;  // huge circle → gentle curve
    static constexpr int   PLANET_BUMP = 130;      // rim height above bottom edge

    void buildBackground() {
        // Starfield layout (deterministic so it doesn't reshuffle per launch)
        stars.clear();
        uint32_t rng = 0x5EED5EED;
        auto rnd = [&rng]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
        for (int i = 0; i < 110; i++) {
            Star s;
            s.x     = (float)(rnd() % SW);
            s.y     = (float)(rnd() % (SH - PLANET_BUMP - 80));
            s.sz    = (rnd() % 100 < 16) ? 2 : 1;
            s.phase = (rnd() % 628) / 100.0f;
            s.speed = 0.35f + (rnd() % 100) / 90.0f;
            stars.push_back(s);
        }

        // Sky/planet/horizon glow is authored as a real SVG (romfs:/background.svg
        // — sky gradient, huge off-screen planet ellipse, radial glow)
        // instead of hand-coded scanline math for the same look — easier to
        // art-direct in an actual vector tool. SDL2_image's own portlib
        // already bundles nanosvg internally (confirmed the hard way: vendoring
        // our own copy collided at link time with symbols already inside
        // libSDL2_image.a), so IMG_Load() rasterizes it directly, same as any
        // other image format this project already loads. The animated
        // starfield stays separate, drawn fresh every frame in
        // drawBackground() on top of this static texture.
        SDL_Surface* svgSurf = IMG_Load("romfs:/background.svg");
        if (!svgSurf) { logSDL("background.svg load failed — flat background"); return; }
        bgTex = SDL_CreateTextureFromSurface(rdr, svgSurf);
        SDL_FreeSurface(svgSurf);
        if (!bgTex) logSDL("bg texture failed — flat background");
    }

    void drawBackground() {
        Uint32 now = SDL_GetTicks();
        if (bgTex) SDL_RenderCopy(rdr, bgTex, nullptr, nullptr);
        else       fill(0, 0, SW, SH, C_BG);
        // Twinkling, slowly drifting stars
        for (auto& s : stars) {
            s.x -= 0.02f * s.speed;
            if (s.x < 0) s.x += SW;
            float tw = 0.5f + 0.5f * sinf(now / 1000.0f * s.speed * 6.2832f + s.phase);
            Uint8 a  = (Uint8)(70 + 170 * tw);
            fill((int)s.x, (int)s.y, s.sz, s.sz, {230, 235, 255, a});
            if (s.sz > 1 && tw > 0.75f) {  // sparkle cross on bright big stars
                fill((int)s.x - 2, (int)s.y, 6, 1, {230, 235, 255, (Uint8)(a / 3)});
                fill((int)s.x, (int)s.y - 2, 1, 6, {230, 235, 255, (Uint8)(a / 3)});
            }
        }
    }

    // Shared translucent header: "Android Horizon" with green accent + rim line
    void drawHeaderBar(const std::string& rightText = "") {
        fill(0, 0, SW, HEADER_H, {24, 22, 86, 205});
        fill(0, HEADER_H - 3, SW, 3, C_RIM);
        int w = drawText(fLg, "Android ", C_WHITE, 30, (HEADER_H - 28) / 2);
        w += drawText(fLg, "Horizon", C_OK, 30 + w, (HEADER_H - 28) / 2);
        drawText(fSm, BUILD_VERSION, C_DIM, 30 + w + 14, (HEADER_H + 4) / 2);
        if (!rightText.empty()) {
            int tw = 0, th = 0;
            TTF_SizeUTF8(fSm, rightText.c_str(), &tw, &th);
            drawText(fSm, rightText, C_DIM, SW - tw - 30, (HEADER_H - 18) / 2);
        }
    }

    // HOS-style footer hints, right-aligned: {glyph-utf8-or-letter, label}.
    // With the NintendoExt font the glyph IS the circled button; otherwise we
    // draw our own chip with the letter.
    void drawFooterBar(const std::vector<std::pair<std::string, std::string>>& hints,
                       const std::string& leftText = "") {
        fill(0, SH - FOOTER_H, SW, FOOTER_H, {9, 8, 34, 225});
        fill(0, SH - FOOTER_H, SW, 2, C_RIM);
        int cy = SH - FOOTER_H / 2;
        if (!leftText.empty())
            drawText(fSm, leftText, C_WARN, 30, cy - 9);
        int x = SW - 30;
        for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
            int lw = 0, lh = 0;
            TTF_SizeUTF8(fSm, it->second.c_str(), &lw, &lh);
            x -= lw;
            drawText(fSm, it->second, C_GRAY, x, cy - lh / 2);
            x -= 8;
            if (fBtn && it->first.size() > 1) {   // real HOS glyph
                int gw = 0, gh = 0;
                TTF_SizeUTF8(fBtn, it->first.c_str(), &gw, &gh);
                x -= gw;
                drawText(fBtn, it->first, C_WHITE, x, cy - gh / 2);
            } else {                              // fallback chip
                x -= 26;
                fillCircle(x + 13, cy, 13, {41, 37, 128, 255});
                std::string letter = it->first.size() > 1 ? "?" : it->first;
                int gw = 0, gh = 0;
                TTF_SizeUTF8(fSm, letter.c_str(), &gw, &gh);
                drawText(fSm, letter, C_WHITE, x + 13 - gw / 2, cy - gh / 2);
            }
            x -= 34;
        }
    }

    // HOS button glyphs in the NintendoExt shared font
    static constexpr const char* GLYPH_A     = "\xEE\x83\xA0";  // U+E0E0
    static constexpr const char* GLYPH_B     = "\xEE\x83\xA1";  // U+E0E1
    static constexpr const char* GLYPH_X     = "\xEE\x83\xA2";  // U+E0E2
    static constexpr const char* GLYPH_Y     = "\xEE\x83\xA3";  // U+E0E3
    static constexpr const char* GLYPH_PLUS  = "\xEE\x83\xAF";  // U+E0EF (+)
    static constexpr const char* GLYPH_MINUS = "\xEE\x83\xB0";  // U+E0F0 (-)

    // Pick the HOS glyph when the ext font loaded, else the plain letter
    // (drawFooterBar renders single-char hints as its own chip).
    std::string BG(const char* glyph, const char* letter) const {
        return fBtn ? glyph : letter;
    }

    static std::string formatSize(uint64_t bytes) {
        char buf[32];
        if (bytes >= 1024ull * 1024 * 1024)
            snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024 * 1024));
        else if (bytes >= 1024ull * 1024)
            snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
        else if (bytes >= 1024ull)
            snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
        else
            snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
        return buf;
    }

    void drawMonogram(const std::string& name, int x, int y, int sz) {
        static const SDL_Color PALETTE[] = {
            {239, 83,  80,  255}, {171, 71,  188, 255}, {66,  165, 245, 255},
            {38,  166, 154, 255}, {255, 167, 38,  255}, {126, 87,  194, 255},
            {92,  107, 192, 255}, {255, 112, 67,  255},
        };
        uint32_t h = 2166136261u;
        for (char c : name) h = (h ^ (uint8_t)c) * 16777619u;
        SDL_Color bg = PALETTE[h % (sizeof(PALETTE) / sizeof(PALETTE[0]))];
        fill(x, y, sz, sz, bg);
        char letter = name.empty() ? '?' : (char)toupper((unsigned char)name[0]);
        std::string s(1, letter);
        int w = 0, h2 = 0;
        TTF_SizeUTF8(fLg, s.c_str(), &w, &h2);
        drawText(fLg, s, C_WHITE, x + (sz - w) / 2, y + (sz - h2) / 2);
    }

    std::string clamp(TTF_Font* f, const std::string& s, int maxW) {
        int w = 0, h = 0;
        TTF_SizeUTF8(f, s.c_str(), &w, &h);
        if (w <= maxW) return s;
        std::string t = s;
        while (!t.empty()) {
            t.pop_back();
            std::string try_ = t + "...";
            TTF_SizeUTF8(f, try_.c_str(), &w, &h);
            if (w <= maxW) return try_;
        }
        return "...";
    }

    // ------------------------------------------------------------------
    void loadIcons() {
        icons.assign(apks.size(), nullptr);
        for (size_t i = 0; i < apks.size(); i++) {
            if (apks[i].iconPng.empty()) continue;
            SDL_RWops* rw = SDL_RWFromConstMem(
                apks[i].iconPng.data(), (int)apks[i].iconPng.size());
            SDL_Surface* surf = IMG_Load_RW(rw, 1);
            if (!surf) continue;
            icons[i] = SDL_CreateTextureFromSurface(rdr, surf);
            SDL_FreeSurface(surf);
            apks[i].iconPng.clear();
        }
    }

    void rescan() {
        for (auto* t : icons) if (t) SDL_DestroyTexture(t);
        icons.clear();
        apks = ::scanApks(APK_DIR);
        loadIcons();
        selected = 0; scroll = 0;
    }

    // ------------------------------------------------------------------
    void render() {
        Uint32 now = SDL_GetTicks();
        drawBackground();

        if (apks.empty()) {
            drawText(fSm,
                "No APKs found — place .apk files in sdmc:/AndroidHorizonNX/apks/",
                C_GRAY, 30, LIST_Y + 30);
        } else {
            // Focus card (borealis-style): eased position + pulsing green glow,
            // drawn under the row content.
            int targetY = LIST_Y + (selected - scroll) * ITEM_H;
            if (selAnimY < 0) selAnimY = (float)targetY;
            selAnimY += (targetY - selAnimY) * 0.35f;
            if (fabsf(selAnimY - targetY) < 0.5f) selAnimY = (float)targetY;
            {
                int cy2 = (int)selAnimY;
                float pulse = 0.5f + 0.5f * sinf(now / 1000.0f * 2.6f);
                SDL_Rect card = {12, cy2 + 4, SW - 24, ITEM_H - 8};
                fill(card.x, card.y, card.w, card.h, {41, 37, 128, 235});
                for (int g = 1; g <= 5; g++) {       // soft outer glow
                    Uint8 a = (Uint8)((60 - g * 10) * (0.55f + 0.45f * pulse));
                    SDL_SetRenderDrawColor(rdr, 52, 230, 134, a);
                    SDL_Rect gr = {card.x - g, card.y - g,
                                   card.w + 2 * g, card.h + 2 * g};
                    SDL_RenderDrawRect(rdr, &gr);
                }
                SDL_SetRenderDrawColor(rdr, 52, 230, 134,
                                       (Uint8)(160 + 95 * pulse));
                SDL_RenderDrawRect(rdr, &card);      // crisp focus border
                fill(card.x, card.y, 5, card.h, C_RIM);
            }

            int end = std::min((int)apks.size(), scroll + VISIBLE);
            for (int i = scroll; i < end; i++) {
                int iy = LIST_Y + (i - scroll) * ITEM_H;
                SDL_SetRenderDrawColor(rdr, C_DIV.r, C_DIV.g, C_DIV.b, 130);
                SDL_RenderDrawLine(rdr, 24, iy + ITEM_H - 1, SW - 24, iy + ITEM_H - 1);

                int iconY = iy + (ITEM_H - ICON_SZ) / 2;
                if (i < (int)icons.size() && icons[i]) {
                    SDL_Rect dst = {28, iconY, ICON_SZ, ICON_SZ};
                    SDL_RenderCopy(rdr, icons[i], nullptr, &dst);
                } else {
                    drawMonogram(apks[i].appName, 28, iconY, ICON_SZ);
                }

                int tx   = 28 + ICON_SZ + 16;
                int maxW = SW - tx - 40;
                drawText(fLg, clamp(fLg, apks[i].appName, maxW), C_WHITE, tx, iy + 14);

                if (apks[i].installed) {
                    static const std::string INST = "INSTALLED";
                    int bw = 0, bh = 0;
                    TTF_SizeUTF8(fSm, INST.c_str(), &bw, &bh);
                    int bx = SW - bw - 40;
                    fill(bx - 6, iy + 14, bw + 12, bh, {13, 72, 40, 200});
                    drawText(fSm, INST, C_INST, bx, iy + 14);
                }

                std::string pkgLine =
                    (apks[i].packageName.empty() ? apks[i].filename : apks[i].packageName);
                if (!apks[i].versionName.empty())
                    pkgLine += "  v" + apks[i].versionName;
                if (apks[i].fileSizeBytes > 0)
                    pkgLine += "  ·  " + formatSize(apks[i].fileSizeBytes);
                drawText(fSm, clamp(fSm, pkgLine, maxW), C_GRAY, tx, iy + 58);
            }
            if ((int)apks.size() > VISIBLE) {
                int barH = LIST_H * VISIBLE / (int)apks.size();
                int barY = LIST_Y + LIST_H * scroll / (int)apks.size();
                fill(SW - 6, barY, 6, barH, {72, 66, 170, 200});
            }
        }

        std::string cnt;
        if (!apks.empty())
            cnt = std::to_string(apks.size()) + (apks.size() == 1 ? " APK" : " APKs");
        drawHeaderBar(cnt);

        if (noticeUntil && now < noticeUntil) {
            const char* msg = noticeText.c_str();
            int w = 0, h = 0;
            TTF_SizeUTF8(fSm, msg, &w, &h);
            fill((SW - w) / 2 - 16, SH - FOOTER_H - 44, w + 32, 34, {60, 18, 14, 235});
            drawText(fSm, msg, C_WARN, (SW - w) / 2, SH - FOOTER_H - 36);
        }

        bool docked = appletGetOperationMode() == AppletOperationMode_Console;
        drawFooterBar({{BG(GLYPH_A, "A"), "Launch"}, {BG(GLYPH_X, "X"), "Reinstall"},
                       {BG(GLYPH_Y, "Y"), "Rescan"}, {BG(GLYPH_MINUS, "-"), "About"},
                       {BG(GLYPH_PLUS, "+"), "Quit"}},
                      docked ? "Docked — games need handheld (touch screen)" : "");

        if (!shotMenu && !apks.empty() && now > 3000) {  // icons + glow settled
            shotMenu = true;
            saveScreenshot("ui_menu.png");
        }
        SDL_RenderPresent(rdr);
    }

    // ------------------------------------------------------------------
    // Snapshot the last N lines from the in-memory detail ring buffer.
    // The detail buffer is written by every compatLog() call on the loader
    // thread — no file I/O, always fresh, works during silent phases too.
    // ------------------------------------------------------------------
    void snapDetailLog(std::vector<std::string>& out, int maxLines) {
        int head = g_detail_head;  // sample once
        int total = head < DETLOG_N ? head : DETLOG_N;
        int show  = total < maxLines ? total : maxLines;
        out.clear();
        out.reserve(show);
        for (int i = show - 1; i >= 0; i--) {
            int slot = ((head - 1 - i) % DETLOG_N + DETLOG_N) % DETLOG_N;
            if (g_detail_log[slot][0])
                out.push_back(std::string(g_detail_log[slot]));
        }
    }
    static const int DETLOG_N = 28;

    // ------------------------------------------------------------------
    // Progress screen — fully animated, called at ~60fps from main thread.
    // Reads shared state written by the loader thread (g_ui_stage, g_ui_pct,
    // g_ui_log, g_ui_head) plus live tails compat_log.txt for the log feed.
    // ------------------------------------------------------------------
    void showProgress() {
        Uint32 now = SDL_GetTicks();

        // Track elapsed time per stage
        static char   s_stage[80] = {};
        static Uint32 s_stage_t   = 0;
        if (strncmp(g_ui_stage, s_stage, sizeof(s_stage)) != 0) {
            memcpy(s_stage, g_ui_stage, sizeof(s_stage));
            s_stage[sizeof(s_stage) - 1] = '\0';
            s_stage_t = now;
        }
        Uint32 elapsed_s = (now - s_stage_t) / 1000;

        // ── Background ──
        drawBackground();
        // Animated spinner in header (4-frame ASCII, cycles every 150ms)
        static const char* SPINS[] = {"| Loading", "/ Loading", "- Loading", "\\ Loading"};
        drawHeaderBar(SPINS[(now / 150) % 4]);

        int y = LIST_Y + 22;

        // ── Stage label with elapsed ──
        std::string stLabel = g_ui_stage[0] ? std::string(g_ui_stage) : "Working...";
        if (elapsed_s >= 2) {
            char es[24];
            snprintf(es, sizeof(es), "  (%us)", (unsigned)elapsed_s);
            stLabel += es;
        }
        drawText(fLg, clamp(fLg, stLabel, SW - 80), C_WHITE, 40, y);
        y += 46;

        // ── Progress bar ──
        const int BAR_X = 40, BAR_W = SW - 80, BAR_H = 18;
        fill(BAR_X, y, BAR_W, BAR_H, {20, 18, 66, 255});
        int fw = g_ui_pct > 0 ? BAR_W * g_ui_pct / 100 : 0;
        if (fw > 0) fill(BAR_X, y, fw, BAR_H, {45, 205, 118, 255});
        SDL_SetRenderDrawColor(rdr, 55, 55, 110, 255);
        SDL_Rect bb = {BAR_X, y, BAR_W, BAR_H};
        SDL_RenderDrawRect(rdr, &bb);
        char pb[8]; snprintf(pb, sizeof(pb), "%d%%", g_ui_pct);
        drawText(fSm, pb, C_DIM, BAR_X + BAR_W + 10, y + 1);
        y += BAR_H + 10;

        // ── Activity scan bar — sweeps left→right every 2s regardless of progress ──
        const int SCAN_H = 8;
        fill(BAR_X, y, BAR_W, SCAN_H, {10, 9, 38, 255});
        // Bright highlight travels across the bar width in a 2s cycle
        const int GLOW_W = 120;
        int sweep = (int)((Uint64)(now % 2000) * (BAR_W + GLOW_W * 2) / 2000) - GLOW_W;
        int sx0 = std::max(BAR_X, BAR_X + sweep);
        int sx1 = std::min(BAR_X + BAR_W, BAR_X + sweep + GLOW_W);
        if (sx1 > sx0)
            fill(sx0, y, sx1 - sx0, SCAN_H, {90, 230, 160, 200});
        // Brighter leading edge
        if (sx1 > sx0) {
            int edge = std::max(sx0, sx1 - 18);
            fill(edge, y, sx1 - edge, SCAN_H, {200, 255, 225, 230});
        }
        y += SCAN_H + 16;

        // ── Terminal log box ──
        const int BOX_X  = 30;
        const int BOX_W  = SW - 60;
        const int LH     = 21;
        const int N_SHOW = 13;   // visible log lines inside the box
        const int BOX_H  = LH * (N_SHOW + 1) + 14; // +1 for title bar, +14 padding

        fill(BOX_X, y, BOX_W, BOX_H, {7, 6, 26, 230});
        // Title bar
        fill(BOX_X, y, BOX_W, LH + 4, {24, 22, 80, 240});
        drawText(fSm, "  compat_log.txt", {120, 140, 225, 255}, BOX_X + 8, y + 3);
        // Pulsing dot to show file is being read live
        Uint32 dotPhase = (now / 600) % 3;
        std::string liveDots = std::string(dotPhase > 0 ? "." : " ")
                             + std::string(dotPhase > 1 ? "." : " ")
                             + std::string(dotPhase > 2 ? "." : " ");
        drawText(fSm, "live" + liveDots, {52, 230, 134, 255}, BOX_X + BOX_W - 110, y + 3);
        y += LH + 8;

        // Log lines from in-memory detail buffer (written by every compatLog call)
        std::vector<std::string> logLines;
        snapDetailLog(logLines, N_SHOW);

        const SDL_Color C_LOG      = {125, 150, 230, 255};
        const SDL_Color C_LOG_NEW  = {225, 238, 255, 255};
        const SDL_Color C_LOG_WARN = {220, 170, 60,  255};
        const SDL_Color C_LOG_ERR  = {220, 100, 80,  255};

        int startIdx = (int)logLines.size() > N_SHOW
                       ? (int)logLines.size() - N_SHOW : 0;
        int liy = y;
        for (int i = startIdx; i < (int)logLines.size(); i++) {
            bool isLast = (i == (int)logLines.size() - 1);
            const std::string& ln = logLines[i];

            SDL_Color c = isLast ? C_LOG_NEW : C_LOG;
            // Colour-code errors/warnings (but newest line always stays bright)
            if (!isLast) {
                if (ln.find("FAULT") != std::string::npos ||
                    ln.find("fail")  != std::string::npos ||
                    ln.find("ERR")   != std::string::npos)
                    c = C_LOG_ERR;
                else if (ln.find("WARN") != std::string::npos ||
                         ln.find("warn") != std::string::npos)
                    c = C_LOG_WARN;
            }

            std::string pfx = isLast ? "> " : "  ";
            drawText(fMd ? fMd : fSm,
                     clamp(fMd ? fMd : fSm, pfx + ln, BOX_W - 24),
                     c, BOX_X + 10, liy);
            liy += LH;
        }
        y = liy + (N_SHOW - (int)(logLines.size() - startIdx)) * LH;
        y += 14; // bottom padding of box

        // ── "Still working" notice after 30s without stage change ──
        if (elapsed_s >= 30) {
            fill(30, y, SW - 60, 52, {38, 16, 10, 230});
            char warnMsg[192];
            snprintf(warnMsg, sizeof(warnMsg),
                     "Still in '%.*s' for %us — normal for large libs.",
                     48, s_stage, (unsigned)elapsed_s);
            drawText(fSm, warnMsg, C_WARN, 42, y + 6);
            drawText(fSm,
                     "If the log above stopped updating, share compat_log.txt.",
                     C_ERR, 42, y + 30);
            y += 60;
        }

        drawFooterBar({}, "Please wait — sdmc:/AndroidHorizonNX/compat_log.txt");

        if (!shotLoading && g_ui_pct >= 40) {  // mid-load with a lively log box
            shotLoading = true;
            saveScreenshot("ui_loading.png");
        }
        SDL_RenderPresent(rdr);
    }

    // ------------------------------------------------------------------
    // Run launchApk on a background thread while this method drives the
    // animated progress screen on the main thread at ~60fps.
    // ------------------------------------------------------------------
    LaunchResult runLaunch(const ApkInfo& apk, bool skipInstall) {
        std::string pkg = apk.packageName.empty() ? apk.filename : apk.packageName;

        // Detail log is in-memory — no cache to invalidate, always fresh

        // Set initial stage before thread starts so first frame looks right
        const char* verb = skipInstall ? "Launching (cached)" : "Installing + Launching";
        strncpy(g_ui_stage, verb, sizeof(g_ui_stage) - 1);

        LoaderCtx ctx;
        ctx.apk_path    = apk.path;
        ctx.pkg_name    = pkg;
        ctx.skip_install = skipInstall;
        g_loader_ctx    = &ctx;

        // The loader thread is pure CPU work (APK unzip, ELF relocation, JIT
        // dual-mapping) with nothing meaningful on screen but our own idle
        // progress UI — no GPU work competing for clocks. Real Switch titles
        // use this exact FastLoad boost mode during their own load screens
        // for the same reason. Reset back to Normal before the game (which
        // *does* need real GPU clocks) ever gets control.
        appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);

        Thread t;
        threadCreate(&t, loaderThreadFn, nullptr, nullptr,
                     0x100000 /*1MB stack*/, 0x2C, 1 /*CPU 1*/);
        threadStart(&t);

        // Main thread render loop — keeps the UI alive until the loader finishes
        bool quitting = false;
        while (!ctx.done.load(std::memory_order_acquire) && !quitting) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) quitting = true;
                if (ev.type == SDL_JOYBUTTONDOWN && ev.jbutton.button == BTN_PLUS) quitting = true;
            }
            showProgress();
            SDL_Delay(16); // ~60fps
        }

        // Render a final frame so the last log line is visible
        showProgress();

        threadWaitForExit(&t);
        threadClose(&t);
        g_loader_ctx = nullptr;

        // FastLoad throttles the GPU to its minimum clock — fine while we're
        // just drawing a static progress bar, disastrous once real gameplay
        // starts. Always drop back to Normal here, even if loading failed.
        appletSetCpuBoostMode(ApmCpuBoostMode_Normal);

        // If game loaded OK, run it here on the main thread (SDL2's EGL context
        // is current on this thread, so GL calls reach the screen).
        if (ctx.result.game_so) {
            std::string base_dir = std::string("sdmc:/AndroidHorizonNX/games/") + pkg;
            runGameOnMainThread(ctx.result.game_so, win, ctx.apk_path, base_dir);
            gameRanOnce = true;
        }

        return ctx.result;
    }

    // ------------------------------------------------------------------
    void showLaunchResult(const LaunchResult& res, int idx) {
        if (idx < 0 || idx >= (int)apks.size()) return;
        const ApkInfo& apk = apks[idx];

        bool done = false;
        while (!done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { done = true; }
                if (ev.type == SDL_JOYBUTTONDOWN &&
                    (ev.jbutton.button == BTN_B || ev.jbutton.button == BTN_PLUS)) { done = true; }
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) { done = true; }
            }

            drawBackground();
            drawHeaderBar();

            // Translucent result panel so text reads over the starfield
            fill(40, LIST_Y + 6, SW - 80, SH - LIST_Y - FOOTER_H - 12, {10, 9, 44, 215});
            {
                SDL_Rect panel = {40, LIST_Y + 6, SW - 80, SH - LIST_Y - FOOTER_H - 12};
                SDL_SetRenderDrawColor(rdr, res.ok ? 52 : 235, res.ok ? 230 : 90,
                                       res.ok ? 134 : 90, 200);
                SDL_RenderDrawRect(rdr, &panel);
            }

            int iconSz = 112;
            if (idx < (int)icons.size() && icons[idx]) {
                SDL_Rect dst = {(SW - iconSz) / 2, LIST_Y + 16, iconSz, iconSz};
                SDL_RenderCopy(rdr, icons[idx], nullptr, &dst);
            } else {
                drawMonogram(apk.appName, (SW - iconSz) / 2, LIST_Y + 16, iconSz);
            }

            int nameY = LIST_Y + 16 + iconSz + 12;
            {
                int w = 0, h = 0;
                std::string nm = clamp(fLg, apk.appName, SW - 80);
                TTF_SizeUTF8(fLg, nm.c_str(), &w, &h);
                drawText(fLg, nm, C_WHITE, (SW - w) / 2, nameY);
            }

            std::string statusStr = res.ok ? "Launch OK" : "Launch Failed";
            SDL_Color   statusCol = res.ok ? C_OK : C_ERR;
            {
                int w = 0, h = 0;
                TTF_SizeUTF8(fLg, statusStr.c_str(), &w, &h);
                drawText(fLg, statusStr, statusCol, (SW - w) / 2, nameY + 44);
            }

            int y = nameY + 100;
            if (!res.ok) {
                if (!res.errorStage.empty())
                    { drawText(fSm, "Failed at:  " + res.errorStage, C_WARN, 60, y); y += 28; }
                if (!res.errorDetail.empty())
                    { drawText(fSm, res.errorDetail, C_GRAY, 60, y); y += 28; }
            }
            if (res.unresolved > 0) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "Unresolved symbols: %d  (may crash when those code paths execute)",
                         res.unresolved);
                drawText(fSm, buf, C_WARN, 60, y); y += 28;
            }
            if (res.svcPermCode != 0) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "JIT alloc: 0x%08X — code segment not executable", res.svcPermCode);
                drawText(fSm, buf, C_ERR, 60, y); y += 28;
                drawText(fSm,
                         "jitCreate/jitTransitionToExecutable failed. Needs Atmosphere CFW.",
                         C_GRAY, 60, y); y += 28;
            }
            y += 8;
            drawText(fSm, "Full log: sdmc:/AndroidHorizonNX/compat_log.txt", C_DIM, 60, y);

            drawFooterBar({{BG(GLYPH_B, "B"), "Back to menu"}, {BG(GLYPH_PLUS, "+"), "Menu"}});

            if (!shotResult) {
                shotResult = true;
                saveScreenshot("ui_result.png");
            }
            SDL_RenderPresent(rdr);
            SDL_Delay(16);
        }
    }

    // ------------------------------------------------------------------
    void showAbout() {
        bool done = false;
        while (!done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { done = true; }
                if (ev.type == SDL_JOYBUTTONDOWN &&
                    (ev.jbutton.button == BTN_B || ev.jbutton.button == BTN_MINUS))
                    { done = true; }
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
                    { done = true; }
            }

            std::vector<uint8_t> img;
            if (avatarPollNewImage(img)) {
                SDL_RWops* rw = SDL_RWFromConstMem(img.data(), (int)img.size());
                SDL_Surface* surf = IMG_Load_RW(rw, 1);
                if (surf) {
                    if (avatarTex) SDL_DestroyTexture(avatarTex);
                    avatarTex = SDL_CreateTextureFromSurface(rdr, surf);
                    SDL_FreeSurface(surf);
                }
            }

            drawBackground();
            drawHeaderBar();

            int avSz = 160;
            int avX  = (SW - avSz) / 2;
            int avY  = LIST_Y + 30;
            if (avatarTex) {
                SDL_Rect dst = {avX, avY, avSz, avSz};
                SDL_RenderCopy(rdr, avatarTex, nullptr, &dst);
            } else {
                drawMonogram("AndroidHorizon", avX, avY, avSz);
                // Centred placeholder text below the monogram — only shown
                // for the one frame before the bundled avatar decodes
                static const std::string FETCH = "Loading avatar...";
                int fw = 0, fh = 0;
                TTF_SizeUTF8(fSm, FETCH.c_str(), &fw, &fh);
                drawText(fSm, FETCH, C_DIM, (SW - fw) / 2, avY + avSz + 8);
            }

            int y = avY + avSz + 40;
            auto center = [&](TTF_Font* f, const std::string& s, SDL_Color col) {
                int w = 0, h = 0;
                TTF_SizeUTF8(f, s.c_str(), &w, &h);
                drawText(f, s, col, (SW - w) / 2, y);
                y += h + 10;
            };
            center(fLg, "Android Horizon", C_WHITE);
            center(fSm, BUILD_VERSION, C_DIM);
            center(fSm, "by aaronworld.uk", C_GRAY);
            y += 10;
            center(fSm, "Android NDK compatibility layer for Nintendo Switch (HorizonOS)", C_GRAY);

            drawFooterBar({{BG(GLYPH_B, "B"), "Back to menu"}});

            if (!shotAbout && avatarTex) {  // wait for the avatar to arrive
                shotAbout = true;
                saveScreenshot("ui_about.png");
            }
            SDL_RenderPresent(rdr);
            SDL_Delay(16);
        }
    }
};

// ---------------------------------------------------------------------------
// Forwarder support — a small NRO (or Sphaira/hbmenu entry) can chain-load us
// with a package name as argv[1] via libnx's envSetNextLoad(path, argv), the
// same mechanism RetroArch forwarders use to boot straight into a ROM+core
// instead of RetroArch's own content browser. libnx's crt0 already parses the
// loader-provided argument block into plain argc/argv before main() runs, so
// no envGetArgv() plumbing is needed here — just read argv like any other
// command-line program.
int main(int argc, char** argv) {
    App app;

    if (!app.init()) return 1;
    avatarStart();

    mkdir(APK_DIR, 0777);

    // Scanning splash
    app.drawBackground();
    app.drawHeaderBar();
    app.drawText(app.fSm, "Scanning for APKs...", C_GRAY, 30, LIST_Y + 30);
    SDL_RenderPresent(app.rdr);

    app.apks = scanApks(APK_DIR);
    app.loadIcons();

    // Direct-launch (forwarder) mode: argv[1] names a package to boot
    // straight into, skipping the app-list UI entirely — same experience as
    // a dedicated home-menu icon for one specific game.
    if (argc > 1 && argv[1] && argv[1][0]) {
        const char* wantPkg = argv[1];
        int idx = -1;
        for (size_t i = 0; i < app.apks.size(); i++) {
            if (app.apks[i].packageName == wantPkg) { idx = (int)i; break; }
        }
        if (idx >= 0) {
            const ApkInfo& apk = app.apks[idx];
            bool skip = apk.installed;
            LaunchResult res = app.runLaunch(apk, skip);
            if (!skip) app.apks[idx].installed = true;
            // A forwarder is meant to look like a dedicated launcher for
            // this one game, not a detour through our full app list. Every
            // successful game session now exits the whole process directly
            // from inside the game loop itself (crash or deliberate quit
            // alike) — runLaunch() only returns here at all when that DIDN'T
            // happen (APK/ELF load failure, or nativeRender missing), so
            // reaching this line always means something worth explaining
            // before we close back to the Switch home menu.
            app.showLaunchResult(res, idx);
            return 0;
        }
        // Requested package not found/installed — fall back to the normal
        // picker instead of silently doing nothing, so a stale/misconfigured
        // forwarder still leaves you somewhere useful rather than a blank
        // screen.
        compatLogFmt("forwarder: requested package '%s' not found in %s — showing picker instead",
                     wantPkg, APK_DIR);
        app.noticeText  = std::string("Forwarder target \"") + wantPkg + "\" not found — showing all APKs";
        app.noticeUntil = SDL_GetTicks() + 6000;
    }

    app.render();

    bool   quit      = false;
    Uint32 lastStick = 0;

    while (!quit) {
        SDL_Event ev;

        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { quit = true; break; }

            if (ev.type == SDL_JOYBUTTONDOWN) {
                switch (ev.jbutton.button) {
                    case BTN_PLUS:
                        quit = true;
                        break;

                    case BTN_A:
                        if (app.gameRanOnce) { app.noticeUntil = SDL_GetTicks() + 4000; break; }
                        if (!app.apks.empty()) {
                            const ApkInfo& apk = app.apks[app.selected];
                            bool skip = apk.installed;
                            LaunchResult res = app.runLaunch(apk, skip);
                            if (!skip) app.apks[app.selected].installed = true;
                            app.showLaunchResult(res, app.selected);
                        }
                        break;

                    case BTN_X:
                        if (app.gameRanOnce) { app.noticeUntil = SDL_GetTicks() + 4000; break; }
                        if (!app.apks.empty()) {
                            const ApkInfo& apk = app.apks[app.selected];
                            LaunchResult res = app.runLaunch(apk, false);
                            app.apks[app.selected].installed = true;
                            app.showLaunchResult(res, app.selected);
                        }
                        break;

                    case BTN_Y:
                        app.rescan();
                        break;

                    case BTN_MINUS:
                        app.showAbout();
                        break;

                    case BTN_B:
                        break;
                }
            }

            if (ev.type == SDL_JOYHATMOTION) {
                if (ev.jhat.value & SDL_HAT_DOWN) {
                    if (!app.apks.empty() && app.selected < (int)app.apks.size() - 1) {
                        app.selected++;
                        if (app.selected >= app.scroll + VISIBLE) app.scroll++;
                    }
                }
                if (ev.jhat.value & SDL_HAT_UP) {
                    if (!app.apks.empty() && app.selected > 0) {
                        app.selected--;
                        if (app.selected < app.scroll) app.scroll--;
                    }
                }
            }

            if (ev.type == SDL_JOYAXISMOTION && ev.jaxis.axis == 1) {
                Uint32 now = SDL_GetTicks();
                if (now - lastStick > 180) {
                    if (ev.jaxis.value > 16384 && !app.apks.empty() &&
                        app.selected < (int)app.apks.size() - 1) {
                        app.selected++;
                        if (app.selected >= app.scroll + VISIBLE) app.scroll++;
                        lastStick = now;
                    } else if (ev.jaxis.value < -16384 && !app.apks.empty() &&
                               app.selected > 0) {
                        app.selected--;
                        if (app.selected < app.scroll) app.scroll--;
                        lastStick = now;
                    }
                }
            }
        }

        // Continuous ~60fps render — the starfield twinkles and the focus
        // card glow pulses even with no input.
        app.render();
        SDL_Delay(16);
    }

    app.cleanup();
    return 0;
}
