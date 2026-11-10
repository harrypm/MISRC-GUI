/*
 * android_usb_jni.c — JNI bridge for Android USB Host capture.
 *
 * Two problems this solves:
 *
 *  (A) libusb cannot open /dev/bus/usb/* on Android without root. Java's
 *      UsbManager grants permission and hands us the device file descriptor;
 *      native code wraps it with libusb_wrap_sys_device() (via libuvc
 *      uvc_wrap()). JNI stages a duplicate of the Java fd, and each capture
 *      holds its own duplicate until libuvc has closed the wrapped device.
 *
 *  (B) The native GUI's "connect" button calls hsdaoh_open() directly and does
 *      NOT go through Java, so the permission dialog was never shown when the
 *      user pressed connect. android_request_usb_permission() calls back into
 *      MainActivity.requestPermissionFromNative() via JNI, then blocks on a
 *      condition variable until Java reports the result
 *      (nativeUsbPermissionResult). This lets the native connect path ask for
 *      permission on demand.
 *
 * Only compiled for Android (added to sources_gui under host_system == 'android'
 * in misrc_tools/meson.build).
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <jni.h>
#ifdef ENABLE_RTLSDR
#include <rtl-sdr.h>
#include "android_rtlsdr_usb.h"
#endif

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "MISRC", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "MISRC", __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#endif
/* Provided by the Android build of libhsdaoh. */
extern void hsdaoh_set_android_usb_fd_provider(int (*provider)(void));

/* ---- state ---- */
static pthread_mutex_t s_usb_fd_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_usb_fd = -1;                   /* native-owned grant, -1 = none */
static atomic_int  s_usb_device_present = 0; /* Java-reported physical attach state */
static JavaVM     *s_jvm = NULL;            /* cached in JNI_OnLoad */
static jobject     s_activity_ref = NULL;  /* global ref to MainActivity */
static pthread_mutex_t s_activity_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifdef ENABLE_RTLSDR
static pthread_mutex_t s_rtl_devices_mutex = PTHREAD_MUTEX_INITIALIZER;
static android_rtlsdr_usb_device_t s_rtl_devices[ANDROID_RTL_USB_MAX_DEVICES];
static int s_rtl_device_count;
static atomic_uint s_rtl_device_revision;
static atomic_int s_rtl_permission_device = -1;
static atomic_int s_rtl_permission_result = -1;
static android_rtlsdr_usb_identity_t s_rtl_permission_identity;
static android_rtlsdr_usb_identity_t s_rtl_open_identity;
#endif

/* App-external files dir (from Java getExternalFilesDir(null)). On Android 11+
 * this is the ONLY path exempt from scoped storage — native fopen() can write
 * here with no permission, and the user can retrieve files via adb pull.
 * Empty until nativeSetStoragePath() is called from MainActivity.onCreate. */
static char s_storage_path[512] = "";

/* Async USB permission result (render thread polls; Java sets).
 * The previous design blocked the raylib render thread on a cond var while
 * the system USB-permission dialog was shown. That dialog is a separate
 * Activity, so Android destroys our NativeActivity's EGL surface while the
 * render thread is stuck -> on resume the render loop is dead (app hangs).
 * Async launch + per-frame poll keeps the render thread alive across the
 * activity transition. */
static atomic_int s_async_perm_active = 0;   /* 1 = request launched */
static atomic_int s_async_perm_result = -1;  /* -1 pending, 0 denied, 1 granted */

/* Async settings picker (folder/file) result (render thread polls; Java sets).
 * Same rationale: ACTION_OPEN_DOCUMENT_TREE / ACTION_OPEN_DOCUMENT launch a
 * separate system Activity; blocking the render thread across that transition
 * kills EGL on resume. */
enum {
    ANDROID_PICKER_KIND_OUTPUT_DIR = 1,
    ANDROID_PICKER_KIND_PLAYBACK_A = 2,
    ANDROID_PICKER_KIND_PLAYBACK_B = 3,
};
static pthread_mutex_t s_async_picker_mtx = PTHREAD_MUTEX_INITIALIZER;
static int  s_async_picker_active = 0;        /* launched, until the render thread consumes */
static int  s_async_picker_result_pending = 0;
static int  s_async_picker_result_kind = 0;
static int  s_async_picker_result_ok = 0;
static char s_async_picker_result_path[1024] = "";
/* Java-keyboard text forwarded from MainActivity into native UI text fields. */
static pthread_mutex_t s_text_input_mtx = PTHREAD_MUTEX_INITIALIZER;
static char            s_text_input_buf[2048] = "";

static void clear_pending_jni_exception(JNIEnv *env, const char *ctx)
{
    if (!env) return;
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        LOGE("Cleared pending JNI exception (%s)", ctx ? ctx : "unknown");
    }
}

/* The capture thread acquires its descriptor while the staged grant is locked.
 * Clearing or replacing the grant can then leave an open session alone. */
static int android_usb_dup_fd(void)
{
    pthread_mutex_lock(&s_usb_fd_mutex);
    int fd = s_usb_fd >= 0 ? fcntl(s_usb_fd, F_DUPFD_CLOEXEC, 0) : -1;
    pthread_mutex_unlock(&s_usb_fd_mutex);
    return fd;
}

static void android_usb_replace_fd(int fd)
{
    pthread_mutex_lock(&s_usb_fd_mutex);
    int previous = s_usb_fd;
    s_usb_fd = fd;
    pthread_mutex_unlock(&s_usb_fd_mutex);
    if (previous >= 0)
        close(previous);
}

/* ---- JNI_OnLoad: cache the JavaVM so native threads can attach ---- */
JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    s_jvm = vm;
    hsdaoh_set_android_usb_fd_provider(android_usb_dup_fd);
    return JNI_VERSION_1_6;
}

/* ---- JNI methods called from Java ----
 * NOTE on name mangling: dev.misrc.gui.MainActivity has dots (-> '_') and NO
 * underscores, so the JNI symbol is Java_dev_misrc_gui_MainActivity_<method>.
 * Do NOT insert _1 (that escapes a literal underscore, which we don't have).
 */

/* MainActivity.nativeRegisterActivity(Activity) — called from onCreate so
 * native code can call back into Java (requestPermissionFromNative). */
JNIEXPORT void JNICALL
Java_dev_misrc_gui_MainActivity_nativeRegisterActivity(JNIEnv *env, jobject thiz, jobject activity)
{
    (void)thiz;
    pthread_mutex_lock(&s_activity_mutex);
    if (s_activity_ref) {
        (*env)->DeleteGlobalRef(env, s_activity_ref);
        s_activity_ref = NULL;
    }
    if (activity) {
        s_activity_ref = (*env)->NewGlobalRef(env, activity);
        LOGI("nativeRegisterActivity: stored activity ref");
    }
    pthread_mutex_unlock(&s_activity_mutex);
}

JNIEXPORT jboolean JNICALL
Java_dev_misrc_gui_MainActivity_nativeIsRtlUsbDevice(JNIEnv *env, jobject thiz,
                                                    jint vid, jint pid)
{
    (void)env;
    (void)thiz;
#ifdef ENABLE_RTLSDR
    return rtlsdr_get_device_name_by_usb_id((uint16_t)vid, (uint16_t)pid)
        ? JNI_TRUE : JNI_FALSE;
#else
    (void)vid;
    (void)pid;
    return JNI_FALSE;
#endif
}

JNIEXPORT void JNICALL
Java_dev_misrc_gui_MainActivity_nativeSetRtlUsbDevices(JNIEnv *env, jobject thiz,
                                                      jintArray devices)
{
    (void)thiz;
#ifdef ENABLE_RTLSDR
    jsize count = (*env)->GetArrayLength(env, devices) / 3;
    if (count > ANDROID_RTL_USB_MAX_DEVICES) count = ANDROID_RTL_USB_MAX_DEVICES;
    jint values[3 * ANDROID_RTL_USB_MAX_DEVICES];
    android_rtlsdr_usb_device_t next[ANDROID_RTL_USB_MAX_DEVICES] = {0};
    (*env)->GetIntArrayRegion(env, devices, 0, count * 3, values);
    if ((*env)->ExceptionCheck(env)) return;
    for (jsize i = 0; i < count; ++i) {
        next[i].id = values[3 * i];
        next[i].vid = (uint16_t)values[3 * i + 1];
        next[i].pid = (uint16_t)values[3 * i + 2];
    }
    pthread_mutex_lock(&s_rtl_devices_mutex);
    if (count != s_rtl_device_count || memcmp(next, s_rtl_devices, sizeof(next)) != 0) {
        memcpy(s_rtl_devices, next, sizeof(next));
        s_rtl_device_count = count;
        atomic_fetch_add(&s_rtl_device_revision, 1);
    }
    pthread_mutex_unlock(&s_rtl_devices_mutex);
#else
    (void)env;
    (void)devices;
#endif
}

JNIEXPORT void JNICALL
Java_dev_misrc_gui_MainActivity_nativeRtlUsbPermissionResult(JNIEnv *env, jobject thiz,
                                                            jint device, jboolean granted,
                                                            jstring serial)
{
    (void)thiz;
#ifdef ENABLE_RTLSDR
    pthread_mutex_lock(&s_rtl_devices_mutex);
    if (device == atomic_load(&s_rtl_permission_device) &&
        atomic_load(&s_rtl_permission_result) < 0) {
        s_rtl_permission_identity.serial[0] = '\0';
        if (granted == JNI_TRUE && serial) {
            jsize length = (*env)->GetStringUTFLength(env, serial);
            if (length < ANDROID_RTL_USB_SERIAL_SIZE) {
                const char *value = (*env)->GetStringUTFChars(env, serial, NULL);
                if (value) {
                    memcpy(s_rtl_permission_identity.serial, value, (size_t)length + 1);
                    (*env)->ReleaseStringUTFChars(env, serial, value);
                } else {
                    (*env)->ExceptionClear(env);
                }
            }
        }
        atomic_store(&s_rtl_permission_result, granted == JNI_TRUE);
    }
    pthread_mutex_unlock(&s_rtl_devices_mutex);
#else
    (void)env;
    (void)device;
    (void)granted;
    (void)serial;
#endif
}

/* MainActivity.nativeSetUsbFd(int fd) */
JNIEXPORT jboolean JNICALL
Java_dev_misrc_gui_MainActivity_nativeSetUsbFd(JNIEnv *env, jobject thiz, jint fd)
{
    (void)env; (void)thiz;
    int owned_fd = fd >= 0 ? fcntl(fd, F_DUPFD_CLOEXEC, 0) : -1;
    if (fd >= 0 && owned_fd < 0) {
        LOGE("nativeSetUsbFd: could not duplicate the USB descriptor");
        return JNI_FALSE;
    }
    android_usb_replace_fd(owned_fd);
    return JNI_TRUE;
}

/* MainActivity.nativeSetStoragePath(String path) — called from onCreate with
 * getExternalFilesDir(null).getAbsolutePath() so native code has the real,
 * scoped-storage-exempt, writable output base before main() runs. */
JNIEXPORT void JNICALL
Java_dev_misrc_gui_MainActivity_nativeSetStoragePath(JNIEnv *env, jobject thiz, jstring path)
{
    (void)thiz;
    const char *cpath = path ? (*env)->GetStringUTFChars(env, path, NULL) : NULL;
    if (cpath) {
        snprintf(s_storage_path, sizeof(s_storage_path), "%s", cpath);
        (*env)->ReleaseStringUTFChars(env, path, cpath);
        LOGI("nativeSetStoragePath: %s", s_storage_path);
    }
}

/* MainActivity.nativeUsbPermissionResult(boolean granted) — Java reports the
 * USB permission dialog result. Stored in async state; the render thread
 * polls android_poll_usb_permission_result() each frame (no cond var). */
JNIEXPORT void JNICALL
Java_dev_misrc_gui_MainActivity_nativeUsbPermissionResult(JNIEnv *env, jobject thiz, jboolean granted)
{
    (void)env; (void)thiz;
    atomic_store(&s_async_perm_result, granted ? 1 : 0);
    /* s_async_perm_active stays 1 until the render thread consumes the result. */
    LOGI("nativeUsbPermissionResult: granted=%d", granted ? 1 : 0);
}

/* MainActivity.nativePickerResult(int kind, boolean accepted, byte[] utf8Path) */
JNIEXPORT jboolean JNICALL
Java_dev_misrc_gui_MainActivity_nativePickerResult(JNIEnv *env, jobject thiz, jint kind,
                                                  jboolean accepted, jbyteArray path)
{
    (void)thiz;
    char cpath[sizeof(s_async_picker_result_path)] = {0};
    jsize length = path ? (*env)->GetArrayLength(env, path) : 0;
    bool valid_path = (size_t)length < sizeof(cpath) &&
                      (!accepted || kind == ANDROID_PICKER_KIND_OUTPUT_DIR || length > 0);
    if (valid_path && length > 0) {
        (*env)->GetByteArrayRegion(env, path, 0, length, (jbyte *)cpath);
        if ((*env)->ExceptionCheck(env)) {
            clear_pending_jni_exception(env, "GetByteArrayRegion(picker path)");
            valid_path = false;
        } else if (memchr(cpath, '\0', (size_t)length)) {
            valid_path = false;
        }
    }

    pthread_mutex_lock(&s_async_picker_mtx);
    bool queued = s_async_picker_active && !s_async_picker_result_pending &&
                  s_async_picker_result_kind == (int)kind;
    if (queued) {
        s_async_picker_result_ok = accepted && valid_path;
        if (s_async_picker_result_ok) {
            memcpy(s_async_picker_result_path, cpath, (size_t)length + 1);
        } else {
            s_async_picker_result_path[0] = '\0';
        }
        s_async_picker_result_pending = 1;
    }
    pthread_mutex_unlock(&s_async_picker_mtx);

    if (queued) {
        LOGI("nativePickerResult: kind=%d accepted=%d path=%s",
             (int)kind, accepted && valid_path, valid_path ? cpath : "(invalid)");
    }
    return queued && valid_path ? JNI_TRUE : JNI_FALSE;
}

/* MainActivity.nativeSetUsbDevicePresent(boolean present) — Java reports
 * whether a known/UVC capture device is physically attached. Native device
 * enumeration uses this to only list the MS2130 entry when real hardware is
 * present (the patched hsdaoh_get_device_count() unconditionally returns 1
 * on Android because rootless libusb cannot enumerate /dev/bus/usb). */
JNIEXPORT void JNICALL
Java_dev_misrc_gui_MainActivity_nativeSetUsbDevicePresent(JNIEnv *env, jobject thiz, jboolean present)
{
    (void)env; (void)thiz;
    atomic_store(&s_usb_device_present, present ? 1 : 0);
    LOGI("nativeSetUsbDevicePresent: %d", present ? 1 : 0);
}

int android_usb_device_present(void)
{
    return atomic_load(&s_usb_device_present);
}

/* MainActivity.nativePushTextInput(String text) */
JNIEXPORT void JNICALL
Java_dev_misrc_gui_MainActivity_nativePushTextInput(JNIEnv *env, jobject thiz, jstring text)
{
    (void)thiz;
    if (!text) return;
    const char *ctext = (*env)->GetStringUTFChars(env, text, NULL);
    if (!ctext) return;

    pthread_mutex_lock(&s_text_input_mtx);
    size_t cur_len = strlen(s_text_input_buf);
    size_t add_len = strlen(ctext);
    size_t room = (sizeof(s_text_input_buf) - 1 > cur_len)
        ? (sizeof(s_text_input_buf) - 1 - cur_len)
        : 0;
    if (room > 0) {
        if (add_len > room) add_len = room;
        memcpy(s_text_input_buf + cur_len, ctext, add_len);
        s_text_input_buf[cur_len + add_len] = '\0';
    }
    pthread_mutex_unlock(&s_text_input_mtx);

    (*env)->ReleaseStringUTFChars(env, text, ctext);
}

/* ---- C API used by the native capture path (gui_capture.c / hsdaoh) ---- */

int android_usb_has_fd(void)
{
    pthread_mutex_lock(&s_usb_fd_mutex);
    int present = s_usb_fd >= 0;
    pthread_mutex_unlock(&s_usb_fd_mutex);
    return present;
}

void android_usb_clear_fd(void)
{
    android_usb_replace_fd(-1);
}

/* Returns the Java-provided external files dir (scoped-storage-exempt,
 * writable, no permission). Empty string if nativeSetStoragePath() hasn't
 * run yet — callers should fall back to a default in that case. */
const char *android_get_storage_path(void) { return s_storage_path; }
size_t android_drain_text_input(char *out, size_t out_len)
{
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    pthread_mutex_lock(&s_text_input_mtx);
    size_t n = strlen(s_text_input_buf);
    if (n >= out_len) n = out_len - 1;
    if (n > 0) {
        memcpy(out, s_text_input_buf, n);
        out[n] = '\0';
    }
    s_text_input_buf[0] = '\0';
    pthread_mutex_unlock(&s_text_input_mtx);
    return n;
}

/* If nativeRegisterActivity() never ran (e.g. its JNI binding failed in
 * onCreate), fall back to finding the current Activity via ActivityThread so
 * the permission request still works. Returns a local ref (caller must
 * DeleteLocalRef) or NULL. */
static jobject find_current_activity(JNIEnv *env)
{
    /* Hold a local ref across a concurrent Java-side activity registration. */
    pthread_mutex_lock(&s_activity_mutex);
    jobject registered = s_activity_ref ? (*env)->NewLocalRef(env, s_activity_ref) : NULL;
    pthread_mutex_unlock(&s_activity_mutex);
    if (registered) return registered;
    /* android.app.ActivityThread.currentApplication().getCurrentTopResumedActivity()
         (API 31+) or a simpler older fallback. The robust, version-agnostic
         route is to enumerate the token's activity; but the simplest portable
         call that works on our minSdk 30 is:
             android.app.ActivityThread.currentActivity()
         which returns the current Activity instance. */
    jclass atCls = (*env)->FindClass(env, "android/app/ActivityThread");
    if (!atCls) {
        clear_pending_jni_exception(env, "FindClass(ActivityThread)");
        return NULL;
    }
    jmethodID currentAT = (*env)->GetStaticMethodID(env, atCls, "currentActivityThread", "()Landroid/app/ActivityThread;");
    if (!currentAT) {
        clear_pending_jni_exception(env, "GetStaticMethodID(currentActivityThread)");
        (*env)->DeleteLocalRef(env, atCls);
        return NULL;
    }
    jobject at = (*env)->CallStaticObjectMethod(env, atCls, currentAT);
    clear_pending_jni_exception(env, "CallStaticObjectMethod(currentActivityThread)");
    (*env)->DeleteLocalRef(env, atCls);
    if (!at) return NULL;
    jclass atObjCls = (*env)->GetObjectClass(env, at);
    if (!atObjCls) {
        clear_pending_jni_exception(env, "GetObjectClass(ActivityThread)");
        (*env)->DeleteLocalRef(env, at);
        return NULL;
    }
    jmethodID getAct = (*env)->GetMethodID(env, atObjCls, "getCurrentActivity", "()Landroid/app/Activity;");
    if (!getAct) {
        clear_pending_jni_exception(env, "GetMethodID(getCurrentActivity)");
        (*env)->DeleteLocalRef(env, atObjCls);
        (*env)->DeleteLocalRef(env, at);
        return NULL;
    }
    (*env)->DeleteLocalRef(env, atObjCls);
    jobject act = getAct ? (*env)->CallObjectMethod(env, at, getAct) : NULL;
    clear_pending_jni_exception(env, "CallObjectMethod(getCurrentActivity)");
    (*env)->DeleteLocalRef(env, at);
    if (act) LOGI("android_request_usb_permission: found Activity via ActivityThread fallback");
    return act;
}

/* Show/hide Android soft keyboard from native text-edit state. */
void android_set_keyboard_visible(int visible)
{
    if (!s_jvm) return;

    JNIEnv *env = NULL;
    jint rc = (*s_jvm)->AttachCurrentThread(s_jvm, &env, NULL);
    if (rc != JNI_OK || !env) return;

    jobject activity = find_current_activity(env);
    if (!activity) {
        (*s_jvm)->DetachCurrentThread(s_jvm);
        return;
    }

    jclass cls = (*env)->GetObjectClass(env, activity);
    if (!cls) {
        clear_pending_jni_exception(env, "GetObjectClass(activity) for keyboard");
        (*env)->DeleteLocalRef(env, activity);
        (*s_jvm)->DetachCurrentThread(s_jvm);
        return;
    }
    jmethodID mid = cls ? (*env)->GetMethodID(env, cls, "setKeyboardVisibleFromNative", "(Z)V") : NULL;
    if (mid) {
        (*env)->CallVoidMethod(env, activity, mid, visible ? JNI_TRUE : JNI_FALSE);
        clear_pending_jni_exception(env, "CallVoidMethod(setKeyboardVisibleFromNative)");
    } else {
        clear_pending_jni_exception(env, "GetMethodID(setKeyboardVisibleFromNative)");
        LOGE("android_set_keyboard_visible: method not found");
    }

    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
    (*s_jvm)->DetachCurrentThread(s_jvm);
}

#ifdef ENABLE_RTLSDR
unsigned android_rtlsdr_device_revision(void)
{
    return atomic_load(&s_rtl_device_revision);
}

int android_rtlsdr_list_usb_devices(android_rtlsdr_usb_device_t *devices, int capacity)
{
    if (!devices || capacity <= 0) return 0;
    pthread_mutex_lock(&s_rtl_devices_mutex);
    int count = s_rtl_device_count < capacity ? s_rtl_device_count : capacity;
    memcpy(devices, s_rtl_devices, (size_t)count * sizeof(*devices));
    pthread_mutex_unlock(&s_rtl_devices_mutex);
    return count;
}

static int android_rtlsdr_call_activity(const char *method, int device_id, int fallback,
                                       const android_rtlsdr_usb_identity_t *open)
{
    if (!s_jvm) return fallback;
    JNIEnv *env = NULL;
    jint status = (*s_jvm)->GetEnv(s_jvm, (void **)&env, JNI_VERSION_1_6);
    bool detach = status == JNI_EDETACHED;
    if (detach) status = (*s_jvm)->AttachCurrentThread(s_jvm, &env, NULL);
    if (status != JNI_OK || !env) return fallback;

    int result = fallback;
    jobject activity = find_current_activity(env);
    if (activity) {
        jclass cls = (*env)->GetObjectClass(env, activity);
        const char *signature = open ? "(IIILjava/lang/String;)I" : "(I)I";
        jmethodID mid = cls ? (*env)->GetMethodID(env, cls, method, signature) : NULL;
        if (mid && open) {
            bool need_serial = open->serial[0] != '\0';
            jstring serial = need_serial ? (*env)->NewStringUTF(env, open->serial) : NULL;
            if (!need_serial || serial) {
                result = (*env)->CallIntMethod(env, activity, mid, (jint)device_id,
                                               (jint)open->usb.vid, (jint)open->usb.pid, serial);
            }
            if (serial) (*env)->DeleteLocalRef(env, serial);
        } else if (mid) {
            result = (*env)->CallIntMethod(env, activity, mid, (jint)device_id);
        }
        if ((*env)->ExceptionCheck(env)) {
            clear_pending_jni_exception(env, method);
            result = fallback;
        }
        if (cls) (*env)->DeleteLocalRef(env, cls);
        (*env)->DeleteLocalRef(env, activity);
    }
    if (detach) (*s_jvm)->DetachCurrentThread(s_jvm);
    return result;
}

void android_rtlsdr_set_open_identity(const android_rtlsdr_usb_identity_t *identity)
{
    pthread_mutex_lock(&s_rtl_devices_mutex);
    s_rtl_open_identity = *identity;
    pthread_mutex_unlock(&s_rtl_devices_mutex);
}

int android_rtlsdr_open_usb_fd(int device_id)
{
    pthread_mutex_lock(&s_rtl_devices_mutex);
    android_rtlsdr_usb_identity_t identity = s_rtl_open_identity;
    s_rtl_open_identity.usb.id = -1;
    pthread_mutex_unlock(&s_rtl_devices_mutex);
    if (identity.usb.id != device_id) return -1;
    return android_rtlsdr_call_activity("openRtlUsbFdFromNative", device_id, -1, &identity);
}

int android_rtlsdr_request_permission(int device_id)
{
    if (device_id < 0 || android_rtlsdr_permission_pending()) return 0;
    pthread_mutex_lock(&s_rtl_devices_mutex);
    bool found = false;
    for (int i = 0; i < s_rtl_device_count; ++i) {
        if (s_rtl_devices[i].id == device_id) {
            s_rtl_permission_identity.usb = s_rtl_devices[i];
            found = true;
            break;
        }
    }
    if (found) {
        atomic_store(&s_rtl_permission_result, -1);
        atomic_store(&s_rtl_permission_device, device_id);
    }
    pthread_mutex_unlock(&s_rtl_devices_mutex);
    if (!found) return 0;
    if (android_rtlsdr_call_activity("requestRtlUsbPermissionFromNative", device_id, 0, NULL)) {
        return 1;
    }
    atomic_store(&s_rtl_permission_device, -1);
    return 0;
}

int android_rtlsdr_permission_pending(void)
{
    return atomic_load(&s_rtl_permission_device) >= 0;
}

int android_rtlsdr_poll_permission(android_rtlsdr_usb_identity_t *identity, int *granted)
{
    if (!android_rtlsdr_permission_pending() ||
        atomic_load(&s_rtl_permission_result) < 0) return 0;
    pthread_mutex_lock(&s_rtl_devices_mutex);
    atomic_store(&s_rtl_permission_device, -1);
    *identity = s_rtl_permission_identity;
    *granted = atomic_load(&s_rtl_permission_result);
    pthread_mutex_unlock(&s_rtl_devices_mutex);
    return 1;
}
#endif

/* ---- Async picker API (non-blocking; render thread polls) ----
 *
 * The render thread calls android_pick_*_async() to launch the system picker
 * (via JNI into MainActivity, which runs startActivityForResult on the UI
 * thread) and returns immediately. The render thread keeps rendering frames.
 * When the user selects a folder/file, MainActivity.onActivityResult calls
 * nativePickerResult(), which stores the result in s_async_picker_* state.
 * The render thread polls android_poll_picker_result() each frame and applies
 * the result to settings. This avoids blocking the render thread across the
 * picker Activity lifecycle (which destroys/recreates our EGL surface). */
static int android_launch_picker_async(int picker_kind, int playback_channel)
{
    if (!s_jvm) { LOGE("android_launch_picker_async: no JavaVM"); return 0; }

    JNIEnv *env = NULL;
    jint rc = (*s_jvm)->AttachCurrentThread(s_jvm, &env, NULL);
    if (rc != JNI_OK || !env) {
        LOGE("android_launch_picker_async: AttachCurrentThread failed: %d", rc);
        return 0;
    }

    jobject activity = find_current_activity(env);
    if (!activity) {
        LOGE("android_launch_picker_async: no Activity available");
        (*s_jvm)->DetachCurrentThread(s_jvm);
        return 0;
    }

    jclass cls = (*env)->GetObjectClass(env, activity);
    if (!cls) {
        clear_pending_jni_exception(env, "GetObjectClass(activity) for picker async");
        (*env)->DeleteLocalRef(env, activity);
        (*s_jvm)->DetachCurrentThread(s_jvm);
        return 0;
    }
    jmethodID mid = NULL;
    if (picker_kind == ANDROID_PICKER_KIND_OUTPUT_DIR) {
        mid = (*env)->GetMethodID(env, cls, "requestOutputFolderFromNative", "()V");
    } else {
        mid = (*env)->GetMethodID(env, cls, "requestPlaybackFileFromNative", "(I)V");
    }
    if (!mid) {
        clear_pending_jni_exception(env, "GetMethodID(picker async)");
        LOGE("android_launch_picker_async: picker method not found (kind=%d)", picker_kind);
        (*env)->DeleteLocalRef(env, cls);
        (*env)->DeleteLocalRef(env, activity);
        (*s_jvm)->DetachCurrentThread(s_jvm);
        return 0;
    }

    pthread_mutex_lock(&s_async_picker_mtx);
    bool started = !s_async_picker_active;
    if (started) {
        s_async_picker_active = 1;
        s_async_picker_result_kind = picker_kind;
    }
    pthread_mutex_unlock(&s_async_picker_mtx);

    if (started) {
        if (picker_kind == ANDROID_PICKER_KIND_OUTPUT_DIR) {
            (*env)->CallVoidMethod(env, activity, mid);
        } else {
            (*env)->CallVoidMethod(env, activity, mid, (jint)playback_channel);
        }
        if ((*env)->ExceptionCheck(env)) {
            clear_pending_jni_exception(env, "CallVoidMethod(Android picker async)");
            pthread_mutex_lock(&s_async_picker_mtx);
            started = s_async_picker_result_pending != 0;
            if (!started) s_async_picker_active = 0;
            pthread_mutex_unlock(&s_async_picker_mtx);
        }
    }

    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
    (*s_jvm)->DetachCurrentThread(s_jvm);
    return started ? 1 : 0;
}

int android_pick_output_folder_async(void)
{
    return android_launch_picker_async(ANDROID_PICKER_KIND_OUTPUT_DIR, 0);
}

int android_pick_playback_file_async(int channel)
{
    int kind = (channel == 1) ? ANDROID_PICKER_KIND_PLAYBACK_B : ANDROID_PICKER_KIND_PLAYBACK_A;
    return android_launch_picker_async(kind, channel);
}

int android_picker_active(void)
{
    int v;
    pthread_mutex_lock(&s_async_picker_mtx);
    v = s_async_picker_active;
    pthread_mutex_unlock(&s_async_picker_mtx);
    return v;
}

/* Returns 1 if a picker result was consumed (out_* filled), 0 if none pending. */
int android_poll_picker_result(int *out_kind, int *out_ok, char *out_path, size_t out_path_len)
{
    pthread_mutex_lock(&s_async_picker_mtx);
    if (!s_async_picker_result_pending) {
        pthread_mutex_unlock(&s_async_picker_mtx);
        return 0;
    }
    s_async_picker_result_pending = 0;
    s_async_picker_active = 0;
    if (out_kind) *out_kind = s_async_picker_result_kind;
    if (out_ok)   *out_ok   = s_async_picker_result_ok;
    if (out_path && out_path_len > 0) {
        size_t path_len = strlen(s_async_picker_result_path);
        if (path_len < out_path_len) {
            memcpy(out_path, s_async_picker_result_path, path_len + 1);
        } else {
            out_path[0] = '\0';
            if (out_ok) *out_ok = 0;
        }
    }
    pthread_mutex_unlock(&s_async_picker_mtx);
    return 1;
}

/* ---- Async USB permission API (non-blocking; render thread polls) ----
 *
 * android_request_usb_permission_async() asks MainActivity to show the system
 * USB permission dialog and returns immediately. The render thread keeps
 * rendering. When the user grants/denies, MainActivity's broadcast receiver
 * calls nativeUsbPermissionResult(), storing the result. The render thread
 * polls android_poll_usb_permission_result() each frame; on grant (and fd set
 * by Java via nativeSetUsbFd) it proceeds to hsdaoh_open + start_stream. */
int android_request_usb_permission_async(void)
{
    if (android_usb_has_fd()) return 1;
    if (!s_jvm) { LOGE("android_request_usb_permission_async: no JavaVM"); return 0; }

    JNIEnv *env = NULL;
    jint rc = (*s_jvm)->AttachCurrentThread(s_jvm, &env, NULL);
    if (rc != JNI_OK || !env) {
        LOGE("android_request_usb_permission_async: AttachCurrentThread failed: %d", rc);
        return 0;
    }

    jobject activity = find_current_activity(env);
    if (!activity) {
        LOGE("android_request_usb_permission_async: no Activity available");
        (*s_jvm)->DetachCurrentThread(s_jvm);
        return 0;
    }

    jclass cls = (*env)->GetObjectClass(env, activity);
    if (!cls) {
        clear_pending_jni_exception(env, "GetObjectClass(activity) for USB permission async");
        (*env)->DeleteLocalRef(env, activity);
        (*s_jvm)->DetachCurrentThread(s_jvm);
        return 0;
    }
    jmethodID mid = (*env)->GetMethodID(env, cls, "requestPermissionFromNative", "()V");
    if (!mid) {
        clear_pending_jni_exception(env, "GetMethodID(requestPermissionFromNative async)");
        LOGE("android_request_usb_permission_async: method not found");
        (*env)->DeleteLocalRef(env, cls);
        (*env)->DeleteLocalRef(env, activity);
        (*s_jvm)->DetachCurrentThread(s_jvm);
        return 0;
    }
    atomic_store(&s_async_perm_result, -1);
    atomic_store(&s_async_perm_active, 1);
    (*env)->CallVoidMethod(env, activity, mid);
    bool failed = (*env)->ExceptionCheck(env);
    clear_pending_jni_exception(env, "CallVoidMethod(requestPermissionFromNative async)");
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
    (*s_jvm)->DetachCurrentThread(s_jvm);
    if (failed) atomic_store(&s_async_perm_active, 0);
    return !failed;
}

int android_permission_pending(void)
{
    return atomic_load(&s_async_perm_active);
}

/* Returns 1 if a permission result was consumed (out_granted filled), 0 if still pending. */
int android_poll_usb_permission_result(int *out_granted)
{
    if (!atomic_load(&s_async_perm_active)) return 0;
    int r = atomic_load(&s_async_perm_result);
    if (r == -1) return 0;  /* dialog still open */
    atomic_store(&s_async_perm_active, 0);
    if (out_granted) *out_granted = r;
    atomic_store(&s_async_perm_result, -1);
    return 1;
}
