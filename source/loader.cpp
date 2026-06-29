#include "compat/loader.h"
#include "compat/android.h"
#include <switch.h>
#include <minizip/unzip.h>
#include <sys/stat.h>
#include <dirent.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>

// ─── Logging ──────────────────────────────────────────────────────────────────
static FILE* g_compat_log = nullptr;

void compatLog(const char* msg) {
    if (g_compat_log) {
        fputs(msg, g_compat_log);
        fputc('\n', g_compat_log);
        fflush(g_compat_log);
    }
}
void compatLogFmt(const char* fmt, ...) {
    char buf[512];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    compatLog(buf);
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
static bool extractApk(const std::string& apk_path, const std::string& dest_dir) {
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
            std::string dest = dest_dir + "/lib/" + n.substr(14);
            compatLogFmt("extract lib: %s", n.substr(14).c_str());
            extractEntry(zf, dest);

        } else if (n.rfind("assets/", 0) == 0 && n.back() != '/') {
            std::string rel  = n.substr(7);
            std::string dest = dest_dir + "/assets/" + rel;
            // Create intermediate directories
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

// ─── Find the main .so ───────────────────────────────────────────────────────
// Scan the lib directory and find the .so that exports ANativeActivity_onCreate
// or JNI_OnLoad (fallback: pick the largest file)
static std::string findMainSo(const std::string& lib_dir) {
    DIR* d = opendir(lib_dir.c_str());
    if (!d) return "";

    std::vector<std::pair<size_t, std::string>> sos; // size, path
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

    if (sos.empty()) return "";

    // Sort largest first (heuristic: main lib is usually biggest)
    std::sort(sos.begin(), sos.end(),
              [](const auto& a, const auto& b){ return a.first > b.first; });

    // Return path of largest .so for now
    // (we'll improve this to check exports in a later iteration)
    compatLogFmt("findMainSo: picked %s (%zu bytes)",
                 sos[0].second.c_str(), sos[0].first);
    return sos[0].second;
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

    // switch-mesa EGL accepts NWindow* as EGLNativeWindowType
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

// ─── launchApk ───────────────────────────────────────────────────────────────
bool launchApk(const std::string& apk_path, const std::string& pkg_name) {
    // Open log file
    std::string log_path = "sdmc:/BareDroidNX/compat_log.txt";
    g_compat_log = fopen(log_path.c_str(), "w");
    compatLogFmt("launchApk: %s  pkg=%s", apk_path.c_str(), pkg_name.c_str());

    // ── 1. Set up directories ────────────────────────────────────────────────
    std::string base_dir = std::string("sdmc:/BareDroidNX/games/") + pkg_name;
    std::string lib_dir  = base_dir + "/lib";
    std::string asset_dir= base_dir + "/assets";
    mkdirp(base_dir);
    mkdirp(lib_dir);
    mkdirp(asset_dir);

    // ── 2. Extract APK ───────────────────────────────────────────────────────
    compatLog("Extracting APK...");
    if (!extractApk(apk_path, base_dir)) {
        compatLog("Extraction failed");
        if (g_compat_log) fclose(g_compat_log);
        return false;
    }

    // ── 3. Find and load the main .so ────────────────────────────────────────
    std::string main_so = findMainSo(lib_dir);
    if (main_so.empty()) {
        compatLog("No arm64 .so found in APK");
        if (g_compat_log) fclose(g_compat_log);
        return false;
    }

    compatLog("Setting up JNI environment...");
    jniSetup(&g_compat);

    compatLog("Loading main library...");
    LoadedSo* so = elfLoad(main_so.c_str());
    if (!so) {
        compatLog("ELF load failed");
        if (g_compat_log) fclose(g_compat_log);
        return false;
    }

    // ── 4. Set up ANativeWindow ───────────────────────────────────────────────
    compatLog("Setting up window...");
    ANativeWindow* win = &g_compat.window;
    win->width  = 1280;
    win->height = 720;
    win->format = 1; // RGBA_8888
    win->nwin   = nwindowGetDefault();

    if (!setupEGL(win)) {
        compatLog("EGL setup failed — continuing anyway");
        // Don't return; some games set up EGL themselves
    }

    // ── 5. Set up ANativeActivity ─────────────────────────────────────────────
    compatLog("Setting up ANativeActivity...");
    ANativeActivity* act = &g_compat.activity;
    memset(&g_compat.callbacks, 0, sizeof(g_compat.callbacks));
    act->callbacks         = &g_compat.callbacks;
    act->vm                = (JavaVM*)g_compat.vm_outer;
    act->env               = (JNIEnv*)g_compat.env_outer;
    act->clazz             = (void*)0x4001; // fake Java object handle
    act->internalDataPath  = base_dir.c_str();
    act->externalDataPath  = base_dir.c_str();
    act->sdkVersion        = 26;
    act->instance          = nullptr;
    act->window            = win;

    // Set up asset manager pointing to extracted assets
    strncpy(g_compat.asset_mgr.base_path, asset_dir.c_str(),
            sizeof(g_compat.asset_mgr.base_path) - 1);
    act->assetManager = &g_compat.asset_mgr;

    // ── 6. Call JNI_OnLoad if present ────────────────────────────────────────
    typedef int32_t (*JNI_OnLoad_fn)(JavaVM**, void*);
    JNI_OnLoad_fn jni_onload = (JNI_OnLoad_fn)so->findSym("JNI_OnLoad");
    if (jni_onload) {
        compatLog("Calling JNI_OnLoad...");
        int32_t ver = jni_onload((JavaVM**)g_compat.vm_outer, nullptr);
        compatLogFmt("JNI_OnLoad returned: 0x%X", ver);
    } else {
        compatLog("JNI_OnLoad not found (OK for NativeActivity games)");
    }

    // ── 7. Call ANativeActivity_onCreate ─────────────────────────────────────
    typedef void (*OnCreate_fn)(ANativeActivity*, void*, size_t);
    OnCreate_fn on_create = (OnCreate_fn)so->findSym("ANativeActivity_onCreate");
    if (!on_create) {
        compatLog("ANativeActivity_onCreate not found — trying largest exported sym");
        // Try other common entry points
        on_create = (OnCreate_fn)so->findSym("Java_com_google_androidgamesdk_GameActivity_initializeNativeCode");
        if (!on_create) {
            compatLog("No entry point found — cannot launch");
            if (g_compat_log) fclose(g_compat_log);
            return false;
        }
    }

    compatLog("Calling ANativeActivity_onCreate...");
    on_create(act, nullptr, 0);
    compatLog("onCreate returned");

    // ── 8. Drive lifecycle ────────────────────────────────────────────────────
    ANativeActivityCallbacks* cb = &g_compat.callbacks;

    if (cb->onStart)  { compatLog("onStart"); cb->onStart(act); }
    if (cb->onResume) { compatLog("onResume"); cb->onResume(act); }
    if (cb->onNativeWindowCreated) {
        compatLog("onNativeWindowCreated");
        cb->onNativeWindowCreated(act, win);
    }

    // ── 9. Game loop ──────────────────────────────────────────────────────────
    compatLog("Entering game loop (game drives rendering via EGL)");
    // The game's NativeActivity thread should now be driving the loop.
    // Since we're single-threaded, we just spin and swap buffers.
    // In a real implementation the game calls eglSwapBuffers itself.
    // For now, just let the callbacks drive things.
    // If the game spawned a thread (pt_create was called), it's a no-op,
    // so the game is stuck. But we log so we can see what happened.
    compatLog("Note: if game uses background threads they are stubbed out");

    // Flush log and return
    if (g_compat_log) { fflush(g_compat_log); fclose(g_compat_log); g_compat_log = nullptr; }
    return true;
}
