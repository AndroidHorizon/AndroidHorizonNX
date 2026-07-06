#include "compat/loader.h"
#include "compat/jni.h"
#include <switch.h>
#include <string.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <vector>
#include <unordered_map>
#include <string>
#include <unordered_set>

extern void compatLog(const char* msg);
extern void compatLogFmt(const char* fmt, ...);

static void* g_jni_funcs[JNI_NUM_SLOTS] = {};
static void* g_vm_funcs[VM_NUM_SLOTS]   = {};
static void* g_jni_inner = nullptr;
static void* g_vm_inner  = nullptr;
static void* g_jni_outer = nullptr;
static void* g_vm_outer  = nullptr;

static std::vector<JNINativeMethod> g_native_methods;

#define DUMMY_CLASS  ((void*)0x1001)
#define DUMMY_METHOD ((void*)0x2001)
#define DUMMY_FIELD  ((void*)0x3001)

// ─── Method registry ─────────────────────────────────────────────────────────
// Each unique (name, sig) pair gets a stable MethodEntry pointer used as jmethodID.
// g_method_pool holds the actual stable storage (jmethodID == &entry, must
// never move/realloc); g_method_index is a name|sig -> pointer hash map purely
// so repeated lookups don't do a linear scan of a pool that can grow past a
// hundred entries in a real game — GetMethodID/GetStaticMethodID resolution
// stays O(1) regardless of how many distinct methods the game has looked up
// so far, instead of degrading as more accumulate.
struct MethodEntry { char name[80]; char sig[128]; };
static MethodEntry g_method_pool[512];
static int g_method_count = 0;
static std::unordered_map<std::string, MethodEntry*> g_method_index;

static MethodEntry* lookupOrCreateMethod(const char* n, const char* sg) {
    std::string key = std::string(n ? n : "") + "|" + (sg ? sg : "");
    auto it = g_method_index.find(key);
    if (it != g_method_index.end()) return it->second;
    if (g_method_count < 512) {
        MethodEntry* e = &g_method_pool[g_method_count++];
        strncpy(e->name, n  ? n  : "", 79);  e->name[79]  = 0;
        strncpy(e->sig,  sg ? sg : "", 127); e->sig[127]  = 0;
        g_method_index.emplace(std::move(key), e);
        return e;
    }
    return nullptr;
}

// ─── UserDefault in-memory store (Cocos2d-x SharedPreferences emulation) ─────
// Game threads are real (see shim_table pt_create) and JNI calls arrive from
// worker threads too — serialize map access.
static std::unordered_map<std::string, int>         g_int_store;
static std::unordered_map<std::string, std::string> g_str_store;
static Mutex g_store_lock;
struct StoreLock {
    StoreLock()  { mutexLock(&g_store_lock); }
    ~StoreLock() { mutexUnlock(&g_store_lock); }
};

// Log-once helper: JNI lookups fire tens of thousands of times during loading
// and every compat log line fsyncs to the SD card — that I/O was the actual
// loading-screen bottleneck (~90 save-keys/second, build 64 log). Log each
// unique message once; repeats are silent.
static bool logOnce(const char* prefix, const char* a, const char* b = nullptr) {
    // unordered_set: this fires for every unique JNI lookup/key during
    // loading (tens of thousands of calls total across a session) — O(1)
    // average lookup instead of std::set's O(log n) tree walk.
    static std::unordered_set<std::string> seen;
    std::string key = std::string(prefix) + "|" + (a ? a : "") + "|" + (b ? b : "");
    StoreLock sl;
    return seen.insert(key).second;
}

static std::unordered_map<std::string, float> g_float_store;
static std::string g_ud_path;
static bool        g_ud_dirty = false;

// ─── UserDefault persistence ─────────────────────────────────────────────────
// The store was RAM-only, so every launch looked like a first run (ToS screen
// again, progress gone). Serialize to <game>/userdefaults.bin on every
// UserDefault.flush and at game exit; load before nativeInit.
// Record: [u8 type I/S/F][u32 klen][key][u32 vlen][value]
static void udWrite(FILE* f, char t, const std::string& k, const void* v, uint32_t vlen) {
    uint32_t klen = (uint32_t)k.size();
    fwrite(&t, 1, 1, f);
    fwrite(&klen, 4, 1, f);
    fwrite(k.data(), 1, klen, f);
    fwrite(&vlen, 4, 1, f);
    fwrite(v, 1, vlen, f);
}
void jniUserDefaultsSave(bool force) {
    StoreLock sl;
    if (g_ud_path.empty() || !g_ud_dirty) return;
    // The game calls UserDefault.flush() after each init phase (masteries,
    // event assets, IAP, ...) — on a fresh save that's several flushes within
    // a few seconds, and each one was re-serializing + rewriting the ENTIRE
    // store (thousands of keys) to the SD card, real disk I/O completely
    // independent of CPU clock speed. Collapse bursts to one real write every
    // 2s; the exit-time call passes force=true so the final state is never lost.
    static uint64_t s_lastSaveTick = 0;
    if (!force && s_lastSaveTick != 0) {
        uint64_t now = armGetSystemTick();
        uint64_t elapsedMs = (now - s_lastSaveTick) * 1000 / armGetSystemTickFreq();
        if (elapsedMs < 2000) return;  // stays dirty — a later call will flush it
    }
    std::string tmp = g_ud_path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { compatLogFmt("UserDefaults: save open FAIL %s", tmp.c_str()); return; }
    for (auto& kv : g_int_store) {
        int32_t v = kv.second;
        udWrite(f, 'I', kv.first, &v, 4);
    }
    for (auto& kv : g_float_store) {
        float v = kv.second;
        udWrite(f, 'F', kv.first, &v, 4);
    }
    for (auto& kv : g_str_store)
        udWrite(f, 'S', kv.first, kv.second.data(), (uint32_t)kv.second.size());
    fclose(f);
    remove(g_ud_path.c_str());
    rename(tmp.c_str(), g_ud_path.c_str());
    g_ud_dirty = false;
    s_lastSaveTick = armGetSystemTick();
    compatLogFmt("UserDefaults: saved %zu ints, %zu floats, %zu strings",
                 g_int_store.size(), g_float_store.size(), g_str_store.size());
}
void jniUserDefaultsLoad(const char* path) {
    StoreLock sl;
    g_ud_path = path ? path : "";
    g_ud_dirty = false;
    FILE* f = fopen(g_ud_path.c_str(), "rb");
    if (!f) { compatLogFmt("UserDefaults: no save file (fresh run)"); return; }
    for (;;) {
        char t; uint32_t klen = 0, vlen = 0;
        if (fread(&t, 1, 1, f) != 1) break;
        if (fread(&klen, 4, 1, f) != 1 || klen > 4096) break;
        std::string key(klen, '\0');
        if (fread(&key[0], 1, klen, f) != klen) break;
        if (fread(&vlen, 4, 1, f) != 1 || vlen > 1u << 20) break;
        std::string val(vlen, '\0');
        if (vlen && fread(&val[0], 1, vlen, f) != vlen) break;
        if (t == 'I' && vlen == 4) g_int_store[key]   = *(const int32_t*)val.data();
        else if (t == 'F' && vlen == 4) g_float_store[key] = *(const float*)val.data();
        else if (t == 'S') g_str_store[key] = val;
    }
    fclose(f);
    compatLogFmt("UserDefaults: loaded %zu ints, %zu floats, %zu strings",
                 g_int_store.size(), g_float_store.size(), g_str_store.size());
}

// ─── Network state ────────────────────────────────────────────────────────────
// Report the Switch's REAL connectivity (nifm). Games handle "offline" via
// their normal Android code paths — pretending to be online sends them into
// ad/IAP/network flows that hit stubs.
static bool netAvailable() {
    static int cached = -1;
    if (cached < 0) {
        cached = 0;
        if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) {
            NifmInternetConnectionType ct;
            u32 strength = 0;
            NifmInternetConnectionStatus st;
            if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&ct, &strength, &st)) &&
                st == NifmInternetConnectionStatus_Connected)
                cached = 1;
            nifmExit();
        }
        compatLogFmt("net: internet %s (nifm)", cached ? "CONNECTED" : "not connected");
    }
    return cached == 1;
}

// ─── Battery ──────────────────────────────────────────────────────────────────
// Unlike the accelerometer, there's no pure-NDK battery API on real Android —
// apps only get this via JNI (Java's BatteryManager, or a BATTERY_CHANGED
// broadcast receiver), and the exact method name an app/SDK uses for it isn't
// standardized. No current test game calls any of these, so this is
// forward-looking: real Switch battery data wired up under the handful of
// naming patterns actually seen across Android game SDKs, so a future game
// (or a bundled ad/analytics SDK) that happens to match gets real data
// instead of silently getting 0/false from the generic fallback.
static bool batteryPercent(u32* outPct) {
    static bool inited = false, ok = false;
    if (!inited) {
        inited = true;
        ok = R_SUCCEEDED(psmInitialize());
        if (ok) compatLog("battery: psm initialized");
        else    compatLog("battery: psmInitialize FAIL — battery queries will return 100");
    }
    if (ok && R_SUCCEEDED(psmGetBatteryChargePercentage(outPct))) return true;
    *outPct = 100;
    return false;
}
static bool batteryCharging() {
    PsmChargerType t;
    if (R_SUCCEEDED(psmGetChargerType(&t))) return t != PsmChargerType_Unconnected;
    return false;
}

// ─── All JNI stubs (must be defined before jniSetup uses their addresses) ─────

static jint     s_GetVersion(JNIEnv*)            { return JNI_VERSION_1_6; }
static jclass   s_FindClass(JNIEnv*, const char* n) {
    if (logOnce("FindClass", n)) compatLogFmt("JNI FindClass: %s", n ? n : "?");
    return DUMMY_CLASS;
}
static jclass   s_GetSuperclass(JNIEnv*, jclass)        { return DUMMY_CLASS; }
static jboolean s_IsAssignableFrom(JNIEnv*, jclass, jclass) { return JNI_TRUE; }
static jint     s_Throw(JNIEnv*, jthrowable)            { return 0; }
static jint     s_ThrowNew(JNIEnv*, jclass, const char*){ return 0; }
static jthrowable s_ExceptionOccurred(JNIEnv*)          { return nullptr; }
static void     s_ExceptionDescribe(JNIEnv*)             {}
static void     s_ExceptionClear(JNIEnv*)                {}
static void     s_FatalError(JNIEnv*, const char* m) {
    compatLogFmt("JNI FatalError: %s", m ? m : "?");
}
static jint     s_PushLocalFrame(JNIEnv*, jint)         { return 0; }
static jobject  s_PopLocalFrame(JNIEnv*, jobject o)     { return o; }
static jobject  s_NewGlobalRef(JNIEnv*, jobject o)      { return o; }
static void     s_DeleteGlobalRef(JNIEnv*, jobject)      {}
static void     s_DeleteLocalRef(JNIEnv*, jobject)       {}
static jboolean s_IsSameObject(JNIEnv*, jobject a, jobject b) {
    return a == b ? JNI_TRUE : JNI_FALSE;
}
static jobject  s_NewLocalRef(JNIEnv*, jobject o)       { return o; }
static jint     s_EnsureLocalCapacity(JNIEnv*, jint)    { return 0; }
static jobject  s_AllocObject(JNIEnv*, jclass)          { return nullptr; }
static jclass   s_GetObjectClass(JNIEnv*, jobject)      { return DUMMY_CLASS; }
static jboolean s_IsInstanceOf(JNIEnv*, jobject, jclass){ return JNI_TRUE; }

static jmethodID s_GetMethodID(JNIEnv*, jclass, const char* n, const char* sg) {
    if (logOnce("GetMethodID", n, sg))
        compatLogFmt("JNI GetMethodID: %s %s", n ? n : "?", sg ? sg : "?");
    MethodEntry* e = lookupOrCreateMethod(n, sg);
    return e ? (jmethodID)e : (jmethodID)DUMMY_METHOD;
}
static jmethodID s_GetStaticMethodID(JNIEnv*, jclass, const char* n, const char* sg) {
    if (logOnce("GetStaticMethodID", n, sg))
        compatLogFmt("JNI GetStaticMethodID: %s %s", n ? n : "?", sg ? sg : "?");
    MethodEntry* e = lookupOrCreateMethod(n, sg);
    return e ? (jmethodID)e : (jmethodID)DUMMY_METHOD;
}
static jfieldID s_GetFieldID(JNIEnv*, jclass, const char* n, const char* sg) {
    if (logOnce("GetFieldID", n, sg))
        compatLogFmt("JNI GetFieldID: %s %s", n ? n : "?", sg ? sg : "?");
    return DUMMY_FIELD;
}
static jfieldID s_GetStaticFieldID(JNIEnv*, jclass, const char* n, const char* sg) {
    compatLogFmt("JNI GetStaticFieldID: %s %s", n ? n : "?", sg ? sg : "?");
    return DUMMY_FIELD;
}

// Return-type stubs (generic, for slots that don't need per-call logging)
static jobject  s_RetObj(JNIEnv*, ...)   { return nullptr; }
static jobject  s_RetObjV(JNIEnv*, jobject, jmethodID, va_list) { return nullptr; }
static jboolean s_RetBool(JNIEnv*, ...)  { return JNI_FALSE; }
static jboolean s_RetBoolV(JNIEnv*, jobject, jmethodID, va_list) { return JNI_FALSE; }
static jint     s_RetInt(JNIEnv*, ...)   { return 0; }
static jint     s_RetIntV(JNIEnv*, jobject, jmethodID, va_list) { return 0; }
static jlong    s_RetLong(JNIEnv*, ...)  { return 0LL; }
static jlong    s_RetLongV(JNIEnv*, jobject, jmethodID, va_list) { return 0LL; }
static jfloat   s_RetFloat(JNIEnv*, ...) { return 0.0f; }
static jfloat   s_RetFloatV(JNIEnv*, jobject, jmethodID, va_list) { return 0.0f; }
static jdouble  s_RetDouble(JNIEnv*, ...) { return 0.0; }
static jdouble  s_RetDoubleV(JNIEnv*, jobject, jmethodID, va_list) { return 0.0; }
static void     s_RetVoid(JNIEnv*, ...)   {}
static void     s_RetVoidV(JNIEnv*, jobject, jmethodID, va_list) {}

// ─── Instance-method call stubs ───────────────────────────────────────────────
static jobject s_CallObjectMethod(JNIEnv*, jobject, jmethodID, ...) {
    compatLog("JNI CallObjectMethod"); return (jobject)"";
}
static jobject s_CallObjectMethodV(JNIEnv*, jobject, jmethodID, va_list) {
    compatLog("JNI CallObjectMethodV"); return (jobject)"";
}
static void s_CallVoidMethod(JNIEnv*, jobject, jmethodID, ...) {
    compatLog("JNI CallVoidMethod");
}
static void s_CallVoidMethodV(JNIEnv*, jobject, jmethodID, va_list) {
    compatLog("JNI CallVoidMethodV");
}
static jboolean s_CallBoolMethod(JNIEnv*, jobject, jmethodID, ...) {
    compatLog("JNI CallBooleanMethod"); return JNI_FALSE;
}
static jint s_CallIntMethod(JNIEnv*, jobject, jmethodID, ...) {
    compatLog("JNI CallIntMethod"); return 0;
}
static jlong s_CallLongMethod(JNIEnv*, jobject, jmethodID, ...) {
    compatLog("JNI CallLongMethod"); return 0LL;
}
static jobject s_NewObject(JNIEnv*, jclass, jmethodID, ...) {
    compatLog("JNI NewObject"); return nullptr;
}

// ─── Static-method dispatching stubs ─────────────────────────────────────────
// Helper: resolve a jmethodID to its MethodEntry (guards against DUMMY_METHOD)
static inline MethodEntry* methodEntry(jmethodID mid) {
    return (mid == (jmethodID)DUMMY_METHOD) ? nullptr : (MethodEntry*)mid;
}

static jint s_CallStaticIntMethodV(JNIEnv*, jclass, jmethodID mid, va_list args) {
    MethodEntry* e = methodEntry(mid);
    if (e && strcmp(e->name, "getIntegerForKey") == 0) {
        const char* key = (const char*)va_arg(args, jstring);
        jint defval    = va_arg(args, jint);
        if (key) {
            // Reads are silent (thousands during loading; each log line fsyncs
            // to SD). A periodic counter shows liveness in the log instead.
            static int reads = 0;
            if (++reads % 2000 == 0)
                compatLogFmt("JNI getIntegerForKey: %d reads", reads);
            StoreLock sl;
            auto it = g_int_store.find(key);
            if (it != g_int_store.end()) return it->second;
        }
        return defval;
    }
    if (e && (strcmp(e->name, "getBatteryLevel") == 0 ||
              strcmp(e->name, "getBatteryPercentage") == 0 ||
              strcmp(e->name, "getBatteryPercent") == 0)) {
        u32 pct = 100;
        batteryPercent(&pct);
        compatLogFmt("JNI %s() → %u", e->name, pct);
        return (jint)pct;
    }
    // SimpleAudioEngine: playEffect(path, loop, pitch, pan[, gain]) → effect id
    if (e && strcmp(e->name, "playEffect") == 0) {
        const char* p = (const char*)va_arg(args, jstring);
        int loop      = va_arg(args, int);
        // (Ljava/lang/String;ZFF)I → the trailing floats are pitch and gain;
        // apply gain per channel (pitch shifting is unsupported in SDL_mixer)
        double pitch = 1.0, gain = 1.0;
        if (strstr(e->sig, "ZFF")) { pitch = va_arg(args, double); gain = va_arg(args, double); }
        (void)pitch;
        return compatAudioPlayEffect(p, loop != 0, (float)gain);
    }
    return 0;
}
static jint s_CallStaticIntMethod(JNIEnv* env, jclass cls, jmethodID mid, ...) {
    va_list a; va_start(a, mid);
    jint r = s_CallStaticIntMethodV(env, cls, mid, a);
    va_end(a);
    return r;
}

static jfloat s_CallStaticFloatMethodV(JNIEnv*, jclass, jmethodID mid, va_list args) {
    MethodEntry* e = methodEntry(mid);
    if (!e) return 0.0f;
    if (strcmp(e->name, "getFloatForKey") == 0 || strcmp(e->name, "getDoubleForKey") == 0) {
        const char* key = (const char*)va_arg(args, jstring);
        double defval   = va_arg(args, double);
        if (key) {
            StoreLock sl;
            auto it = g_float_store.find(key);
            if (it != g_float_store.end()) return it->second;
        }
        return (jfloat)defval;
    }
    if (strcmp(e->name, "getEffectsVolume") == 0)         return compatAudioGetEffectsVolume();
    if (strcmp(e->name, "getBackgroundMusicVolume") == 0) return compatAudioGetMusicVolume();
    if (logOnce("FloatV", e->name, e->sig))
        compatLogFmt("JNI CallStaticFloatMethodV: %s %s → 0", e->name, e->sig);
    return 0.0f;
}
static jfloat s_CallStaticFloatMethod(JNIEnv* env, jclass c, jmethodID m, ...) {
    va_list a; va_start(a, m);
    jfloat r = s_CallStaticFloatMethodV(env, c, m, a);
    va_end(a);
    return r;
}

static jboolean s_CallStaticBoolMethodV(JNIEnv*, jclass, jmethodID mid, va_list args) {
    MethodEntry* e = methodEntry(mid);
    if (e) {
        // EULA/consent checks — return true so the game doesn't wait forever
        if (strcmp(e->name, "eulaHasBeenAccepted") == 0 ||
            strcmp(e->name, "hasUserConsented")      == 0) {
            if (logOnce("BoolT", e->name)) compatLogFmt("JNI %s() → true", e->name);
            return JNI_TRUE;
        }
        if (strcmp(e->name, "isNetworkAvailable") == 0)
            return netAvailable() ? JNI_TRUE : JNI_FALSE;
        if (strcmp(e->name, "getBoolForKey") == 0) {
            const char* key = (const char*)va_arg(args, jstring);
            jint defval     = va_arg(args, int);
            if (key) {
                StoreLock sl;
                auto it = g_int_store.find(std::string("b:") + key);
                if (it != g_int_store.end()) return it->second ? JNI_TRUE : JNI_FALSE;
            }
            return defval ? JNI_TRUE : JNI_FALSE;
        }
        if (strcmp(e->name, "isBackgroundMusicPlaying") == 0)
            return compatAudioMusicPlaying() ? JNI_TRUE : JNI_FALSE;
        if (strcmp(e->name, "isCharging") == 0 || strcmp(e->name, "isBatteryCharging") == 0 ||
            strcmp(e->name, "isPlugged") == 0) {
            bool charging = batteryCharging();
            compatLogFmt("JNI %s() → %s", e->name, charging ? "true" : "false");
            return charging ? JNI_TRUE : JNI_FALSE;
        }
        if (logOnce("BoolV", e->name))
            compatLogFmt("JNI CallStaticBooleanMethodV: %s → false", e->name);
    }
    return JNI_FALSE;
}
static jboolean s_CallStaticBoolMethod(JNIEnv* env, jclass cls, jmethodID mid, ...) {
    va_list a; va_start(a, mid);
    jboolean r = s_CallStaticBoolMethodV(env, cls, mid, a);
    va_end(a);
    return r;
}

static void s_CallStaticVoidMethodV(JNIEnv*, jclass, jmethodID mid, va_list args) {
    MethodEntry* e = methodEntry(mid);
    if (!e) { compatLog("JNI CallStaticVoidMethodV"); return; }

    if (strcmp(e->name, "setIntegerForKey") == 0) {
        const char* key = (const char*)va_arg(args, jstring);
        jint val        = va_arg(args, jint);
        if (logOnce("setInt", key ? key : "?"))
            compatLogFmt("JNI setIntegerForKey(%s, %d)", key ? key : "?", val);
        if (key) { StoreLock sl; g_int_store[key] = val; g_ud_dirty = true; }
        return;
    }
    if (strcmp(e->name, "setStringForKey") == 0) {
        const char* key = (const char*)va_arg(args, jstring);
        const char* val = (const char*)va_arg(args, jstring);
        if (logOnce("setStr", key ? key : "?"))
            compatLogFmt("JNI setStringForKey(%s, \"%s\")", key ? key : "?", val ? val : "null");
        if (key) { StoreLock sl; g_str_store[key] = val ? val : ""; g_ud_dirty = true; }
        return;
    }
    if (strcmp(e->name, "setBoolForKey") == 0) {
        const char* key = (const char*)va_arg(args, jstring);
        int val         = va_arg(args, int);
        // Was completely silent — this is likely how a mute/volume-on-off
        // toggle persists. Log every call (not logOnce) since we specifically
        // need to see the value change across calls, not just the first one.
        if (key) compatLogFmt("JNI setBoolForKey(%s, %d)", key, val);
        if (key) { StoreLock sl; g_int_store[std::string("b:") + key] = val ? 1 : 0; g_ud_dirty = true; }
        return;
    }
    if (strcmp(e->name, "setFloatForKey") == 0 || strcmp(e->name, "setDoubleForKey") == 0) {
        const char* key = (const char*)va_arg(args, jstring);
        double val      = va_arg(args, double);
        // Same gap as setBoolForKey above — a volume slider is almost
        // certainly persisted as a float and we couldn't see its value.
        if (key) compatLogFmt("JNI setFloatForKey(%s, %f)", key, val);
        if (key) { StoreLock sl; g_float_store[key] = (float)val; g_ud_dirty = true; }
        return;
    }
    if (strcmp(e->name, "flush") == 0) {
        jniUserDefaultsSave();
        return;
    }
    if (strcmp(e->name, "splashScreenHasCompleted") == 0) {
        compatLog("JNI splashScreenHasCompleted — hiding branding overlay");
        compatMarkSplashDone();
        return;
    }
    if (strcmp(e->name, "debugStringOnAndroid") == 0) {
        const char* msg = (const char*)va_arg(args, jstring);
        // Exact-match dedup (logOnce) doesn't help here: many of these embed
        // a continuously-changing value ("Pedals down = 0.083333", a new
        // number practically every frame during driving), so every call
        // looked "new" and triggered a real SD-card fflush via the normal
        // compatLog path — a steady stream of disk writes throughout actual
        // gameplay, not just loading. That's a much bigger stutter source
        // than anything overlay-related. Time-throttle instead: at most
        // 2/sec regardless of content, which is still plenty to see what
        // the game's doing without flushing every single frame.
        static uint64_t s_lastDebugTick = 0;
        uint64_t nowTick = armGetSystemTick();
        uint64_t elapsedMs = (nowTick - s_lastDebugTick) * 1000 / armGetSystemTickFreq();
        if (elapsedMs >= 500) {
            compatLogFmt("game debug: %s", msg ? msg : "null");
            s_lastDebugTick = nowTick;
        }
        return;
    }
    // trackPage(pageName) fires as the game enters a new screen. Any
    // IAP/purchase-related screen has a confirmed, deterministic crash (same
    // PC/offset on hardware every time, disassembly shows a generic inlined
    // std::vector append reading a dangling reference — consistent with
    // populating some product/offer list). First seen on the Shop screen
    // itself, then again from a DIFFERENT page, "iap/playstore/view" — same
    // exact crash, confirming it's the whole IAP surface, not just Shop.
    // Can't fix without the game's source or a disassembler-level patch, so
    // instead of guessing at each button's on-screen touch region, catch any
    // page name that smells like IAP and force our way back out before the
    // crash-prone code runs. jstring is literally a `const char*` in this
    // JNI layer (see s_NewStringUTF), so the page name is directly readable
    // without GetStringUTFChars.
    if (strcmp(e->name, "trackPage") == 0) {
        const char* page = (const char*)va_arg(args, jstring);
        compatLogFmt("JNI trackPage(%s)", page ? page : "null");
        compatMarkPastLoading();
        if (page) {
            char lower[64]; size_t n = 0;
            for (; page[n] && n < sizeof(lower) - 1; n++) lower[n] = (char)tolower((unsigned char)page[n]);
            lower[n] = '\0';
            if (strstr(lower, "shop") || strstr(lower, "iap") ||
                strstr(lower, "playstore") || strstr(lower, "purchase") ||
                strstr(lower, "store"))
                compatBlockShopEntry();
        }
        return;
    }

    // fetchCountryCode: on Android this is an async Java web request whose
    // result comes back via the native returnCountryCode(jstring) — without a
    // reply the post-EULA flow spins forever. Answer immediately with "US"
    // (also keeps the game on the non-GDPR consent path).
    if (strcmp(e->name, "fetchCountryCode") == 0) {
        typedef void (*RetCC_fn)(JNIEnv*, jobject, jstring);
        RetCC_fn f = (RetCC_fn)compatFindGameSym(
            "Java_com_fingersoft_game_MainActivity_returnCountryCode");
        compatLogFmt("JNI fetchCountryCode → returnCountryCode(\"US\") cb=%p", (void*)f);
        if (f) f((JNIEnv*)g_jni_outer, nullptr, (jstring)"US");
        return;
    }

    // ── SimpleAudioEngine (Cocos2dxSound / Cocos2dxMusic) → audio.cpp ──
    // Note: jboolean promotes to int and jfloat to double in va_lists.
    if (strcmp(e->name, "playBackgroundMusic") == 0) {
        const char* p = (const char*)va_arg(args, jstring);
        int loop      = va_arg(args, int);
        compatAudioPlayMusic(p, loop != 0);
        return;
    }
    if (strcmp(e->name, "preloadBackgroundMusic") == 0) {
        compatAudioPreloadMusic((const char*)va_arg(args, jstring));
        return;
    }
    if (strcmp(e->name, "stopBackgroundMusic") == 0)    { compatAudioStopMusic(); return; }
    if (strcmp(e->name, "pauseBackgroundMusic") == 0)   { compatAudioPauseMusic(); return; }
    if (strcmp(e->name, "resumeBackgroundMusic") == 0)  { compatAudioResumeMusic(); return; }
    if (strcmp(e->name, "rewindBackgroundMusic") == 0)  { compatAudioRewindMusic(); return; }
    if (strcmp(e->name, "setBackgroundMusicVolume") == 0) {
        float v = (float)va_arg(args, double);
        compatLogFmt("JNI setBackgroundMusicVolume(%f)", v);
        compatAudioSetMusicVolume(v);
        return;
    }
    if (strcmp(e->name, "preloadEffect") == 0) {
        compatAudioPreloadEffect((const char*)va_arg(args, jstring));
        return;
    }
    if (strcmp(e->name, "unloadEffect") == 0) {
        compatAudioUnloadEffect((const char*)va_arg(args, jstring));
        return;
    }
    if (strcmp(e->name, "setEffectsVolume") == 0) {
        float v = (float)va_arg(args, double);
        compatLogFmt("JNI setEffectsVolume(%f)", v);
        compatAudioSetEffectsVolume(v);
        return;
    }
    if (strcmp(e->name, "stopEffect") == 0)      { compatAudioStopEffect(va_arg(args, int)); return; }
    if (strcmp(e->name, "pauseEffect") == 0)     { compatAudioPauseEffect(va_arg(args, int)); return; }
    if (strcmp(e->name, "resumeEffect") == 0)    { compatAudioResumeEffect(va_arg(args, int)); return; }
    // setEffectVolume(id, vol) — per-channel volume for a SPECIFIC playing
    // effect (distinct from the global setEffectsVolume above). HCR calls this
    // continuously to ramp the looping engine sound with RPM and to fade/mute
    // it on crash or pause. This was falling through to the generic no-op
    // logger, so the engine sound never changed volume or stopped — reported
    // as "engine noise constantly playing even after dying / in menus".
    if (strcmp(e->name, "setEffectVolume") == 0) {
        int id     = va_arg(args, int);
        double vol = va_arg(args, double);
        // Called continuously (RPM ramp) — same changing-value dedup problem
        // as debugStringOnAndroid above, so throttle logging, not the actual
        // volume call (that must still apply every time).
        static uint64_t s_lastTick = 0;
        uint64_t now = armGetSystemTick();
        uint64_t elapsedMs = (now - s_lastTick) * 1000 / armGetSystemTickFreq();
        if (elapsedMs >= 500) {
            compatLogFmt("JNI setEffectVolume(id=%d, vol=%f)", id, vol);
            s_lastTick = now;
        }
        compatAudioSetEffectVolume(id, (float)vol);
        return;
    }
    // setEffectRate(id, rate) — playback-rate/pitch change for a specific
    // effect (engine pitch rising with RPM). Not implemented: SDL_mixer's
    // Mix_Chunk playback rate isn't adjustable per-channel without a custom
    // resampling engine bypassing SDL_mixer's mixer entirely. No-op for now
    // (silent — this one is expected/logged in README, not a bug to chase).
    if (strcmp(e->name, "setEffectRate") == 0) { va_arg(args, int); va_arg(args, double); return; }
    if (strcmp(e->name, "stopAllEffects") == 0)  { compatAudioStopAllEffects(); return; }
    if (strcmp(e->name, "pauseAllEffects") == 0) { compatAudioPauseAllEffects(); return; }
    if (strcmp(e->name, "resumeAllEffects") == 0){ compatAudioResumeAllEffects(); return; }
    if (strcmp(e->name, "end") == 0) { compatAudioStopMusic(); compatAudioStopAllEffects(); return; }

    if (logOnce("VoidV", e->name, e->sig))
        compatLogFmt("JNI CallStaticVoidMethodV: %s %s", e->name, e->sig);
}
static void s_CallStaticVoidMethod(JNIEnv* env, jclass cls, jmethodID mid, ...) {
    va_list a; va_start(a, mid);
    s_CallStaticVoidMethodV(env, cls, mid, a);
    va_end(a);
}

static jobject s_CallStaticObjectMethodV(JNIEnv*, jclass, jmethodID mid, va_list args) {
    MethodEntry* e = methodEntry(mid);
    if (!e) { compatLog("JNI CallStaticObjectMethodV"); return (jobject)""; }

    if (strcmp(e->name, "getStringForKey") == 0) {
        const char* key    = (const char*)va_arg(args, jstring);
        const char* defval = (const char*)va_arg(args, jstring);
        if (key) {
            StoreLock sl;
            auto it = g_str_store.find(key);
            if (it != g_str_store.end()) return (jobject)it->second.c_str();
        }
        return (jobject)(defval ? defval : "");
    }
    if (strcmp(e->name, "retrieveDefaultsString") == 0) {
        const char* key = (const char*)va_arg(args, jstring);
        if (key) {
            StoreLock sl;
            auto it = g_str_store.find(key);
            if (it != g_str_store.end()) return (jobject)it->second.c_str();
        }
        return (jobject)"";
    }
    if (strcmp(e->name, "aesDecrypt") == 0 || strcmp(e->name, "aesEncrypt") == 0) {
        const char* data = (const char*)va_arg(args, jstring);
        // Return input unchanged — identity cipher so round-trips are consistent
        return (jobject)(data ? data : "");
    }
    if (logOnce("ObjV", e->name, e->sig))
        compatLogFmt("JNI CallStaticObjectMethodV: %s %s → \"\"", e->name, e->sig);
    return (jobject)"";
}
static jobject s_CallStaticObjectMethod(JNIEnv* env, jclass cls, jmethodID mid, ...) {
    va_list a; va_start(a, mid);
    jobject r = s_CallStaticObjectMethodV(env, cls, mid, a);
    va_end(a);
    return r;
}

// Fields (get/set)
static jobject  s_GetObjField(JNIEnv*, jobject, jfieldID) { return nullptr; }
static jboolean s_GetBoolField(JNIEnv*, jobject, jfieldID){ return JNI_FALSE; }
static jbyte    s_GetByteField(JNIEnv*, jobject, jfieldID){ return 0; }
static jchar    s_GetCharField(JNIEnv*, jobject, jfieldID){ return 0; }
static jshort   s_GetShortField(JNIEnv*, jobject, jfieldID){ return 0; }
static jint     s_GetIntField(JNIEnv*, jobject, jfieldID) { return 0; }
static jlong    s_GetLongField(JNIEnv*, jobject, jfieldID){ return 0LL; }
static jfloat   s_GetFloatField(JNIEnv*, jobject, jfieldID){ return 0.0f; }
static jdouble  s_GetDoubleField(JNIEnv*, jobject, jfieldID){ return 0.0; }
static void     s_SetField(JNIEnv*, jobject, jfieldID, ...) {}

static jobject  s_GetStaticObjField(JNIEnv*, jclass, jfieldID) { return nullptr; }
static jint     s_GetStaticIntField(JNIEnv*, jclass, jfieldID)  { return 0; }
static jlong    s_GetStaticLongField(JNIEnv*, jclass, jfieldID) { return 0LL; }
static void     s_SetStaticField(JNIEnv*, jclass, jfieldID, ...) {}

// Strings
static jstring s_NewStringUTF(JNIEnv*, const char* str) { return (jstring)str; }
static jsize   s_GetStringUTFLength(JNIEnv*, jstring s) {
    return s ? (jsize)strlen((const char*)s) : 0;
}
static const char* s_GetStringUTFChars(JNIEnv*, jstring s, jboolean* cp) {
    if (cp) *cp = JNI_TRUE;
    // ART returns a malloc'd copy, and sloppy game code free()s it directly
    // instead of calling ReleaseStringUTFChars — that only survives if the
    // pointer really is heap. Returning the jstring (often a string literal
    // in OUR rodata) made such a free() corrupt the allocator (_free_r write
    // fault into the module's RX segment, build 60 log).
    const char* src = s ? (const char*)s : "";
    size_t n = strlen(src) + 1;
    char* c = (char*)malloc(n);
    if (c) memcpy(c, src, n);
    return c;
}
static void    s_ReleaseStringUTFChars(JNIEnv*, jstring, const char* c) { free((void*)c); }
static jstring s_NewString(JNIEnv*, const jchar*, jsize) { return nullptr; }
static jsize   s_GetStringLength(JNIEnv*, jstring)       { return 0; }
static const jchar* s_GetStringChars(JNIEnv*, jstring, jboolean* cp) {
    if (cp) *cp = JNI_FALSE; return nullptr;
}
static void    s_ReleaseStringChars(JNIEnv*, jstring, const jchar*) {}
static void    s_GetStringRegion(JNIEnv*, jstring, jsize, jsize, jchar*) {}
static void    s_GetStringUTFRegion(JNIEnv*, jstring, jsize, jsize, char*) {}
static const jchar* s_GetStringCritical(JNIEnv*, jstring, jboolean* cp) {
    if (cp) *cp = JNI_FALSE; return nullptr;
}
static void    s_ReleaseStringCritical(JNIEnv*, jstring, const jchar*) {}

// Arrays — blob layout is [jint len][payload]; len counts ELEMENTS.
static jsize s_GetArrayLength(JNIEnv*, jarray a) { return a ? *(jint*)a : 0; }
static jbyteArray s_NewByteArray(JNIEnv*, jsize len) {
    uint8_t* p = (uint8_t*)calloc(1, 4 + (size_t)(len > 0 ? len : 0));
    if (p && len > 0) *(jint*)p = len;
    return p;
}
static jobjectArray s_NewObjectArray(JNIEnv*, jsize, jclass, jobject) { return nullptr; }
static jobject      s_GetObjectArrayElement(JNIEnv*, jobjectArray, jsize) { return nullptr; }
static void         s_SetObjectArrayElement(JNIEnv*, jobjectArray, jsize, jobject) {}
static jbooleanArray s_NewBoolArray(JNIEnv*, jsize l)  { return (jbooleanArray)s_NewByteArray(nullptr, l); }
static jcharArray    s_NewCharArray(JNIEnv*, jsize l)  { return (jcharArray)s_NewByteArray(nullptr, l*2); }
static jshortArray   s_NewShortArray(JNIEnv*, jsize l) { return (jshortArray)s_NewByteArray(nullptr, l*2); }
static jintArray     s_NewIntArray(JNIEnv*, jsize l)   { return (jintArray)s_NewByteArray(nullptr, l*4); }
static jlongArray    s_NewLongArray(JNIEnv*, jsize l)  { return (jlongArray)s_NewByteArray(nullptr, l*8); }
static jfloatArray   s_NewFloatArray(JNIEnv*, jsize l) { return (jfloatArray)s_NewByteArray(nullptr, l*4); }
static jdoubleArray  s_NewDoubleArray(JNIEnv*, jsize l){ return (jdoubleArray)s_NewByteArray(nullptr, l*8); }

static jbyte*    s_GetByteElements(JNIEnv*, jbyteArray a, jboolean* cp) {
    if (cp) *cp = JNI_FALSE;
    return a ? (jbyte*)((uint8_t*)a + 4) : nullptr;
}
static void* s_GetElements(JNIEnv*, jarray a, jboolean* cp) {
    if (cp) *cp = JNI_FALSE;
    return a ? (uint8_t*)a + 4 : nullptr;
}
static void  s_ReleaseElements(JNIEnv*, jarray, void*, jint) {}
static void  s_GetByteRegion(JNIEnv*, jbyteArray a, jsize st, jsize l, jbyte* buf) {
    if (a && buf) memcpy(buf, (uint8_t*)a + 4 + st, (size_t)l);
}
static void  s_SetByteRegion(JNIEnv*, jbyteArray a, jsize st, jsize l, const jbyte* buf) {
    if (a && buf) memcpy((uint8_t*)a + 4 + st, buf, (size_t)l);
}
// Typed regions: the JNIEnv table has one slot per element type, so bake the
// element size into each function (cocos2d-x touch dispatch uses Int/Float).
template <size_t ES>
static void s_GetRegionT(JNIEnv*, jarray a, jsize st, jsize l, void* buf) {
    if (a && buf && l > 0) memcpy(buf, (uint8_t*)a + 4 + (size_t)st * ES, (size_t)l * ES);
}
template <size_t ES>
static void s_SetRegionT(JNIEnv*, jarray a, jsize st, jsize l, const void* buf) {
    if (a && buf && l > 0) memcpy((uint8_t*)a + 4 + (size_t)st * ES, buf, (size_t)l * ES);
}

static void* s_GetPrimArrayCritical(JNIEnv*, jarray a, jboolean* cp) {
    if (cp) *cp = JNI_FALSE;
    return a ? (uint8_t*)a + 4 : nullptr;
}
static void  s_ReleasePrimArrayCritical(JNIEnv*, jarray, void*, jint) {}

// Misc
static jint s_RegisterNatives(JNIEnv*, jclass, const JNINativeMethod* m, jint n) {
    for (jint i = 0; i < n; i++) {
        compatLogFmt("JNI RegisterNative: %s", m[i].name ? m[i].name : "?");
        g_native_methods.push_back(m[i]);
    }
    return JNI_OK;
}
static jint s_UnregisterNatives(JNIEnv*, jclass) { return JNI_OK; }
static jint s_MonitorEnter(JNIEnv*, jobject) { return 0; }
static jint s_MonitorExit(JNIEnv*, jobject)  { return 0; }
static jint s_GetJavaVM(JNIEnv*, JavaVM** out) {
    if (out) *out = (JavaVM*)g_vm_outer;
    return JNI_OK;
}
static jboolean s_ExceptionCheck(JNIEnv*) { return JNI_FALSE; }
static jobject  s_NewDirectByteBuffer(JNIEnv*, void*, jlong) { return nullptr; }
static void*    s_GetDirectBufferAddress(JNIEnv*, jobject)   { return nullptr; }
static jlong    s_GetDirectBufferCapacity(JNIEnv*, jobject)  { return -1L; }
static jweak    s_NewWeakGlobalRef(JNIEnv*, jobject o)       { return o; }
static void     s_DeleteWeakGlobalRef(JNIEnv*, jweak)        {}
static jobjectRefType s_GetObjectRefType(JNIEnv*, jobject)   { return JNILocalRefType; }

// ─── JavaVM stubs ─────────────────────────────────────────────────────────────
static jint vm_DestroyJavaVM(JavaVM*) { return 0; }
static jint vm_AttachCurrentThread(JavaVM*, JNIEnv** e, void*) {
    if (e) *e = (JNIEnv*)g_jni_outer; return JNI_OK;
}
static jint vm_DetachCurrentThread(JavaVM*) { return 0; }
static jint vm_GetEnv(JavaVM*, void** e, jint) {
    if (e) *e = g_jni_outer; return JNI_OK;
}
static jint vm_AttachDaemon(JavaVM*, JNIEnv** e, void*) {
    if (e) *e = (JNIEnv*)g_jni_outer; return JNI_OK;
}

// ─── jniSetup ─────────────────────────────────────────────────────────────────
void jniSetup(CompatLayer* cl) {
    // Reserved slots 0-3 stay null
    g_jni_funcs[4]  = (void*)s_GetVersion;
    g_jni_funcs[5]  = (void*)s_RetObj;       // DefineClass (stub)
    g_jni_funcs[6]  = (void*)s_FindClass;
    g_jni_funcs[7]  = (void*)s_RetObj;       // FromReflectedMethod
    g_jni_funcs[8]  = (void*)s_RetObj;       // FromReflectedField
    g_jni_funcs[9]  = (void*)s_RetObj;       // ToReflectedMethod
    g_jni_funcs[10] = (void*)s_GetSuperclass;
    g_jni_funcs[11] = (void*)s_IsAssignableFrom;
    g_jni_funcs[12] = (void*)s_RetObj;       // ToReflectedField
    g_jni_funcs[13] = (void*)s_Throw;
    g_jni_funcs[14] = (void*)s_ThrowNew;
    g_jni_funcs[15] = (void*)s_ExceptionOccurred;
    g_jni_funcs[16] = (void*)s_ExceptionDescribe;
    g_jni_funcs[17] = (void*)s_ExceptionClear;
    g_jni_funcs[18] = (void*)s_FatalError;
    g_jni_funcs[19] = (void*)s_PushLocalFrame;
    g_jni_funcs[20] = (void*)s_PopLocalFrame;
    g_jni_funcs[21] = (void*)s_NewGlobalRef;
    g_jni_funcs[22] = (void*)s_DeleteGlobalRef;
    g_jni_funcs[23] = (void*)s_DeleteLocalRef;
    g_jni_funcs[24] = (void*)s_IsSameObject;
    g_jni_funcs[25] = (void*)s_NewLocalRef;
    g_jni_funcs[26] = (void*)s_EnsureLocalCapacity;
    g_jni_funcs[27] = (void*)s_AllocObject;
    g_jni_funcs[28] = (void*)s_NewObject;     // NewObject (varargs)
    g_jni_funcs[29] = (void*)s_RetObjV;       // NewObjectV
    g_jni_funcs[30] = (void*)s_NewObject;     // NewObjectA
    g_jni_funcs[31] = (void*)s_GetObjectClass;
    g_jni_funcs[32] = (void*)s_IsInstanceOf;
    g_jni_funcs[33] = (void*)s_GetMethodID;
    // CallObjectMethod 34-36
    g_jni_funcs[34] = (void*)s_CallObjectMethod;
    g_jni_funcs[35] = (void*)s_CallObjectMethodV;
    g_jni_funcs[36] = (void*)s_CallObjectMethod;
    // CallBooleanMethod 37-39
    g_jni_funcs[37] = (void*)s_CallBoolMethod;
    g_jni_funcs[38] = (void*)s_RetBoolV;
    g_jni_funcs[39] = (void*)s_CallBoolMethod;
    // CallByte/Char/ShortMethod 40-48
    for (int i = 40; i <= 48; i++) g_jni_funcs[i] = (void*)s_RetInt;
    // CallIntMethod 49-51
    g_jni_funcs[49] = (void*)s_CallIntMethod;
    g_jni_funcs[50] = (void*)s_RetIntV;
    g_jni_funcs[51] = (void*)s_CallIntMethod;
    // CallLongMethod 52-54
    g_jni_funcs[52] = (void*)s_CallLongMethod;
    g_jni_funcs[53] = (void*)s_RetLongV;
    g_jni_funcs[54] = (void*)s_CallLongMethod;
    // CallFloat 55-57
    g_jni_funcs[55] = (void*)s_RetFloat;
    g_jni_funcs[56] = (void*)s_RetFloatV;
    g_jni_funcs[57] = (void*)s_RetFloat;
    // CallDouble 58-60
    g_jni_funcs[58] = (void*)s_RetDouble;
    g_jni_funcs[59] = (void*)s_RetDoubleV;
    g_jni_funcs[60] = (void*)s_RetDouble;
    // CallVoidMethod 61-63
    g_jni_funcs[61] = (void*)s_CallVoidMethod;
    g_jni_funcs[62] = (void*)s_CallVoidMethodV;
    g_jni_funcs[63] = (void*)s_CallVoidMethod;
    // Nonvirtual 64-93 (all void stubs)
    for (int i = 64; i <= 93; i++) g_jni_funcs[i] = (void*)s_RetVoid;
    // GetFieldID, Get/SetXxxField 94-112
    g_jni_funcs[94]  = (void*)s_GetFieldID;
    g_jni_funcs[95]  = (void*)s_GetObjField;
    g_jni_funcs[96]  = (void*)s_GetBoolField;
    g_jni_funcs[97]  = (void*)s_GetByteField;
    g_jni_funcs[98]  = (void*)s_GetCharField;
    g_jni_funcs[99]  = (void*)s_GetShortField;
    g_jni_funcs[100] = (void*)s_GetIntField;
    g_jni_funcs[101] = (void*)s_GetLongField;
    g_jni_funcs[102] = (void*)s_GetFloatField;
    g_jni_funcs[103] = (void*)s_GetDoubleField;
    for (int i = 104; i <= 112; i++) g_jni_funcs[i] = (void*)s_SetField;
    // GetStaticMethodID 113
    g_jni_funcs[113] = (void*)s_GetStaticMethodID;
    // CallStaticXxxMethod 114-143
    g_jni_funcs[114] = (void*)s_CallStaticObjectMethod;
    g_jni_funcs[115] = (void*)s_CallStaticObjectMethodV;
    g_jni_funcs[116] = (void*)s_CallStaticObjectMethod;  // A variant
    g_jni_funcs[117] = (void*)s_CallStaticBoolMethod;
    g_jni_funcs[118] = (void*)s_CallStaticBoolMethodV;
    g_jni_funcs[119] = (void*)s_CallStaticBoolMethod;    // A variant
    for (int i = 120; i <= 128; i++) g_jni_funcs[i] = (void*)s_RetInt; // byte/char/short
    g_jni_funcs[129] = (void*)s_CallStaticIntMethod;
    g_jni_funcs[130] = (void*)s_CallStaticIntMethodV;
    g_jni_funcs[131] = (void*)s_CallStaticIntMethod;     // A variant
    g_jni_funcs[132] = (void*)s_RetLong;
    g_jni_funcs[133] = (void*)s_RetLongV;
    g_jni_funcs[134] = (void*)s_RetLong;
    g_jni_funcs[135] = (void*)s_CallStaticFloatMethod;
    g_jni_funcs[136] = (void*)s_CallStaticFloatMethodV;
    g_jni_funcs[137] = (void*)s_CallStaticFloatMethod;   // A variant
    g_jni_funcs[138] = (void*)s_RetDouble;
    g_jni_funcs[139] = (void*)s_RetDoubleV;
    g_jni_funcs[140] = (void*)s_RetDouble;
    g_jni_funcs[141] = (void*)s_CallStaticVoidMethod;
    g_jni_funcs[142] = (void*)s_CallStaticVoidMethodV;
    g_jni_funcs[143] = (void*)s_CallStaticVoidMethod;    // A variant
    // GetStaticFieldID + Get/SetStaticXxxField 144-162
    g_jni_funcs[144] = (void*)s_GetStaticFieldID;
    g_jni_funcs[145] = (void*)s_GetStaticObjField;
    for (int i = 146; i <= 153; i++) g_jni_funcs[i] = (void*)s_GetStaticIntField;
    for (int i = 154; i <= 162; i++) g_jni_funcs[i] = (void*)s_SetStaticField;
    // Strings 163-170
    g_jni_funcs[163] = (void*)s_NewString;
    g_jni_funcs[164] = (void*)s_GetStringLength;
    g_jni_funcs[165] = (void*)s_GetStringChars;
    g_jni_funcs[166] = (void*)s_ReleaseStringChars;
    g_jni_funcs[167] = (void*)s_NewStringUTF;
    g_jni_funcs[168] = (void*)s_GetStringUTFLength;
    g_jni_funcs[169] = (void*)s_GetStringUTFChars;
    g_jni_funcs[170] = (void*)s_ReleaseStringUTFChars;
    // Arrays 171-214
    g_jni_funcs[171] = (void*)s_GetArrayLength;
    g_jni_funcs[172] = (void*)s_NewObjectArray;
    g_jni_funcs[173] = (void*)s_GetObjectArrayElement;
    g_jni_funcs[174] = (void*)s_SetObjectArrayElement;
    g_jni_funcs[175] = (void*)s_NewBoolArray;
    g_jni_funcs[176] = (void*)s_NewByteArray;
    g_jni_funcs[177] = (void*)s_NewCharArray;
    g_jni_funcs[178] = (void*)s_NewShortArray;
    g_jni_funcs[179] = (void*)s_NewIntArray;
    g_jni_funcs[180] = (void*)s_NewLongArray;
    g_jni_funcs[181] = (void*)s_NewFloatArray;
    g_jni_funcs[182] = (void*)s_NewDoubleArray;
    g_jni_funcs[183] = (void*)s_GetElements;  // GetBooleanArrayElements
    g_jni_funcs[184] = (void*)s_GetByteElements;
    g_jni_funcs[185] = (void*)s_GetElements;
    g_jni_funcs[186] = (void*)s_GetElements;
    g_jni_funcs[187] = (void*)s_GetElements;
    g_jni_funcs[188] = (void*)s_GetElements;
    g_jni_funcs[189] = (void*)s_GetElements;
    g_jni_funcs[190] = (void*)s_GetElements;
    for (int i = 191; i <= 198; i++) g_jni_funcs[i] = (void*)s_ReleaseElements;
    g_jni_funcs[199] = (void*)s_GetRegionT<1>;   // Boolean
    g_jni_funcs[200] = (void*)s_GetByteRegion;
    g_jni_funcs[201] = (void*)s_GetRegionT<2>;   // Char
    g_jni_funcs[202] = (void*)s_GetRegionT<2>;   // Short
    g_jni_funcs[203] = (void*)s_GetRegionT<4>;   // Int
    g_jni_funcs[204] = (void*)s_GetRegionT<8>;   // Long
    g_jni_funcs[205] = (void*)s_GetRegionT<4>;   // Float
    g_jni_funcs[206] = (void*)s_GetRegionT<8>;   // Double
    g_jni_funcs[207] = (void*)s_SetRegionT<1>;
    g_jni_funcs[208] = (void*)s_SetByteRegion;
    g_jni_funcs[209] = (void*)s_SetRegionT<2>;
    g_jni_funcs[210] = (void*)s_SetRegionT<2>;
    g_jni_funcs[211] = (void*)s_SetRegionT<4>;
    g_jni_funcs[212] = (void*)s_SetRegionT<8>;
    g_jni_funcs[213] = (void*)s_SetRegionT<4>;
    g_jni_funcs[214] = (void*)s_SetRegionT<8>;
    // Misc 215-232
    g_jni_funcs[215] = (void*)s_RegisterNatives;
    g_jni_funcs[216] = (void*)s_UnregisterNatives;
    g_jni_funcs[217] = (void*)s_MonitorEnter;
    g_jni_funcs[218] = (void*)s_MonitorExit;
    g_jni_funcs[219] = (void*)s_GetJavaVM;
    g_jni_funcs[220] = (void*)s_GetStringRegion;
    g_jni_funcs[221] = (void*)s_GetStringUTFRegion;
    g_jni_funcs[222] = (void*)s_GetPrimArrayCritical;
    g_jni_funcs[223] = (void*)s_ReleasePrimArrayCritical;
    g_jni_funcs[224] = (void*)s_GetStringCritical;
    g_jni_funcs[225] = (void*)s_ReleaseStringCritical;
    g_jni_funcs[226] = (void*)s_NewWeakGlobalRef;
    g_jni_funcs[227] = (void*)s_DeleteWeakGlobalRef;
    g_jni_funcs[228] = (void*)s_ExceptionCheck;
    g_jni_funcs[229] = (void*)s_NewDirectByteBuffer;
    g_jni_funcs[230] = (void*)s_GetDirectBufferAddress;
    g_jni_funcs[231] = (void*)s_GetDirectBufferCapacity;
    g_jni_funcs[232] = (void*)s_GetObjectRefType;

    // JavaVM table
    g_vm_funcs[3] = (void*)vm_DestroyJavaVM;
    g_vm_funcs[4] = (void*)vm_AttachCurrentThread;
    g_vm_funcs[5] = (void*)vm_DetachCurrentThread;
    g_vm_funcs[6] = (void*)vm_GetEnv;
    g_vm_funcs[7] = (void*)vm_AttachDaemon;

    // Build indirection chain
    g_jni_inner = (void*)g_jni_funcs;
    g_jni_outer = (void*)&g_jni_inner;
    g_vm_inner  = (void*)g_vm_funcs;
    g_vm_outer  = (void*)&g_vm_inner;

    cl->vm_outer  = g_vm_outer;
    cl->env_outer = g_jni_outer;
}
