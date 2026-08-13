/*
 * android_usb_jni.c — JNI bridge for Android USB Host capture.
 *
 * Two problems this solves:
 *
 *  (A) libusb cannot open /dev/bus/usb/* on Android without root. Java's
 *      UsbManager grants permission and hands us the device file descriptor;
 *      native code wraps it with libusb_wrap_sys_device() (via libuvc
 *      uvc_wrap()). MainActivity.nativeSetUsbFd(int) stores that fd.
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
#include <time.h>
#include <jni.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "MISRC", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "MISRC", __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#endif

/* ---- state ---- */
static atomic_int  s_usb_fd = -1;          /* granted fd, -1 = none */
static JavaVM     *s_jvm = NULL;            /* cached in JNI_OnLoad */
static jobject     s_activity_ref = NULL;  /* global ref to MainActivity */

/* App-external files dir (from Java getExternalFilesDir(null)). On Android 11+
 * this is the ONLY path exempt from scoped storage — native fopen() can write
 * here with no permission, and the user can retrieve files via adb pull.
 * Empty until nativeSetStoragePath() is called from MainActivity.onCreate. */
static char s_storage_path[512] = "";

/* Permission request result signaling (native waits, Java signals). */
static pthread_mutex_t  s_perm_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   s_perm_cv  = PTHREAD_COND_INITIALIZER;
static atomic_int       s_perm_result = -1; /* -1=pending, 0=denied, 1=granted */

/* ---- JNI_OnLoad: cache the JavaVM so native threads can attach ---- */
JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    s_jvm = vm;
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
    if (s_activity_ref) {
        (*env)->DeleteGlobalRef(env, s_activity_ref);
        s_activity_ref = NULL;
    }
    if (activity) {
        s_activity_ref = (*env)->NewGlobalRef(env, activity);
        LOGI("nativeRegisterActivity: stored activity ref");
    }
}

/* MainActivity.nativeSetUsbFd(int fd) */
JNIEXPORT void JNICALL
Java_dev_misrc_gui_MainActivity_nativeSetUsbFd(JNIEnv *env, jobject thiz, jint fd)
{
    (void)env; (void)thiz;
    atomic_store(&s_usb_fd, (int)fd);
    LOGI("nativeSetUsbFd: fd=%d", (int)fd);
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

/* MainActivity.nativeUsbPermissionResult(boolean granted) — Java signals the
 * waiting native thread that the permission dialog completed. */
JNIEXPORT void JNICALL
Java_dev_misrc_gui_MainActivity_nativeUsbPermissionResult(JNIEnv *env, jobject thiz, jboolean granted)
{
    (void)env; (void)thiz;
    atomic_store(&s_perm_result, granted ? 1 : 0);
    pthread_mutex_lock(&s_perm_mtx);
    pthread_cond_signal(&s_perm_cv);
    pthread_mutex_unlock(&s_perm_mtx);
    LOGI("nativeUsbPermissionResult: granted=%d", granted ? 1 : 0);
}

/* ---- C API used by the native capture path (gui_capture.c / hsdaoh) ---- */

int android_usb_get_fd(void) { return atomic_load(&s_usb_fd); }
int android_usb_has_fd(void) { return atomic_load(&s_usb_fd) >= 0; }
void android_usb_clear_fd(void) { atomic_store(&s_usb_fd, -1); }

/* Returns the Java-provided external files dir (scoped-storage-exempt,
 * writable, no permission). Empty string if nativeSetStoragePath() hasn't
 * run yet — callers should fall back to a default in that case. */
const char *android_get_storage_path(void) { return s_storage_path; }

/* If nativeRegisterActivity() never ran (e.g. its JNI binding failed in
 * onCreate), fall back to finding the current Activity via ActivityThread so
 * the permission request still works. Returns a local ref (caller must
 * DeleteLocalRef) or NULL. */
static jobject find_current_activity(JNIEnv *env)
{
    if (s_activity_ref) {
        /* Hold a fresh local ref so callers can uniformly DeleteLocalRef. */
        return (*env)->NewLocalRef(env, s_activity_ref);
    }
    /* android.app.ActivityThread.currentApplication().getCurrentTopResumedActivity()
         (API 31+) or a simpler older fallback. The robust, version-agnostic
         route is to enumerate the token's activity; but the simplest portable
         call that works on our minSdk 30 is:
             android.app.ActivityThread.currentActivity()
         which returns the current Activity instance. */
    jclass atCls = (*env)->FindClass(env, "android/app/ActivityThread");
    if (!atCls) return NULL;
    jmethodID currentAT = (*env)->GetStaticMethodID(env, atCls, "currentActivityThread", "()Landroid/app/ActivityThread;");
    if (!currentAT) { (*env)->DeleteLocalRef(env, atCls); return NULL; }
    jobject at = (*env)->CallStaticObjectMethod(env, atCls, currentAT);
    (*env)->DeleteLocalRef(env, atCls);
    if (!at) return NULL;
    jclass atObjCls = (*env)->GetObjectClass(env, at);
    jmethodID getAct = (*env)->GetMethodID(env, atObjCls, "getCurrentActivity", "()Landroid/app/Activity;");
    (*env)->DeleteLocalRef(env, atObjCls);
    jobject act = getAct ? (*env)->CallObjectMethod(env, at, getAct) : NULL;
    (*env)->DeleteLocalRef(env, at);
    if (act) LOGI("android_request_usb_permission: found Activity via ActivityThread fallback");
    return act;
}

/* Ask Java (MainActivity) to show the USB permission dialog for the known
 * hsdaoh device, then block up to timeout_seconds for the result.
 * Returns 1 if permission granted (and fd now set), 0 otherwise.
 * If an fd is already granted, returns 1 immediately without re-prompting. */
int android_request_usb_permission(int timeout_seconds)
{
    if (android_usb_has_fd()) return 1;
    if (!s_jvm) {
        LOGE("android_request_usb_permission: no JavaVM (JNI_OnLoad not called)");
        return 0;
    }

    JNIEnv *env = NULL;
    jint rc = (*s_jvm)->AttachCurrentThread(s_jvm, &env, NULL);
    if (rc != JNI_OK || !env) {
        LOGE("android_request_usb_permission: AttachCurrentThread failed: %d", rc);
        return 0;
    }

    jobject activity = find_current_activity(env);
    if (!activity) {
        LOGE("android_request_usb_permission: no Activity available");
        (*s_jvm)->DetachCurrentThread(s_jvm);
        return 0;
    }

    /* Reset result to pending before requesting. */
    atomic_store(&s_perm_result, -1);

    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID mid = cls ? (*env)->GetMethodID(env, cls, "requestPermissionFromNative", "()V") : NULL;
    if (!mid) {
        LOGE("android_request_usb_permission: requestPermissionFromNative not found on %s",
             cls ? "Activity" : "(no class)");
        if (cls) (*env)->DeleteLocalRef(env, cls);
        (*env)->DeleteLocalRef(env, activity);
        (*s_jvm)->DetachCurrentThread(s_jvm);
        return 0;
    }
    (*env)->CallVoidMethod(env, activity, mid);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
    (*s_jvm)->DetachCurrentThread(s_jvm);

    /* Block for the result. */
    int granted = 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (timeout_seconds > 0 ? timeout_seconds : 30);

    pthread_mutex_lock(&s_perm_mtx);
    while (atomic_load(&s_perm_result) == -1) {
        if (pthread_cond_timedwait(&s_perm_cv, &s_perm_mtx, &ts) != 0) {
            break;  /* timeout or error */
        }
    }
    granted = (atomic_load(&s_perm_result) == 1);
    pthread_mutex_unlock(&s_perm_mtx);

    if (granted) {
        LOGI("android_request_usb_permission: granted, fd=%d", android_usb_get_fd());
    } else {
        LOGE("android_request_usb_permission: denied or timed out");
    }
    return granted;
}
