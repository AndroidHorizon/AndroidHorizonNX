#include "compat/loader.h"
#include "compat/android.h"
#include "build_number.h"
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <GLES2/gl2.h>
#include <minizip/unzip.h>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <setjmp.h>
#include <stdarg.h>

// ─── Fake Android TLS block ───────────────────────────────────────────────────
// Switch homebrews have TPIDR_EL0=0 (no .tls section in the NRO).  NDK-compiled
// code reads Bionic's thread-local state directly via `mrs xN, tpidr_el0` then
// `ldr x8, [xN, #0x28]` — no pthread shim is involved.  We install a zeroed fake
// TLS block before any game code runs so these accesses land in valid mapped memory.
alignas(16) static uint8_t g_android_tls[512];      // main TLS block
alignas(16) static uint8_t g_android_tls_sub[512];  // sub-buffer for tls[0x28]

// Crash recovery shared with elf_loader.cpp and shim_table.cpp
extern jmp_buf        g_recover_jmp;
extern volatile bool     g_in_recover;
extern volatile void*    g_recover_owner;
extern volatile int      g_recover_sig;
extern volatile uint32_t g_recover_esr;
extern volatile uint64_t g_recover_pc;
extern volatile uint64_t g_recover_far;

// Persistent path storage — launchApk stores these so ANativeActivity pointers
// remain valid after launchApk returns (for runGameOnMainThread).
static std::string g_base_dir_stored;
static std::string g_apk_path_stored;

// The running game's LoadedSo — lets jni_env.cpp invoke the game's registered
// Java_ native callbacks (async replies the Java side would normally deliver,
// e.g. returnCountryCode after fetchCountryCode).
static LoadedSo* g_game_so = nullptr;
void* compatFindGameSym(const char* name) {
    return g_game_so ? g_game_so->findSym(name) : nullptr;
}

// ─── Logging ──────────────────────────────────────────────────────────────────
static FILE*   g_compat_log   = nullptr;
static uint64_t g_log_start_t = 0;   // armGetSystemTick() at launch start

// Game threads are real now (pt_create → libnx Thread), so log calls arrive
// concurrently — serialize the dedup state and FILE* behind a mutex.
static Mutex g_log_lock;

// Dedup state: collapse consecutive identical lines into "msg x<N>"
static char g_log_last[512] = {};
static int  g_log_repeat    = 0;

// ─── Detail ring buffer (every compatLog line, unthrottled) ───────────────────
// Read by the main/render thread to display live log output without file I/O.
#define DETLOG_N  28
#define DETLOG_W  164
char g_detail_log[DETLOG_N][DETLOG_W] = {};
int  g_detail_head = 0;

static void detailPush(const char* line) {
    // Prepend "[Xs] " timestamp using ticks since launch start
    uint64_t ticks = armGetSystemTick() - g_log_start_t;
    uint32_t secs  = (uint32_t)(ticks / armGetSystemTickFreq());
    char entry[DETLOG_W];
    snprintf(entry, sizeof(entry), "[%3us] %s", secs, line);
    int i = g_detail_head % DETLOG_N;
    memcpy(g_detail_log[i], entry, DETLOG_W);
    g_detail_log[i][DETLOG_W - 1] = '\0';
    ++g_detail_head;
}

static void logFlushDedup() {
    if (g_log_repeat == 0) return;
    char linebuf[560];
    if (g_log_repeat == 1) {
        snprintf(linebuf, sizeof(linebuf), "%s", g_log_last);
    } else {
        snprintf(linebuf, sizeof(linebuf), "%s  x%d", g_log_last, g_log_repeat);
    }
    if (g_compat_log) {
        // Write timestamp + line to file too
        uint64_t ticks = armGetSystemTick() - g_log_start_t;
        uint32_t secs  = (uint32_t)(ticks / armGetSystemTickFreq());
        fprintf(g_compat_log, "[%3us] %s\n", secs, linebuf);
        fflush(g_compat_log);
    }
    detailPush(linebuf);
    g_log_repeat = 0;
}

// Lock-free emergency logger for crash-forensics call sites (fault handler,
// game-loop FAULT branch). If the thread that's now crashing faulted WHILE
// holding g_log_lock (e.g. mid-format inside a normal compatLogFmt call
// elsewhere), the ordinary path would deadlock forever — the process just
// hangs with nothing on disk and no further sign of life until force-quit.
// This bypasses the mutex entirely (accepting a small risk of interleaved
// output with a concurrent normal logger, which is fine: we're crashing).
void compatLogRaw(const char* msg) {
    if (g_compat_log) {
        uint64_t ticks = armGetSystemTick() - g_log_start_t;
        uint32_t secs  = (uint32_t)(ticks / armGetSystemTickFreq());
        fprintf(g_compat_log, "[%3us] %s\n", secs, msg);
        fflush(g_compat_log);
    }
    detailPush(msg);
}

// Force the current pending log message to disk and detail buffer immediately,
// without waiting for the next different message to trigger the dedup flush.
void compatLogFlush() {
    mutexLock(&g_log_lock);
    logFlushDedup();
    mutexUnlock(&g_log_lock);
}

void compatLog(const char* msg) {
    mutexLock(&g_log_lock);
    if (g_log_repeat > 0 && strcmp(msg, g_log_last) == 0) {
        g_log_repeat++;
        mutexUnlock(&g_log_lock);
        return;
    }
    logFlushDedup();
    snprintf(g_log_last, sizeof(g_log_last), "%s", msg);
    g_log_repeat = 1;
    mutexUnlock(&g_log_lock);
}

void compatLogFmt(const char* fmt, ...) {
    char buf[512];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    compatLog(buf);
}

static void androidTlsInstall() {
    uint64_t old_tp;
    asm volatile("mrs %0, tpidr_el0" : "=r"(old_tp));
    compatLogFmt("AndroidTLS: old TPIDR_EL0=0x%llx — installing fake TLS @%p",
                 (unsigned long long)old_tp, (void*)g_android_tls);
    *(void**)(g_android_tls + 0x00) = g_android_tls;   // TLS_SLOT_SELF
    *(void**)(g_android_tls + 0x28) = g_android_tls_sub; // slot 5: EH/locale state
    uint64_t new_tp = (uint64_t)g_android_tls;
    asm volatile("msr tpidr_el0, %0" :: "r"(new_tp) : "memory");
}

// Same fake-TLS layout, but a private heap block for a game worker thread —
// called by the pthread_create trampoline in shim_table.cpp so each real
// thread gets its own Bionic per-thread state (errno slot, EH/locale slot).
// The block intentionally leaks: game threads are few and effectively live
// for the whole session.
void androidTlsInstallThread() {
    uint8_t* blk = (uint8_t*)calloc(1, 1024);
    if (!blk) return;
    *(void**)(blk + 0x00) = blk;          // TLS_SLOT_SELF
    *(void**)(blk + 0x28) = blk + 512;    // slot 5: EH/locale state
    asm volatile("msr tpidr_el0, %0" :: "r"((uint64_t)blk) : "memory");
}

// ─── UI ring buffer ───────────────────────────────────────────────────────────
// Last UILOG_N short messages, shown as a rolling sub-step log in showProgress.
#define UILOG_N  20
#define UILOG_W  128
char g_ui_log[UILOG_N][UILOG_W] = {};
int  g_ui_head = 0;   // next write index (not wrapped)
int  g_ui_pct  = 0;   // progress bar percentage 0-100

void compatUiLog(const char* msg) {
    if (!msg) return;
    int i = g_ui_head % UILOG_N;
    strncpy(g_ui_log[i], msg, UILOG_W - 1);
    g_ui_log[i][UILOG_W - 1] = '\0';
    ++g_ui_head;
}
void compatUiSetPct(int pct) {
    g_ui_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

// ─── CompatLayer singleton ────────────────────────────────────────────────────
static CompatLayer g_compat = {};

CompatLayer* compatGet() { return &g_compat; }

// ─── Filesystem helpers ───────────────────────────────────────────────────────
static void mkdirp(const std::string& path) {
    std::string p;
    for (char c : path) {
        p += c;
        if (c == '/') mkdir(p.c_str(), 0777);
    }
    mkdir(path.c_str(), 0777);
}

// Extract a single ZIP entry to a file path
static bool extractEntry(unzFile zf, const std::string& dest) {
    unz_file_info fi;
    if (unzGetCurrentFileInfo(zf, &fi, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK)
        return false;
    if (unzOpenCurrentFile(zf) != UNZ_OK) return false;

    FILE* f = fopen(dest.c_str(), "wb");
    if (!f) { unzCloseCurrentFile(zf); return false; }

    char buf[65536];
    int n;
    while ((n = unzReadCurrentFile(zf, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, (size_t)n, f);
    fclose(f);
    unzCloseCurrentFile(zf);
    return n == 0;
}

// ─── APK extraction ───────────────────────────────────────────────────────────
static bool extractApk(const std::string& apk_path, const std::string& dest_dir,
                       ProgressCb cb) {
    unzFile zf = unzOpen(apk_path.c_str());
    if (!zf) { compatLogFmt("extract: cannot open %s", apk_path.c_str()); return false; }

    mkdirp(dest_dir + "/lib/");
    mkdirp(dest_dir + "/assets/");

    unz_global_info gi;
    if (unzGetGlobalInfo(zf, &gi) != UNZ_OK) { unzClose(zf); return false; }

    char name[1024];
    for (uLong i = 0; i < gi.number_entry; i++) {
        unz_file_info fi;
        if (unzGetCurrentFileInfo(zf, &fi, name, sizeof(name),
                                  nullptr, 0, nullptr, 0) != UNZ_OK) break;

        std::string n = name;

        if (n.rfind("lib/arm64-v8a/", 0) == 0 && n.size() > 14 &&
            n.back() != '/') {
            std::string rel  = n.substr(14);
            std::string dest = dest_dir + "/lib/" + rel;
            compatLogFmt("extract lib: %s", rel.c_str());
            if (cb) cb("Extracting APK", rel.c_str());
            extractEntry(zf, dest);

        } else if (n.rfind("assets/", 0) == 0 && n.back() != '/') {
            std::string rel  = n.substr(7);
            std::string dest = dest_dir + "/assets/" + rel;
            size_t p = 0;
            while ((p = rel.find('/', p)) != std::string::npos) {
                mkdirp(dest_dir + "/assets/" + rel.substr(0, p));
                p++;
            }
            extractEntry(zf, dest);
        }

        if (i + 1 < gi.number_entry && unzGoToNextFile(zf) != UNZ_OK) break;
    }

    unzClose(zf);
    compatLog("extract: done");
    return true;
}

// ─── Per-game asset patches ───────────────────────────────────────────────────
// Some games ship in-app reference images for specific hardware (Hill Climb
// Racing bundles MOGA Bluetooth-controller button-layout guides — see
// moga_pro_guide/moga_pocket_guide save keys). Swap them for Switch
// controller-layout equivalents bundled in romfs, so if/when real controller
// support lands, the game's own onboarding art already shows Switch buttons
// instead of a MOGA pad nobody reading this owns. Applied after every
// extraction (fresh or cached) so it's always current even if the patch
// image itself changes across builds, and resized to match whatever
// dimensions the ORIGINAL file actually has (read from the file we're about
// to replace) rather than a hardcoded guess, since the exact original
// dimensions aren't known ahead of time.
struct AssetPatch { const char* filename; const char* romfsSrc; };
static const AssetPatch kHillClimbPatches[] = {
    {"Moga_Pro_Guide.png",    "romfs:/patches/hillclimb/moga_pro_guide.png"},
    {"Moga_Pocket_Guide.png", "romfs:/patches/hillclimb/moga_pocket_guide.png"},
};

// Recursively search dir for a file matching `filename` (case-insensitive).
// Returns the full path, or "" if not found. Extracted asset trees are only
// a few directories deep, so a plain recursive walk is plenty fast.
static std::string findFileRecursive(const std::string& dir, const std::string& filename) {
    DIR* d = opendir(dir.c_str());
    if (!d) return "";
    std::string found;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        std::string nm = ent->d_name;
        if (nm == "." || nm == "..") continue;
        std::string path = dir + "/" + nm;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            found = findFileRecursive(path, filename);
            if (!found.empty()) break;
        } else if (strcasecmp(nm.c_str(), filename.c_str()) == 0) {
            found = path;
            break;
        }
    }
    closedir(d);
    return found;
}

static void applyGamePatches(const std::string& pkg_name, const std::string& asset_dir) {
    if (pkg_name != "com.fingersoft.hillclimb") return;
    romfsInit();
    for (const AssetPatch& patch : kHillClimbPatches) {
        std::string target = findFileRecursive(asset_dir, patch.filename);
        if (target.empty()) {
            compatLogFmt("patch: %s not found under assets — skipped", patch.filename);
            continue;
        }
        SDL_Surface* orig = IMG_Load(target.c_str());
        if (!orig) {
            compatLogFmt("patch: couldn't read original %s (%s) — skipped",
                         target.c_str(), IMG_GetError());
            continue;
        }
        int origW = orig->w, origH = orig->h;
        SDL_FreeSurface(orig);

        SDL_Surface* repl = IMG_Load(patch.romfsSrc);
        if (!repl) {
            compatLogFmt("patch: bundled replacement %s missing (%s) — skipped",
                         patch.romfsSrc, IMG_GetError());
            continue;
        }
        SDL_Surface* scaled = SDL_CreateRGBSurfaceWithFormat(
            0, origW, origH, 32, SDL_PIXELFORMAT_ABGR8888);
        if (scaled) {
            SDL_SetSurfaceBlendMode(repl, SDL_BLENDMODE_NONE);  // scale RGBA as-is, no alpha compositing
            SDL_BlitScaled(repl, nullptr, scaled, nullptr);
            if (IMG_SavePNG(scaled, target.c_str()) == 0)
                compatLogFmt("patch: replaced %s (%dx%d, Switch controller layout)",
                             target.c_str(), origW, origH);
            else
                compatLogFmt("patch: failed to write %s (%s)", target.c_str(), IMG_GetError());
            SDL_FreeSurface(scaled);
        }
        SDL_FreeSurface(repl);
    }
}

// ─── Find all .so files ───────────────────────────────────────────────────────
// Returns {size, path} pairs sorted smallest-first so dependency libs load
// before the main game lib (which is typically the largest).
static std::vector<std::pair<size_t, std::string>> findAllSos(const std::string& lib_dir) {
    DIR* d = opendir(lib_dir.c_str());
    if (!d) return {};

    std::vector<std::pair<size_t, std::string>> sos;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        std::string nm = ent->d_name;
        if (nm.size() < 4 || nm.compare(nm.size()-3, 3, ".so") != 0) continue;
        std::string path = lib_dir + "/" + nm;
        struct stat st;
        if (stat(path.c_str(), &st) == 0)
            sos.push_back({(size_t)st.st_size, path});
    }
    closedir(d);

    // Smallest first: dependency/helper libs before the main game lib
    std::sort(sos.begin(), sos.end(),
              [](const auto& a, const auto& b){ return a.first < b.first; });

    return sos;
}

// ─── EGL / window setup ───────────────────────────────────────────────────────
static EGLDisplay g_egl_display = EGL_NO_DISPLAY;
static EGLContext g_egl_context  = EGL_NO_CONTEXT;
static EGLSurface g_egl_surface  = EGL_NO_SURFACE;

static bool setupEGL(ANativeWindow* win) {
    g_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_egl_display == EGL_NO_DISPLAY) {
        compatLog("EGL: no display");
        return false;
    }
    EGLint major, minor;
    if (!eglInitialize(g_egl_display, &major, &minor)) {
        compatLog("EGL: init failed");
        return false;
    }
    compatLogFmt("EGL: version %d.%d", major, minor);

    eglBindAPI(EGL_OPENGL_ES_API);

    static const EGLint cfg_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE,   8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint ncfg = 0;
    if (!eglChooseConfig(g_egl_display, cfg_attribs, &cfg, 1, &ncfg) || ncfg == 0) {
        compatLog("EGL: no matching config");
        return false;
    }

    static const EGLint ctx_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    g_egl_context = eglCreateContext(g_egl_display, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (g_egl_context == EGL_NO_CONTEXT) {
        compatLog("EGL: create context failed");
        return false;
    }

    g_egl_surface = eglCreateWindowSurface(g_egl_display, cfg,
                                           (EGLNativeWindowType)win->nwin, nullptr);
    if (g_egl_surface == EGL_NO_SURFACE) {
        compatLog("EGL: create window surface failed");
        return false;
    }

    if (!eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context)) {
        compatLog("EGL: makeCurrent failed");
        return false;
    }

    compatLog("EGL: setup OK");
    return true;
}

// ─── apkInstall ──────────────────────────────────────────────────────────────
bool apkInstall(const std::string& apk_path, const std::string& pkg_name, ProgressCb cb) {
    std::string base_dir  = std::string("sdmc:/AndroidHorizonNX/games/") + pkg_name;
    mkdirp(base_dir);
    mkdirp(base_dir + "/lib/");
    mkdirp(base_dir + "/assets/");

    if (cb) cb("Installing APK", "Extracting libs and assets...");
    compatLogFmt("apkInstall: %s -> %s", apk_path.c_str(), pkg_name.c_str());
    if (!extractApk(apk_path, base_dir, cb)) {
        compatLog("apkInstall: extraction failed");
        return false;
    }
    // Write .installed marker so subsequent launches can skip extraction
    std::string marker = base_dir + "/.installed";
    FILE* mf = fopen(marker.c_str(), "w");
    if (mf) { fputs(apk_path.c_str(), mf); fclose(mf); }
    compatLog("apkInstall: done — marker written");
    return true;
}

// ─── launchApk ───────────────────────────────────────────────────────────────
LaunchResult launchApk(const std::string& apk_path, const std::string& pkg_name,
                       ProgressCb cb, bool already_installed) {
    LaunchResult result;

    std::string log_path = "sdmc:/AndroidHorizonNX/compat_log.txt";
    g_compat_log = fopen(log_path.c_str(), "w");
    g_log_start_t = armGetSystemTick();
    compatLogFmt("launchApk: %s  pkg=%s  installed=%d",
                 apk_path.c_str(), pkg_name.c_str(), (int)already_installed);

    // ── 1. Set up directories (always, mkdirp is idempotent) ─────────────────
    std::string base_dir  = std::string("sdmc:/AndroidHorizonNX/games/") + pkg_name;
    std::string lib_dir   = base_dir + "/lib";
    std::string asset_dir = base_dir + "/assets";
    mkdirp(base_dir);
    mkdirp(lib_dir);
    mkdirp(asset_dir);

    // ── 2. Extract APK (skipped when already installed) ──────────────────────
    if (!already_installed) {
        compatUiLog("Extracting APK...");
        compatUiSetPct(2);
        if (cb) cb("Installing APK", "Extracting libs and assets from APK...");
        compatLog("Extracting APK...");
        if (!extractApk(apk_path, base_dir, cb)) {
            compatLog("Extraction failed");
            result.errorStage  = "Extracting APK";
            result.errorDetail = "Could not open or read the APK file.";
            if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
            return result;
        }
        // Write .installed marker
        std::string marker = base_dir + "/.installed";
        FILE* mf = fopen(marker.c_str(), "w");
        if (mf) { fputs(apk_path.c_str(), mf); fclose(mf); }
        compatLog("APK installed — marker written");
    } else {
        compatUiLog("Using cached install (skip extract)");
        compatUiSetPct(12);
        if (cb) cb("Loading cached install", "Skipping APK extraction...");
        compatLog("Already installed — skipping extraction");
    }

    // Applied every launch (fresh or cached) so it's always current.
    applyGamePatches(pkg_name, asset_dir);

    // ── 3. Find all .so files ────────────────────────────────────────────────
    compatUiLog("Scanning for .so files...");
    compatUiSetPct(12);
    if (cb) cb("Finding libraries", "Scanning extracted libs...");
    auto all_sos = findAllSos(lib_dir);
    if (all_sos.empty()) {
        compatLog("No arm64 .so found in APK");
        result.errorStage  = "Finding libraries";
        result.errorDetail = "No arm64-v8a .so found — APK may not support this architecture.";
        if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
        return result;
    }
    for (auto& [sz, path] : all_sos) {
        size_t sl = path.rfind('/');
        const char* nm = (sl != std::string::npos) ? path.c_str() + sl + 1 : path.c_str();
        compatLogFmt("findAllSos: %s (%zu bytes)", nm, sz);
    }
    // Main SO is largest (last in smallest-first vector)
    const std::string& main_so = all_sos.back().second;

    // ── 4. Set up JNI ───────────────────────────────────────────────────────
    compatUiLog("Setting up JNI stubs...");
    compatUiSetPct(15);
    if (cb) cb("Setting up JNI", "Preparing Android runtime environment...");
    compatLog("Setting up JNI environment...");
    jniSetup(&g_compat);

    // ── 5. Load all ELF libraries (smallest-first = deps before main game) ──
    // All SOs are loaded BEFORE any constructors run, so cross-library symbols
    // are available during constructor calls.
    elfResetCounts();
    std::vector<LoadedSo*> loaded;
    LoadedSo* so = nullptr;  // the main (largest) SO
    int so_idx = 0;
    int so_total = (int)all_sos.size();
    for (auto& [sz, so_path] : all_sos) {
        size_t sl = so_path.rfind('/');
        const char* soName = (sl != std::string::npos)
                             ? so_path.c_str() + sl + 1 : so_path.c_str();
        {
            char ub[80];
            snprintf(ub, sizeof(ub), "Loading %s...", soName);
            compatUiLog(ub);
        }
        int pct = 18 + (so_total > 0 ? 40 * so_idx / so_total : 0);
        compatUiSetPct(pct);
        if (cb) cb("Loading ELF library", soName);
        compatLogFmt("Loading: %s", soName);
        LoadedSo* loaded_so = elfLoad(so_path.c_str(), cb);
        // Advance the progress bar now that elfLoad returned, so the screen
        // moves past the last RELA/JMPREL update and shows clear completion.
        {
            int pct2 = 18 + (so_total > 0 ? 40 * (so_idx + 1) / so_total : 40);
            compatUiSetPct(pct2 < 58 ? pct2 : 57);
        }
        if (loaded_so) {
            loaded.push_back(loaded_so);
            if (so_path == main_so) so = loaded_so;
            char ub[80];
            snprintf(ub, sizeof(ub), "Loaded %s OK", soName);
            compatUiLog(ub);
            if (cb) cb("ELF loaded", soName);
        } else {
            compatLogFmt("WARN: failed to load %s — skipping", soName);
            char ub[80];
            snprintf(ub, sizeof(ub), "WARN: %s failed to load", soName);
            compatUiLog(ub);
        }
        ++so_idx;
    }

    result.unresolved  = elfGetUnresolvedCount();
    result.svcPermCode = elfGetLastSvcPermCode();

    if (!so) {
        compatLog("Main .so failed to load");
        result.errorStage  = "Loading ELF library";
        result.errorDetail = "ELF loader rejected the main .so — may not be valid ARM64.";
        if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
        return result;
    }
    if (result.unresolved > 0) {
        compatLogFmt("ELF: %d total unresolved symbols across all libs", result.unresolved);
    }

    // ── 5b. Verify code pages are executable ────────────────────────────────
    if (cb) cb("Checking code permissions", "Verifying JIT code mapping...");
    if (result.svcPermCode != 0) {
        compatLogFmt("Aborting launch — JIT code mapping failed (0x%08X). "
                     "Calling into the game would cause a Switch fatal error.",
                     result.svcPermCode);
        result.errorStage  = "Setting code pages executable";
        result.errorDetail = "JIT allocation failed — code segment is not executable. "
                             "Check that Atmosphere CFW is active and has JIT permission.";
        if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
        return result;
    }

    // ── 5c. Run constructors for all SOs (dependency order, smallest first) ──
    compatUiSetPct(58);
    // Install fake Bionic TLS block BEFORE any game code runs.
    // libgame.so reads directly from TPIDR_EL0 via `mrs` — our shim table cannot
    // intercept this.  Without this, every site that touches Bionic's thread-local
    // state (EH globals, locale, etc.) faults at virtual address 0x28.
    androidTlsInstall();
    {
        int total_ctors = 0;
        for (LoadedSo* lso : loaded) total_ctors += (int)lso->init_arr_count;
        compatLogFmt("CTORS: starting %d constructors — this may take 30-120s, please wait", total_ctors);
        compatLogFlush();
        char cb_msg[96];
        snprintf(cb_msg, sizeof(cb_msg), "%d constructors — may take 30-120s, please wait...", total_ctors);
        compatUiLog(cb_msg);
        if (cb) cb("Running constructors", cb_msg);
    }
    for (LoadedSo* lso : loaded) {
        elfRunCtors(lso, cb);
    }

    // ── 6. Set up ANativeWindow (no EGL here — main thread does that) ────────
    ANativeWindow* nwin = &g_compat.window;
    nwin->width  = 1280;
    nwin->height = 720;
    nwin->format = 1; // RGBA_8888
    nwin->nwin   = nwindowGetDefault();

    // ── 7. Set up ANativeActivity ────────────────────────────────────────────
    // Store paths durably so the pointers remain valid after this function returns.
    g_base_dir_stored  = base_dir;
    g_apk_path_stored  = apk_path;

    ANativeActivity* act = &g_compat.activity;
    memset(&g_compat.callbacks, 0, sizeof(g_compat.callbacks));
    act->callbacks        = &g_compat.callbacks;
    act->vm               = (JavaVM*)g_compat.vm_outer;
    act->env              = (JNIEnv*)g_compat.env_outer;
    act->clazz            = (void*)0x4001;
    act->internalDataPath = g_base_dir_stored.c_str();
    act->externalDataPath = g_base_dir_stored.c_str();
    act->sdkVersion       = 26;
    act->instance         = nullptr;
    act->window           = nwin;

    strncpy(g_compat.asset_mgr.base_path, asset_dir.c_str(),
            sizeof(g_compat.asset_mgr.base_path) - 1);
    act->assetManager = &g_compat.asset_mgr;

    // ── 8. Scan libgame for Java_ native methods ─────────────────────────────
    compatLog("Scanning for Java_ native methods in main .so...");
    if (so->symtab && so->strtab) {
        int jcount = 0;
        for (uint32_t i = 1; i < so->sym_count; i++) {
            const Elf64_Sym& s = so->symtab[i];
            if (s.st_shndx == SHN_UNDEF || s.st_value == 0) continue;
            if (so->strsz > 0 && (uint64_t)s.st_name >= so->strsz) continue;
            if (so->alloc_size > 0 && s.st_value >= so->alloc_size) continue;
            const char* sname = so->strtab + s.st_name;
            if (strncmp(sname, "Java_", 5) == 0) {
                compatLogFmt("JAVA_METHOD: %s @%p", sname,
                             (void*)((uint8_t*)so->base + s.st_value));
                ++jcount;
            }
        }
        compatLogFmt("JAVA_METHOD: %d total Java_ symbols found", jcount);
    }

    // ── 9+. Game execution runs on the MAIN thread (has SDL2 EGL context). ──
    // Return here — main.cpp's runLaunch will call runGameOnMainThread.
    compatUiLog("ELF loaded — handing off to main thread");
    compatUiSetPct(95);
    if (cb) cb("ELF loaded", "Starting game on main thread...");
    compatLog("ELF: loading complete — game execution on main thread");
    compatLogFlush();

    result.ok      = true;
    result.game_so = (void*)so;
    // Note: g_compat_log stays open; runGameOnMainThread will close it.
    return result;
}

// ─── Branding overlay ─────────────────────────────────────────────────────────
// Draws "Android Horizon vX.Y.Z" over the game's own loading screen — a small
// GLES2 textured quad composited directly into the game's frame, right after
// nativeRender() returns and before the buffer swap. We can't reuse the
// game's own bitmap font without reverse-engineering its asset pipeline, so
// this renders our own app font once into a texture instead (matching the
// launcher's colour scheme: white "Android" + icon-green "Horizon").
// Hidden automatically once the game calls splashScreenHasCompleted (see
// compatMarkSplashDone below) so it never overlaps actual gameplay.
static volatile bool g_splash_active = true;
void compatMarkSplashDone() { g_splash_active = false; }

// See loader.h doc comment. Consumed once per frame in the game loop, which
// sends a synthetic BACK key each frame while this is > 0.
static volatile int g_force_back_frames = 0;
void compatBlockShopEntry() {
    g_force_back_frames = 90;  // ~1.5s at 60fps — several chances to register
    compatLog("shop-guard: trackPage looked like Shop — forcing BACK for ~1.5s");
}

namespace {
struct BrandOverlay {
    bool     ready   = false;
    bool     failed  = false;
    GLuint   prog    = 0;
    GLuint   tex     = 0;
    GLuint   vbo     = 0;
    GLint    aPos = -1, aUV = -1, uRect = -1, uScreen = -1, uTex = -1;
    int      texW = 0, texH = 0;
};
static BrandOverlay g_brand;

static GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        compatLogFmt("branding: shader compile FAIL: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static void initBrandOverlay() {
    g_brand.failed = true;  // reset to false only on full success below

    // Render the text once into an SDL surface using the same fonts the
    // launcher UI uses (system BFTTF, falling back to the bundled romfs font).
    PlFontData fd = {};
    TTF_Font* font = nullptr;
    if (plGetSharedFontByType(&fd, PlSharedFontType_Standard) == 0 && fd.size > 8) {
        SDL_RWops* rw = SDL_RWFromConstMem((const uint8_t*)fd.address + 8, (int)fd.size - 8);
        font = TTF_OpenFontRW(rw, 1, 26);
    }
    if (!font) {
        romfsInit();
        font = TTF_OpenFont("romfs:/fonts/DejaVuSans.ttf", 26);
    }
    if (!font) { compatLog("branding: font load FAIL — overlay disabled"); return; }
    // HCR's own version text is a bold condensed display font — our system
    // font isn't that shape, but bold at least matches its weight better
    // than a thin regular face sitting right next to it.
    TTF_SetFontStyle(font, TTF_STYLE_BOLD);

    const SDL_Color white = {255, 255, 255, 255};
    const SDL_Color green = {52,  230, 134, 255};
    const SDL_Color dim   = {180, 182, 205, 255};

    SDL_Surface* a = TTF_RenderUTF8_Blended(font, "Android ", white);
    SDL_Surface* b = TTF_RenderUTF8_Blended(font, "Horizon", green);
    std::string verStr = std::string(" ") + BUILD_VERSION;
    SDL_Surface* c = TTF_RenderUTF8_Blended(font, verStr.c_str(), dim);
    TTF_CloseFont(font);
    if (!a || !b || !c) { compatLog("branding: text render FAIL — overlay disabled"); return; }

    SDL_Surface* parts[3] = {a, b, c};
    int w = a->w + b->w + c->w, h = a->h;
    for (int i = 0; i < 3; i++) if (parts[i]->h > h) h = parts[i]->h;
    SDL_Surface* combo = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!combo) {
        compatLog("branding: combo surface FAIL");
        for (int i = 0; i < 3; i++) SDL_FreeSurface(parts[i]);
        return;
    }
    SDL_FillRect(combo, nullptr, SDL_MapRGBA(combo->format, 0, 0, 0, 0));
    int x = 0;
    for (int i = 0; i < 3; i++) {
        SDL_Surface* s = parts[i];
        SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_BLEND);
        SDL_Rect dst = {x, 0, s->w, s->h};
        SDL_BlitSurface(s, nullptr, combo, &dst);
        x += s->w;
        SDL_FreeSurface(s);
    }

    glGenTextures(1, &g_brand.tex);
    glBindTexture(GL_TEXTURE_2D, g_brand.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, combo->w, combo->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, combo->pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_brand.texW = combo->w; g_brand.texH = combo->h;
    SDL_FreeSurface(combo);

    static const char* kVS =
        "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV;\n"
        "uniform vec4 uRect; uniform vec2 uScreen;\n"
        "void main() {\n"
        "  vec2 pix = uRect.xy + aPos * uRect.zw;\n"
        "  vec2 ndc = vec2(pix.x / uScreen.x * 2.0 - 1.0, 1.0 - pix.y / uScreen.y * 2.0);\n"
        "  gl_Position = vec4(ndc, 0.0, 1.0); vUV = aUV;\n"
        "}\n";
    static const char* kFS =
        "precision mediump float; varying vec2 vUV; uniform sampler2D uTex;\n"
        "void main() { gl_FragColor = texture2D(uTex, vUV); }\n";

    GLuint vs = compileShader(GL_VERTEX_SHADER, kVS);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFS);
    if (!vs || !fs) return;
    g_brand.prog = glCreateProgram();
    glAttachShader(g_brand.prog, vs);
    glAttachShader(g_brand.prog, fs);
    // Deliberately high, unusual attribute indices — cocos2d-x's own shaders
    // use low indices (0-2ish) and its GL state cache assumes nothing else
    // touches those slots. Using indices it never looks at means our draw
    // can't desync its cache no matter what we leave enabled/disabled.
    glBindAttribLocation(g_brand.prog, 8, "aPos");
    glBindAttribLocation(g_brand.prog, 9, "aUV");
    glLinkProgram(g_brand.prog);
    GLint linked = 0;
    glGetProgramiv(g_brand.prog, GL_LINK_STATUS, &linked);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!linked) { compatLog("branding: shader link FAIL — overlay disabled"); return; }

    g_brand.aPos    = 8;
    g_brand.aUV     = 9;
    g_brand.uRect   = glGetUniformLocation(g_brand.prog, "uRect");
    g_brand.uScreen = glGetUniformLocation(g_brand.prog, "uScreen");
    g_brand.uTex    = glGetUniformLocation(g_brand.prog, "uTex");

    // Unit quad: pos(x,y) + uv(x,y) per vertex, triangle strip
    const float verts[] = {
        0,0, 0,0,
        1,0, 1,0,
        0,1, 0,1,
        1,1, 1,1,
    };
    glGenBuffers(1, &g_brand.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_brand.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    g_brand.failed = false;
    g_brand.ready  = true;
    compatLogFmt("branding: overlay ready (%dx%d texture)", g_brand.texW, g_brand.texH);
}

// ─── Loading-screen detection by pixel fingerprint ───────────────────────────
// splashScreenHasCompleted only tells us the ENTIRE splash+loading sequence
// is over — it doesn't distinguish the earlier Fingersoft logo animation
// (plain black background) from the later "HILL CLIMB RACING / LOADING..."
// screen with the version text, so the overlay was appearing during the logo
// too ("showed too early"). Instead, sample a few fixed screen points each
// frame and only show the overlay when their colours match this specific
// screen's look (estimated from a real handheld screenshot — corners of its
// dark vignette background + the white loading bar). Coordinates are in
// image space (0,0 = top-left); glReadPixels needs window space (0,0 =
// bottom-left), so the Y is flipped at sample time.
struct ProbePoint { int x, y; uint8_t r, g, b; };
// Recalibrated from a REAL captured log (build 77 test): the two top corners
// read (48,54,66) and (45,49,60) — a blue-gray, not the near-black originally
// guessed. The loading bar point turned out unstable (255,255,255 early,
// dropping to 67,73,76 once the fill animation passes it), so it's dropped
// in favour of a third corner — same static vignette tone, always stable.
// Top corners confirmed matching in a real log (48,54,66)/(45,49,60) vs
// (46,51,63) — no recalibration needed there. The bottom-left corner reads
// genuinely darker (16,19,27), consistently across two samples — the
// vignette isn't uniform, it darkens further toward that corner.
static const ProbePoint kLoadingProbes[3] = {
    {10,   10,  46, 51, 63},   // dark vignette corner, top-left
    {1270, 10,  46, 51, 63},   // dark vignette corner, top-right
    {10,  710,  16, 19, 27},   // vignette corner, bottom-left (darker falloff)
};
static const int kProbeTolerance = 25;

// Logs actual vs. expected every 300 frames (~5s at 60fps, not every frame —
// this now runs for the whole session, not just briefly during loading) so a wrong
// guess is cheap to recalibrate from the next compat_log.txt — this data is
// exactly what's needed to correct kLoadingProbes without more guesswork.
// Once we've seen the loading screen at least once and then lost the match
// for a sustained stretch (not just a single flickered frame), permanently
// latch the overlay off for the rest of the session — via g_brand.failed,
// which drawBrandOverlay() already checks first thing. This is what makes
// sure it's gone for good once the game reaches vehicle-select/upgrades/menu
// (a completely different, bright background that won't match these dark
// corners anyway) rather than depending solely on a live per-frame match
// that could theoretically flicker back on if some later screen briefly
// shares similar dark corners (e.g. a transition/loading flash mid-game).
// Also saves the 3 glReadPixels/frame for the rest of the session once latched.
static bool  s_everMatchedLoadingScreen = false;
static int   s_mismatchStreak           = 0;
static const int kMismatchLatchFrames   = 90;  // ~1.5s at 60fps

static bool isOnLoadingScreen(int frame) {
    if (g_brand.failed) return false;  // already latched off — skip the probe entirely

    uint8_t px[4];
    bool match = true;
    char detail[256]; detail[0] = '\0';
    for (int i = 0; i < 3; i++) {
        const ProbePoint& p = kLoadingProbes[i];
        glReadPixels(p.x, 720 - 1 - p.y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        bool ok = std::abs((int)px[0] - (int)p.r) <= kProbeTolerance &&
                  std::abs((int)px[1] - (int)p.g) <= kProbeTolerance &&
                  std::abs((int)px[2] - (int)p.b) <= kProbeTolerance;
        if (!ok) match = false;
        char part[80];
        snprintf(part, sizeof(part), " [%d]=%d,%d,%d(want %d,%d,%d)%s",
                 i, px[0], px[1], px[2], p.r, p.g, p.b, ok ? "" : "X");
        strncat(detail, part, sizeof(detail) - strlen(detail) - 1);
    }
    if (frame % 300 == 0)
        compatLogFmt("branding: probe%s → %s", detail, match ? "MATCH" : "no match");

    if (match) {
        s_everMatchedLoadingScreen = true;
        s_mismatchStreak = 0;
    } else if (s_everMatchedLoadingScreen) {
        if (++s_mismatchStreak > kMismatchLatchFrames) {
            g_brand.failed = true;  // permanently done for this session
            compatLogFmt("branding: past loading screen (frame %d) — overlay off for rest of session", frame);
        }
    }
    return match;
}

// Draw the overlay quad over the game's already-rendered frame. Screen-space
// position: bottom-left, stacked just above where HCR draws its own
// "1.67.0 (166)" version text, so both are visible without overlapping.
// cocos2d-x keeps its OWN software cache of GL state (current program, bound
// buffer/texture, enabled attribs...) and skips redundant driver calls when it
// thinks nothing changed. Our first cut here changed real GL state behind
// its back — its cache went stale, and its very next draw call fed the GPU
// wrong attribute/program/texture state, which is exactly why the screen
// went solid black after the overlay appeared for one frame. Fix: save every
// piece of global GL state we touch and restore it bit-for-bit before
// returning, so cocos2d's next draw sees an identical GL machine to the one
// it left behind and its cache stays valid. Vertex attributes use indices 8/9
// (cocos2d only uses low indices), so we don't even need to save/restore
// per-attribute pointer state — just enable/disable is enough there.
static void drawBrandOverlay() {
    if (g_brand.failed) return;
    if (!g_brand.ready) initBrandOverlay();
    if (!g_brand.ready) return;

    // Position estimated from a real handheld-mode screenshot: HCR draws its
    // own "1.67.0 (166)" bottom-left, ~20px in from the left edge and ~15px
    // up from the bottom, roughly 30px tall. We sit right next to it on the
    // same line rather than stacking above — matches "next to the version
    // text" from the original ask. Nudge X_GAP/Y_BOTTOM if it's off in
    // practice; there's no way to pixel-measure this without the file itself.
    const float SCALE      = 0.9f;
    const float X_GAP      = 190.0f;  // estimated right edge of "1.67.0 (166)" + padding
    const float Y_BOTTOM   = 15.0f;   // estimated bottom margin, matching the game's own
    float w = g_brand.texW * SCALE, h = g_brand.texH * SCALE;
    float x = X_GAP;
    float y = 720.0f - h - Y_BOTTOM;

    GLint  prevProgram = 0, prevArrayBuf = 0, prevTex = 0, prevActiveTex = 0;
    GLint  prevBlendSrcRGB = 0, prevBlendDstRGB = 0, prevBlendSrcA = 0, prevBlendDstA = 0;
    GLboolean prevBlend = glIsEnabled(GL_BLEND);
    GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevCull  = glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevBlendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevBlendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlendSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlendDstA);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(g_brand.prog);
    glUniform4f(g_brand.uRect, x, y, w, h);
    glUniform2f(g_brand.uScreen, 1280.0f, 720.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_brand.tex);
    glUniform1i(g_brand.uTex, 0);

    glBindBuffer(GL_ARRAY_BUFFER, g_brand.vbo);
    glEnableVertexAttribArray(g_brand.aPos);
    glEnableVertexAttribArray(g_brand.aUV);
    glVertexAttribPointer(g_brand.aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glVertexAttribPointer(g_brand.aUV,  2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(g_brand.aPos);
    glDisableVertexAttribArray(g_brand.aUV);

    // Restore everything, exactly, before handing the frame back to cocos2d.
    glUseProgram(prevProgram);
    glBindBuffer(GL_ARRAY_BUFFER, prevArrayBuf);
    glActiveTexture(prevActiveTex);
    glBindTexture(GL_TEXTURE_2D, prevTex);
    glBlendFuncSeparate(prevBlendSrcRGB, prevBlendDstRGB, prevBlendSrcA, prevBlendDstA);
    if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (prevDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prevCull)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);
}

// The game draws directly via raw EGL/GLES2 on the same window/context the
// launcher's SDL_Renderer uses. When the game loop ends — whether by a clean
// quit or (more importantly) a caught mid-frame crash — it can leave GL in an
// arbitrary state: a bound shader/texture/buffer, or scissor/stencil test
// left enabled from whatever draw call was interrupted. SDL's renderer
// doesn't necessarily reset every one of these before its own draws, so a
// crash could leave the launcher showing a corrupted frame (a stray white
// quad, a clipped scissor rect, etc.) — visually "a white box then nothing,
// have to close with Home" — even though our side of the game loop exited
// and logged cleanly. Reset everything to sane defaults before handing the
// window back to the launcher.
static void resetGLStateForLauncher(int screenW, int screenH) {
    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int i = 0; i < 16; i++) glDisableVertexAttribArray((GLuint)i);
    for (int unit = 0; unit < 4; unit++) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glViewport(0, 0, screenW, screenH);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
} // namespace

// ─── runGameOnMainThread ─────────────────────────────────────────────────────
// Called from the MAIN thread (SDL2's EGL context is current on this thread).
// Captures SDL2's EGL context, runs JNI_OnLoad, nativeSetPaths, nativeInit,
// then loops on nativeRender + eglSwapBuffers until the game faults or exits.
// sdl_win is SDL_Window* used for buffer swap via SDL_GL_SwapWindow.
void runGameOnMainThread(void* game_so_ptr,
                         void* sdl_win,
                         const std::string& apk_path,
                         const std::string& data_path) {
    LoadedSo* so = (LoadedSo*)game_so_ptr;
    SDL_Window* win = (SDL_Window*)sdl_win;
    g_game_so = so;   // for compatFindGameSym (JNI → native callbacks)

    // Re-install fake Android TLS on THIS thread.  TPIDR_EL0 is per-thread;
    // launchApk set it on its thread (the background worker) for ctors, but
    // runGameOnMainThread runs on the main thread where TPIDR_EL0 is still 0.
    androidTlsInstall();

    // SimpleAudioEngine paths are asset-relative — point audio.cpp at the
    // extracted APK assets, and bring the mixer up now, outside the game
    // loop's recovery window (lazy init mid-frame is a fault suspect).
    compatAudioSetAssetsDir((data_path + "/assets").c_str());
    compatAudioWarmup();

    // Restore the game's saved state (UserDefaults) from the previous session
    jniUserDefaultsLoad((data_path + "/userdefaults.bin").c_str());

    // Capture SDL2's active EGL context (current on this main thread).
    g_egl_display = eglGetCurrentDisplay();
    g_egl_surface = eglGetCurrentSurface(EGL_DRAW);
    g_egl_context = eglGetCurrentContext();
    if (g_egl_display != EGL_NO_DISPLAY && g_egl_surface != EGL_NO_SURFACE &&
        g_egl_context != EGL_NO_CONTEXT) {
        compatLog("EGL: using SDL2 context on main thread");
    } else {
        compatLog("EGL: SDL2 context not current — GL calls may fail");
    }
    compatLogFlush();

    JNIEnv* env = (JNIEnv*)g_compat.env_outer;
    jobject obj = (jobject)(uintptr_t)0x4001;

    // ── JNI_OnLoad ────────────────────────────────────────────────────────────
    typedef int32_t (*JNI_OnLoad_fn)(JavaVM**, void*);
    JNI_OnLoad_fn jni_onload = (JNI_OnLoad_fn)so->findSym("JNI_OnLoad");
    if (jni_onload) {
        compatLogFmt("Calling JNI_OnLoad @%p ...", (void*)jni_onload);
        g_recover_owner = threadGetSelf(); g_in_recover = true; g_recover_sig = 0; g_recover_esr = 0; g_recover_far = 0;
        if (setjmp(g_recover_jmp) == 0) {
            int32_t ver = jni_onload((JavaVM**)g_compat.vm_outer, nullptr);
            g_in_recover = false;
            compatLogFmt("JNI_OnLoad returned: 0x%X", ver);
        } else {
            g_in_recover = false;
            char sym_buf[160];
            elfNearestSym(so, g_recover_pc - (uint64_t)so->base, sym_buf, sizeof(sym_buf));
            compatLogFmt("JNI_OnLoad FAULT sig=%d esr=0x%08x pc=%p far=%p sym=%s — skipped",
                         g_recover_sig, g_recover_esr,
                         (void*)g_recover_pc, (void*)g_recover_far, sym_buf);
            const uint32_t* insn = (const uint32_t*)(uintptr_t)g_recover_pc;
            compatLogFmt("INSN: [pc-12]=%08x [pc-8]=%08x [pc-4]=%08x [pc]=%08x [pc+4]=%08x",
                         insn[-3], insn[-2], insn[-1], insn[0], insn[1]);
        }
        compatLogFlush();
    }

    // ── ANativeActivity_onCreate (NativeActivity path) ────────────────────────
    typedef void (*OnCreate_fn)(ANativeActivity*, void*, size_t);
    OnCreate_fn on_create = (OnCreate_fn)so->findSym("ANativeActivity_onCreate");
    if (on_create) {
        ANativeActivity* act = &g_compat.activity;
        compatLogFmt("ANativeActivity_onCreate @%p", (void*)on_create);
        g_recover_owner = threadGetSelf(); g_in_recover = true; g_recover_sig = 0; g_recover_esr = 0; g_recover_far = 0;
        if (setjmp(g_recover_jmp) == 0) {
            on_create(act, nullptr, 0);
            g_in_recover = false;
            compatLog("onCreate returned");
        } else {
            g_in_recover = false;
            compatLogFmt("onCreate FAULT sig=%d esr=0x%08x pc=%p far=%p — skipped",
                         g_recover_sig, g_recover_esr,
                         (void*)g_recover_pc, (void*)g_recover_far);
        }
        compatLogFlush();

        // Deliver window if callback registered
        ANativeActivityCallbacks* cbs = &g_compat.callbacks;
        if (cbs->onStart)  { cbs->onStart(act); }
        if (cbs->onResume) { cbs->onResume(act); }
        if (cbs->onNativeWindowCreated) {
            compatLog("onNativeWindowCreated");
            cbs->onNativeWindowCreated(act, &g_compat.window);
        }
        compatLog("Entering game loop (NativeActivity)");
        compatLogFlush();
        // NativeActivity drives its own render loop via callbacks; we wait.
        if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
        return;
    }

    // ── Cocos2d-x direct Java_ path ───────────────────────────────────────────
    compatLog("No NativeActivity — Cocos2d-x direct Java_ path");
    compatLogFlush();

    typedef void (*SetPaths_fn)(JNIEnv*, jobject, jstring, jstring);
    SetPaths_fn setPaths = (SetPaths_fn)so->findSym(
        "Java_org_cocos2dx_lib_Cocos2dxActivity_nativeSetPaths");
    if (setPaths) {
        compatLogFmt("Cocos2d-x: nativeSetPaths @%p", (void*)setPaths);
        g_recover_owner = threadGetSelf(); g_in_recover = true; g_recover_sig = 0; g_recover_esr = 0; g_recover_far = 0;
        if (setjmp(g_recover_jmp) == 0) {
            setPaths(env, obj, (jstring)apk_path.c_str(), (jstring)data_path.c_str());
            g_in_recover = false;
            compatLog("Cocos2d-x: nativeSetPaths OK");
        } else {
            g_in_recover = false;
            char sym_buf[160];
            elfNearestSym(so, g_recover_pc - (uint64_t)so->base, sym_buf, sizeof(sym_buf));
            compatLogFmt("Cocos2d-x: nativeSetPaths FAULT sig=%d esr=0x%08x pc=%p far=%p sym=%s",
                         g_recover_sig, g_recover_esr,
                         (void*)g_recover_pc, (void*)g_recover_far, sym_buf);
            { const uint32_t* insn = (const uint32_t*)(uintptr_t)g_recover_pc;
              compatLogFmt("INSN: [pc-12]=%08x [pc-8]=%08x [pc-4]=%08x [pc]=%08x [pc+4]=%08x",
                           insn[-3], insn[-2], insn[-1], insn[0], insn[1]); }
        }
        compatLogFlush();
    }

    typedef void (*NativeInit_fn)(JNIEnv*, jobject, jint, jint);
    NativeInit_fn nativeInit = (NativeInit_fn)so->findSym(
        "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit");
    if (!nativeInit)
        nativeInit = (NativeInit_fn)so->findSym(
            "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeResize");
    if (nativeInit) {
        compatLogFmt("Cocos2d-x: nativeInit @%p 1280x720", (void*)nativeInit);
        g_recover_owner = threadGetSelf(); g_in_recover = true; g_recover_sig = 0; g_recover_esr = 0; g_recover_far = 0;
        if (setjmp(g_recover_jmp) == 0) {
            nativeInit(env, obj, 1280, 720);
            g_in_recover = false;
            compatLog("Cocos2d-x: nativeInit OK");
        } else {
            g_in_recover = false;
            char sym_buf[160];
            elfNearestSym(so, g_recover_pc - (uint64_t)so->base, sym_buf, sizeof(sym_buf));
            compatLogFmt("Cocos2d-x: nativeInit FAULT sig=%d esr=0x%08x pc=%p far=%p sym=%s",
                         g_recover_sig, g_recover_esr,
                         (void*)g_recover_pc, (void*)g_recover_far, sym_buf);
            { const uint32_t* insn = (const uint32_t*)(uintptr_t)g_recover_pc;
              compatLogFmt("INSN: [pc-12]=%08x [pc-8]=%08x [pc-4]=%08x [pc]=%08x [pc+4]=%08x",
                           insn[-3], insn[-2], insn[-1], insn[0], insn[1]); }
        }
        compatLogFlush();
    }

    // Save the current GL back buffer as a PNG on the SD card — README material
    // and proof of what the game actually rendered at that point.
    auto saveGameScreenshot = [](int frame) {
        mkdir("sdmc:/AndroidHorizonNX/screenshots", 0777);
        char path[96];
        snprintf(path, sizeof(path),
                 "sdmc:/AndroidHorizonNX/screenshots/game_frame%d.png", frame);
        const int W = 1280, H = 720;
        uint8_t* px = (uint8_t*)malloc((size_t)W * H * 4);
        if (!px) return;
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
        uint8_t* row = (uint8_t*)malloc((size_t)W * 4);
        if (row) {  // GL origin is bottom-left — flip rows for the PNG
            for (int y = 0; y < H / 2; y++) {
                memcpy(row, px + (size_t)y * W * 4, (size_t)W * 4);
                memcpy(px + (size_t)y * W * 4, px + (size_t)(H - 1 - y) * W * 4, (size_t)W * 4);
                memcpy(px + (size_t)(H - 1 - y) * W * 4, row, (size_t)W * 4);
            }
            free(row);
        }
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(
            px, W, H, 32, W * 4, SDL_PIXELFORMAT_ABGR8888);
        if (s) {
            if (IMG_SavePNG(s, path) == 0) compatLogFmt("screenshot: %s", path);
            SDL_FreeSurface(s);
        }
        free(px);
    };

    typedef void (*NativeRender_fn)(JNIEnv*, jobject);
    NativeRender_fn nativeRender = (NativeRender_fn)so->findSym(
        "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender");

    // ─── Touch input: SDL finger events → Cocos2dxRenderer touch natives ─────
    // The Java GLSurfaceView would normally deliver these; we call the game's
    // registered native entry points directly. Begin/End take a single id+xy;
    // Move/Cancel take JNI arrays (blob layout: [jint len][payload]).
    typedef void     (*TouchBE_fn)(JNIEnv*, jobject, jint, jfloat, jfloat);
    typedef void     (*TouchArr_fn)(JNIEnv*, jobject, void*, void*, void*);
    typedef jboolean (*KeyDown_fn)(JNIEnv*, jobject, jint);
    TouchBE_fn  touchBegin = (TouchBE_fn)so->findSym(
        "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin");
    TouchBE_fn  touchEnd = (TouchBE_fn)so->findSym(
        "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd");
    TouchArr_fn touchMove = (TouchArr_fn)so->findSym(
        "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove");
    KeyDown_fn  keyDown = (KeyDown_fn)so->findSym(
        "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyDown");
    compatLogFmt("touch: begin=%p end=%p move=%p keyDown=%p",
                 (void*)touchBegin, (void*)touchEnd, (void*)touchMove, (void*)keyDown);

    struct IntArr1   { jint len; jint   v[1]; };
    struct FloatArr1 { jint len; jfloat v[1]; };

    if (nativeRender) {
        compatLogFmt("Cocos2d-x: nativeRender @%p — game loop", (void*)nativeRender);
        compatLogFlush();
        volatile int frame = 0;
        bool crashed = false;
        while (appletMainLoop()) {
            // Recovery window covers the whole iteration (event poll, render,
            // swap) — a fault inside eglSwapBuffers/SDL used to rethrow to the
            // OS with nothing in the log.
            g_recover_owner = threadGetSelf(); g_in_recover = true; g_recover_sig = 0; g_recover_esr = 0; g_recover_far = 0;
            if (setjmp(g_recover_jmp) == 0) {
                // Poll SDL events so + button exits
                SDL_Event ev;
                while (SDL_PollEvent(&ev)) {
                    if (ev.type == SDL_QUIT ||
                        (ev.type == SDL_JOYBUTTONDOWN && ev.jbutton.button == 10 /*PLUS*/)) {
                        g_in_recover = false;
                        goto game_loop_done;
                    }
                    // B button → Android BACK key (cocos routes it to menus)
                    if (ev.type == SDL_JOYBUTTONDOWN && ev.jbutton.button == 1 /*B*/ && keyDown)
                        keyDown(env, obj, 4 /*AKEYCODE_BACK*/);
                    // Shop guard active (see compatBlockShopEntry) — swallow
                    // touch input entirely so a lingering finger-down from
                    // the tap that opened Shop can't trigger anything else
                    // while we're forcing our way back out.
                    if (g_force_back_frames > 0) continue;
                    if (ev.type == SDL_FINGERDOWN && touchBegin)
                        touchBegin(env, obj, (jint)ev.tfinger.fingerId,
                                   ev.tfinger.x * 1280.0f, ev.tfinger.y * 720.0f);
                    if (ev.type == SDL_FINGERUP && touchEnd)
                        touchEnd(env, obj, (jint)ev.tfinger.fingerId,
                                 ev.tfinger.x * 1280.0f, ev.tfinger.y * 720.0f);
                    if (ev.type == SDL_FINGERMOTION && touchMove) {
                        IntArr1   ids = {1, {(jint)ev.tfinger.fingerId}};
                        FloatArr1 xs  = {1, {ev.tfinger.x * 1280.0f}};
                        FloatArr1 ys  = {1, {ev.tfinger.y * 720.0f}};
                        touchMove(env, obj, &ids, &xs, &ys);
                    }
                }

                // Shop guard: inject one synthetic BACK per frame while
                // active, giving the game's own navigation many chances to
                // process it and leave the Shop scene before whatever runs
                // next has a chance to hit its known crash. Decrement
                // unconditionally — if keyDown were ever null, tying the
                // countdown to a successful call would swallow touch input
                // (see the `continue` above) forever instead of just for
                // this one window.
                if (g_force_back_frames > 0) {
                    if (keyDown) keyDown(env, obj, 4 /*AKEYCODE_BACK*/);
                    g_force_back_frames--;
                }

                nativeRender(env, obj);

                // Android Horizon branding — gated purely on the pixel
                // fingerprint now. splashScreenHasCompleted turned out to
                // fire at ~8s, long before the actual "HILL CLIMB RACING /
                // LOADING..." screen with the version text ever appears —
                // gating on it (g_splash_active) meant the probe check got
                // permanently disabled before the target screen was ever
                // reached, so the overlay could never show. The 3-point
                // fingerprint alone is specific enough to gate this safely
                // for the whole session (real gameplay HUD looks nothing
                // like a dark vignette + white bar at these exact points).
                if (isOnLoadingScreen(frame)) drawBrandOverlay();

                // Milestone captures: early splash, post-splash, in-menu
                if (frame == 30 || frame == 300 || frame == 900)
                    saveGameScreenshot(frame);

                // Swap buffers (Cocos2d-x doesn't call eglSwapBuffers itself)
                if (g_egl_display != EGL_NO_DISPLAY && g_egl_surface != EGL_NO_SURFACE)
                    eglSwapBuffers(g_egl_display, g_egl_surface);
                else if (win)
                    SDL_GL_SwapWindow(win);

                g_in_recover = false;
            } else {
                g_in_recover = false;
                char sym_buf[160];
                elfNearestSym(so, g_recover_pc - (uint64_t)so->base, sym_buf, sizeof(sym_buf));
                compatLogFmt("Cocos2d-x: game loop FAULT sig=%d esr=0x%08x pc=%p far=%p sym=%s frame=%d — stop",
                             g_recover_sig, g_recover_esr,
                             (void*)g_recover_pc, (void*)g_recover_far, sym_buf, frame);
                {
                    extern void elfLogAddrInfo(const char*, uint64_t);
                    extern void elfDescribePc(uint64_t pc, char* buf, size_t sz);
                    char where[256];
                    elfDescribePc(g_recover_pc, where, sizeof(where));
                    compatLogFmt("FAULT pc: %s", where);
                    elfLogAddrInfo("FAULT pc", g_recover_pc);
                    elfLogAddrInfo("FAULT far", g_recover_far);
                }
                { const uint32_t* insn = (const uint32_t*)(uintptr_t)g_recover_pc;
                  compatLogFmt("INSN: [pc-12]=%08x [pc-8]=%08x [pc-4]=%08x [pc]=%08x [pc+4]=%08x",
                               insn[-3], insn[-2], insn[-1], insn[0], insn[1]); }
                crashed = true;
                goto game_loop_done;
            }

            ++frame;
            if (frame % 300 == 0) {
                compatLogFmt("game: frame %d", frame);
                compatLogFlush();
            }
        }
        game_loop_done:
        compatLogFmt("Cocos2d-x: loop done frames=%d", frame);
        // The mixer outlives the game (it belongs to the launcher process) —
        // silence it so music doesn't keep playing over the APK browser.
        compatAudioStopMusic();
        compatAudioStopAllEffects();
        jniUserDefaultsSave();
        g_game_so = nullptr;
        // Reset GL to sane defaults before the launcher's SDL_Renderer draws
        // again on this same context — see resetGLStateForLauncher's comment.
        // Deliberately NOT calling eglSwapBuffers/SDL_GL_SwapWindow here: an
        // extra swap outside of SDL_Renderer's own SDL_RenderPresent calls
        // desyncs its internal front/back-buffer bookkeeping (SDL2's GLES2
        // renderer backend assumes it's the only thing swapping this
        // surface) — that's almost certainly last build's freeze → fade →
        // rapid black/frozen-frame flicker. The very next SDL_RenderPresent
        // in the launcher's own screens presents cleanly on its own.
        resetGLStateForLauncher(1280, 720);
        // The game can call eglSwapInterval (it's in our shim table, mapped
        // straight to the real EGL function) to control its OWN frame
        // pacing — e.g. 0 for uncapped rendering. That setting is global to
        // the surface and survives a crash; if left at 0, the launcher's
        // SDL_Renderer (which expects vsync — SDL_RENDERER_PRESENTVSYNC)
        // would present as fast as the driver allows with no pacing at all,
        // which reads exactly like the persistent flicker reported after a
        // crash. Force vsync back on before the launcher renders again.
        if (g_egl_display != EGL_NO_DISPLAY) eglSwapInterval(g_egl_display, 1);

        // Games spawn real background libnx threads now (pt_create — e.g.
        // HCR's own asset loader, seen starting mid-session in the log).
        // We have no registry of them and no safe way to force-stop
        // arbitrary running native code, so after a CRASH we can't
        // guarantee none of them are still executing — still touching
        // JNI/audio/heap state that the launcher's menu code would then
        // race against. Two different GL/EGL state fixes (swap-desync,
        // then vsync) were tried across builds 78-79 and neither changed
        // the reported freeze→fade→rapid-flicker symptom, which fits an
        // ACTIVE ongoing conflict far better than a one-time leftover
        // state issue. Rather than keep guessing at symptoms, remove the
        // shared risk entirely: on a caught crash, don't attempt to
        // return to the launcher's menu in this same (possibly still
        // multi-threaded, possibly heap-corrupted) process at all — just
        // exit cleanly. Horizon OS tears down every thread in the process
        // together, which a same-process "return to menu" fundamentally
        // cannot guarantee. The launcher UI already forced a full app
        // restart before allowing a second game session (see gameRanOnce
        // in main.cpp), so a crash landing you at the same place is a
        // smaller UX regression than an unrecoverable flicker.
        if (crashed) {
            compatLog("Cocos2d-x: crash recovery — exiting app cleanly rather than "
                      "risking a still-running game thread racing the launcher");
            compatLogFlush();
            if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
            exit(0);
        }
    } else {
        compatLog("Cocos2d-x: nativeRender not found");
    }

    compatLogFlush();
    if (g_compat_log) { logFlushDedup(); fclose(g_compat_log); g_compat_log = nullptr; }
}
