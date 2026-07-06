// ─── Android NDK Sensor API (android/sensor.h) — real accelerometer + gyro ───
// Unlike battery status (Java-only on real Android, no pure NDK path), motion
// sensors have a stable, well-documented native ABI that NDK games call
// directly with no JNI involved — so unlike battery, we can implement this
// against the EXACT real API surface rather than guessing at method names.
// Both sensors are backed by the SAME Switch six-axis sensor reading (handheld
// Joy-Con IMU) via libnx hid — one hidGetSixAxisSensorStates() call per frame
// feeds both the accelerometer and gyroscope events. No current test game
// (Hill Climb Racing) uses either — forward-looking groundwork for a future
// tilt/motion-control game, requested explicitly so "some games" that expect
// real Android sensor behaviour get it.
//
// Known limitation: Android's ASensorEventQueue is normally driven through
// ALooper's fd-based event notification (the queue has a pollable fd; the
// game either polls it directly or gets called back via ALooper_pollAll).
// We don't have a real fd-backed ALooper here, so games that rely on
// ALooper waking them up when new samples arrive won't be notified that way
// — but ASensorEventQueue_getEvents() always returns fresh real data when
// polled directly, which covers games that just poll every frame (the more
// common simple pattern for a tilt-steering control scheme).

#include "compat/loader.h"
#include <switch.h>
#include <cstring>
#include <cstdint>

extern void compatLog(const char* msg);
extern void compatLogFmt(const char* fmt, ...);

// ─── Exact Android NDK ABI layout (android/sensor.h) ─────────────────────────
// Struct shapes/offsets must match real Android exactly — games compiled
// against the real NDK header read these fields directly.
struct ASensorVector {
    union { float v[3]; struct { float x, y, z; }; struct { float azimuth, pitch, roll; }; };
    int8_t status;
    uint8_t reserved[3];
};
struct AMetaDataEvent { int32_t what; int32_t sensor; };
struct AUncalibratedEvent {
    union { float uncalib[3]; struct { float x_uncalib, y_uncalib, z_uncalib; }; };
    union { float bias[3];    struct { float x_bias,    y_bias,    z_bias;    }; };
};
struct AHeartRateEvent { float bpm; int8_t status; };
struct ADynamicSensorEvent { int32_t connected; int32_t handle; };
struct AAdditionalInfoEvent {
    int32_t type;
    int32_t serial;
    union { int32_t data_int32[14]; float data_float[14]; };
};
struct ASensorEvent {
    int32_t version;
    int32_t sensor;
    int32_t type;
    int32_t reserved0;
    int64_t timestamp;
    union {
        union {
            float data[16];
            ASensorVector vector;
            ASensorVector acceleration;
            ASensorVector magnetic;
            float temperature;
            float distance;
            float light;
            float pressure;
            float relative_humidity;
            AUncalibratedEvent uncalibrated_gyro;
            AUncalibratedEvent uncalibrated_magnetic;
            AMetaDataEvent meta_data;
            AHeartRateEvent heart_rate;
            ADynamicSensorEvent dynamic_sensor;
            AAdditionalInfoEvent additional_info;
        };
        struct { uint64_t data[8]; } u64;
    };
    int32_t flags;
    int32_t reserved1[3];
};

#define ASENSOR_TYPE_ACCELEROMETER 1
#define ASENSOR_TYPE_GYROSCOPE     4

// Opaque handles — games only ever hold pointers to these, never dereference
// their layout directly, so our own internal shape doesn't need to match Android.
struct ASensorManager { int unused; };
struct ASensor        { int32_t type; };
struct ASensorEventQueue {
    bool accelEnabled = false;
    bool gyroEnabled  = false;
    HidSixAxisSensorHandle handle;
    bool handleValid = false;
};

static ASensorManager g_manager;
static ASensor        g_accelSensor  = { ASENSOR_TYPE_ACCELEROMETER };
static ASensor        g_gyroSensor   = { ASENSOR_TYPE_GYROSCOPE };

static bool ensureSixAxisStarted(ASensorEventQueue* q) {
    if (q->handleValid) return true;
    // Handheld Joy-Con IMU — matches how this project is actually played
    // (docked mode has no controller input path yet, touch/handheld only).
    Result rc = hidGetSixAxisSensorHandles(&q->handle, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    if (R_FAILED(rc)) {
        compatLogFmt("sensors: hidGetSixAxisSensorHandles FAIL 0x%x", rc);
        return false;
    }
    rc = hidStartSixAxisSensor(q->handle);
    if (R_FAILED(rc)) {
        compatLogFmt("sensors: hidStartSixAxisSensor FAIL 0x%x", rc);
        return false;
    }
    q->handleValid = true;
    compatLog("sensors: six-axis sensor started (handheld)");
    return true;
}

extern "C" {

ASensorManager* ASensorManager_getInstance(void) {
    return &g_manager;
}
ASensorManager* ASensorManager_getInstanceForPackage(const char*) {
    return &g_manager;
}

int ASensorManager_getSensorList(ASensorManager*, ASensor const*** list) {
    static ASensor const* two[2];
    two[0] = &g_accelSensor;
    two[1] = &g_gyroSensor;
    if (list) *list = two;
    compatLog("sensors: ASensorManager_getSensorList → [accelerometer, gyroscope]");
    return 2;
}

ASensor const* ASensorManager_getDefaultSensor(ASensorManager*, int type) {
    compatLogFmt("sensors: ASensorManager_getDefaultSensor(type=%d)", type);
    if (type == ASENSOR_TYPE_ACCELEROMETER) return &g_accelSensor;
    if (type == ASENSOR_TYPE_GYROSCOPE)     return &g_gyroSensor;
    return nullptr;  // magnetic/etc not wired up yet
}

ASensorEventQueue* ASensorManager_createEventQueue(ASensorManager*, void* /*ALooper*/,
                                                    int /*ident*/, void* /*callback*/, void* /*data*/) {
    compatLog("sensors: ASensorManager_createEventQueue");
    return new ASensorEventQueue();
}

int ASensorManager_destroyEventQueue(ASensorManager*, ASensorEventQueue* q) {
    if (q && q->handleValid) hidStopSixAxisSensor(q->handle);
    delete q;
    return 0;
}

int ASensorEventQueue_enableSensor(ASensorEventQueue* q, ASensor const* sensor) {
    if (!q || !sensor) return -1;
    if (!ensureSixAxisStarted(q)) return -1;
    if (sensor->type == ASENSOR_TYPE_GYROSCOPE) {
        q->gyroEnabled = true;
        compatLog("sensors: gyroscope enabled");
    } else {
        q->accelEnabled = true;
        compatLog("sensors: accelerometer enabled");
    }
    return 0;
}

int ASensorEventQueue_disableSensor(ASensorEventQueue* q, ASensor const* sensor) {
    if (!q) return -1;
    if (sensor && sensor->type == ASENSOR_TYPE_GYROSCOPE) q->gyroEnabled = false;
    else                                                  q->accelEnabled = false;
    return 0;
}

int ASensorEventQueue_setEventRate(ASensorEventQueue*, ASensor const*, int32_t) {
    return 0;  // libnx doesn't expose a sample-rate knob here; always full rate
}

ssize_t ASensorEventQueue_getEvents(ASensorEventQueue* q, ASensorEvent* events, size_t count) {
    if (!q || (!q->accelEnabled && !q->gyroEnabled) || !events || count == 0) return 0;
    HidSixAxisSensorState state = {};
    size_t got = hidGetSixAxisSensorStates(q->handle, &state, 1);
    if (got == 0) return 0;

    int64_t now = (int64_t)armGetSystemTick();
    size_t n = 0;

    // Switch's HidVector acceleration is in units of G (1.0 = Earth gravity);
    // Android's ASensorVector for TYPE_ACCELEROMETER is in m/s^2 — multiply
    // by standard gravity to match what a real Android game expects.
    if (q->accelEnabled && n < count) {
        const float G = 9.80665f;
        ASensorEvent& ev = events[n++];
        memset(&ev, 0, sizeof(ev));
        ev.version   = sizeof(ASensorEvent);
        ev.sensor    = (int32_t)(intptr_t)&g_accelSensor;
        ev.type      = ASENSOR_TYPE_ACCELEROMETER;
        ev.timestamp = now;
        ev.acceleration.x = state.acceleration.x * G;
        ev.acceleration.y = state.acceleration.y * G;
        ev.acceleration.z = state.acceleration.z * G;
        ev.acceleration.status = 3;  // SENSOR_STATUS_ACCURACY_HIGH
    }

    // Switch's angular_velocity is in revolutions/sec; Android's TYPE_GYROSCOPE
    // expects radians/sec — multiply by 2*pi to match.
    if (q->gyroEnabled && n < count) {
        const float TWO_PI = 6.283185307f;
        ASensorEvent& ev = events[n++];
        memset(&ev, 0, sizeof(ev));
        ev.version   = sizeof(ASensorEvent);
        ev.sensor    = (int32_t)(intptr_t)&g_gyroSensor;
        ev.type      = ASENSOR_TYPE_GYROSCOPE;
        ev.timestamp = now;
        ev.vector.x = state.angular_velocity.x * TWO_PI;
        ev.vector.y = state.angular_velocity.y * TWO_PI;
        ev.vector.z = state.angular_velocity.z * TWO_PI;
        ev.vector.status = 3;  // SENSOR_STATUS_ACCURACY_HIGH
    }

    return (ssize_t)n;
}

int ASensor_getType(ASensor const* s) { return s ? s->type : 0; }
const char* ASensor_getName(ASensor const* s) {
    return (s && s->type == ASENSOR_TYPE_GYROSCOPE) ? "Switch Gyroscope" : "Switch Accelerometer";
}
const char* ASensor_getVendor(ASensor const*) { return "Nintendo"; }
float ASensor_getResolution(ASensor const*) { return 0.0001f; }
int ASensor_getMinDelay(ASensor const*) { return 10000; }  // 10ms ~= 100Hz, matches hid polling

} // extern "C"
