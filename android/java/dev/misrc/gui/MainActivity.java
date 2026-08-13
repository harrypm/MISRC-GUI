package dev.misrc.gui;

import android.app.NativeActivity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.util.Log;
import java.util.HashMap;
import java.util.Map;

/**
 * NativeActivity entry point for the MISRC GUI Android build with USB Host
 * permission handling.
 *
 * On Android, libusb cannot open /dev/bus/usb/* without root. The standard
 * pattern is: request the user's permission for the USB device via UsbManager,
 * then open the device and pass its file descriptor to native code, which
 * wraps it with libusb_wrap_sys_device() (via libuvc uvc_wrap()) so hsdaoh can
 * drive the MS2130 without root.
 *
 * Flow:
 *   1. A matching MS2130/MS2131 is plugged in -> Android sends
 *      USB_DEVICE_ATTACHED (device_filter.xml + manifest intent-filter) and
 *      launches this activity, OR onCreate/onNewIntent re-runs the request.
 *   2. requestUsbPermission() asks UsbManager to prompt the user for
 *      permission; a BroadcastReceiver catches the granted result.
 *   3. On grant, openUsbDeviceAndHandFd() opens the device, reads its file
 *      descriptor, and calls native nativeSetUsbFd(fd).
 *   4. The native capture path (hsdaoh_open on Android) reads that fd via
 *      android_usb_get_fd() and uses uvc_wrap() instead of
 *      uvc_find_device()+uvc_open().
 */
public class MainActivity extends NativeActivity {
    private static final String TAG = "MISRC";
    private static final String ACTION_USB_PERMISSION = "dev.misrc.gui.USB_PERMISSION";
    private static final int REQ_CAMERA = 0x5143;  // 'CA' -> CAMERA runtime request code

    // VID/PID of known hsdaoh devices (mirror third_party/hsdaoh known_devices).
    // 0x345f = 13407 (NOT 13343 — that was a decimal-conversion typo that
    // broke device matching, causing no chooser/prompt on plug-in).
    private static final int[][] KNOWN_DEVICES = {
        {0x345f, 0x2130},  // MS2130      (13407, 8496)
        {0x534d, 0x2130},  // MS2130 OEM  (21325, 8496)
        {0x345f, 0x2131},  // MS2131      (13407, 8497)
    };

    private UsbManager mUsbManager;
    private BroadcastReceiver mUsbReceiver;
    private boolean mReceiverRegistered = false;

    /** Native bridge: store the granted USB device file descriptor. */
    private native void nativeSetUsbFd(int fd);
    /** Native bridge: register this Activity so native can call
     * requestPermissionFromNative() when the user presses "connect". */
    private native void nativeRegisterActivity(android.app.Activity activity);
    /** Native bridge: signal the waiting native thread that permission
     * was granted or denied. */
    private native void nativeUsbPermissionResult(boolean granted);
    /** Native bridge: set the scoped-storage-exempt output directory
     * (getExternalFilesDir(null)) so native fopen() can write capture
     * logs/FLAC files to a retrievable path on Android 11+. */
    private native void nativeSetStoragePath(String path);

    @Override
    public void onCreate(android.os.Bundle savedInstanceState) {
        // CRITICAL ORDERING: request CAMERA (and queue USB permission) BEFORE
        // super.onCreate(). In a NativeActivity, super.onCreate() starts the
        // native thread which immediately calls InitWindow() and takes the
        // fullscreen surface. If we request permissions AFTER super.onCreate(),
        // the system permission dialog is fought over / covered by the native
        // fullscreen window, and the user sees no prompt. Requesting before
        // super.onCreate() queues the dialog so it shows first, then native
        // starts underneath it.
        mUsbManager = (UsbManager) getSystemService(Context.USB_SERVICE);
        // CRITICAL: hand native code the real scoped-storage-exempt output dir
        // (getExternalFilesDir(null)) BEFORE super.onCreate() starts the native
        // main(). On Android 11+, native fopen() can ONLY write here without
        // permission — the hardcoded /sdcard/... default is blocked by scoped
        // storage, so without this the capture log + FLAC files never land
        // anywhere retrievable and the user can't test. adb pull this dir to
        // retrieve captures: /sdcard/Android/data/dev.misrc.gui/files/
        java.io.File extDir = getExternalFilesDir(null);
        if (extDir != null) {
            String path = extDir.getAbsolutePath();
            Log.i(TAG, "App external files dir (capture output): " + path);
            try {
                nativeSetStoragePath(path);
            } catch (UnsatisfiedLinkError e) {
                Log.e(TAG, "nativeSetStoragePath unavailable: " + e.getMessage());
            }
        }
        try {
            registerUsbReceiver();
        } catch (Exception e) {
            Log.e(TAG, "USB receiver setup failed (app will still open): " + e.getMessage());
        }
        requestCameraPermissionThenUsb();
        // NOW start native (loads libmisrc_gui.so, runs android_main -> main()).
        super.onCreate(savedInstanceState);
        // Register ourselves with native so the native connect path can call
        // back into requestPermissionFromNative() to show the USB dialog. Wrap
        // in try/catch: if the JNI binding is unavailable for any reason, the
        // app MUST still open (NativeActivity's own native main runs
        // independently of these methods). USB capture just won't work.
        try {
            nativeRegisterActivity(this);
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "nativeRegisterActivity unavailable: " + e.getMessage());
        }
    }

    /** Request the dangerous CAMERA permission at runtime (needed before
     * UsbManager will show a USB-permission dialog for a UVC device). */
    private void requestCameraPermissionThenUsb() {
        if (checkSelfPermission(android.Manifest.permission.CAMERA)
                == PackageManager.PERMISSION_GRANTED) {
            Log.i(TAG, "CAMERA permission already granted; requesting USB permission");
            requestPermissionForKnownDevices();
            return;
        }
        Log.i(TAG, "Requesting CAMERA permission (required for UVC USB access on API 28+)");
        requestPermissions(new String[]{android.Manifest.permission.CAMERA}, REQ_CAMERA);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_CAMERA) {
            boolean granted = grantResults.length > 0
                    && grantResults[0] == PackageManager.PERMISSION_GRANTED;
            Log.i(TAG, "CAMERA permission result: granted=" + granted);
            // Whether or not CAMERA was granted, attempt the USB permission
            // request — if CAMERA was denied, UsbManager may still refuse, but
            // at least the user sees we tried and can retry from settings.
            requestPermissionForKnownDevices();
        }
    }

    @Override
    public void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if ("android.hardware.usb.action.USB_DEVICE_ATTACHED".equals(intent.getAction())) {
            requestPermissionForKnownDevices();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Re-request CAMERA (if previously denied) then USB permission, and
        // hand fd for any device already permitted. Covers the case where the
        // user granted CAMERA in settings while the app was backgrounded.
        requestCameraPermissionThenUsb();
        handFdForAlreadyPermittedDevices();
    }

    private boolean isKnownDevice(UsbDevice device) {
        int vid = device.getVendorId();
        int pid = device.getProductId();
        for (int[] kp : KNOWN_DEVICES) {
            if (kp[0] == vid && kp[1] == pid) return true;
        }
        return false;
    }

    private void registerUsbReceiver() {
        if (mReceiverRegistered) return;
        mUsbReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                String action = intent.getAction();
                if (ACTION_USB_PERMISSION.equals(action)) {
                    UsbDevice device = (UsbDevice)
                            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
                    boolean granted = intent.getBooleanExtra(
                            UsbManager.EXTRA_PERMISSION_GRANTED, false);
                    if (granted && device != null) {
                        openUsbDeviceAndHandFd(device);
                    }
                    // Signal the native thread that may be blocked in
                    // android_request_usb_permission() waiting on the result.
                    try {
                        nativeUsbPermissionResult(granted);
                    } catch (UnsatisfiedLinkError e) {
                        Log.e(TAG, "nativeUsbPermissionResult unavailable: " + e.getMessage());
                    }
                }
            }
        };
        IntentFilter filter = new IntentFilter(ACTION_USB_PERMISSION);
        // CRITICAL: use RECEIVER_EXPORTED, not NOT_EXPORTED. The permission-
        // result broadcast is sent by the SYSTEM UsbService (a different
        // UID/process), not by our own app. On API 33+ (we target 34),
        // RECEIVER_NOT_EXPORTED blocks broadcasts from other UIDs, so even if
        // the dialog appeared and the user granted, the result never reaches
        // nativeUsbPermissionResult() -> native times out -> "permission
        // denied". The canonical Google CTS Verifier USB impl uses
        // RECEIVER_EXPORTED for this same receiver. This matches that.
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(mUsbReceiver, filter, Context.RECEIVER_EXPORTED);
        } else {
            registerReceiver(mUsbReceiver, filter);
        }
        mReceiverRegistered = true;
    }

    /** Called from native (android_request_usb_permission) when the user
     * presses "connect" in the GUI and no fd has been granted yet. Shows
     * the system USB permission dialog for the attached hsdaoh device. */
    public void requestPermissionFromNative() {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                requestPermissionForKnownDevices();
            }
        });
    }

    /** Request USB permission for every known hsdaoh device currently attached. */
    private void requestPermissionForKnownDevices() {
        if (mUsbManager == null) { Log.e(TAG, "requestPermission: no UsbManager"); return; }
        HashMap<String, UsbDevice> deviceList = mUsbManager.getDeviceList();
        if (deviceList == null) { Log.e(TAG, "requestPermission: deviceList null"); return; }
        Log.i(TAG, "requestPermission: " + deviceList.size() + " USB device(s) attached");
        for (Map.Entry<String, UsbDevice> entry : deviceList.entrySet()) {
            UsbDevice device = entry.getValue();
            Log.i(TAG, "  USB dev: " + device.getDeviceName()
                    + " VID=0x" + Integer.toHexString(device.getVendorId())
                    + " PID=0x" + Integer.toHexString(device.getProductId())
                    + (isKnownDevice(device) ? " (KNOWN)" : " (not hsdaoh)"));
            if (!isKnownDevice(device)) continue;
            if (mUsbManager.hasPermission(device)) {
                openUsbDeviceAndHandFd(device);
            } else {
                int flags = PendingIntent.FLAG_UPDATE_CURRENT;
                if (Build.VERSION.SDK_INT >= 31) flags |= PendingIntent.FLAG_MUTABLE;
                // CRITICAL: the intent MUST be explicit (set our package) or
                // Android 12+ (API 31+, we target 34) blocks it as an implicit
                // PendingIntent — the permission-result broadcast then never
                // fires and nativeUsbPermissionResult() is never called.
                Intent permIntent = new Intent(ACTION_USB_PERMISSION);
                permIntent.setPackage(getPackageName());
                PendingIntent pi = PendingIntent.getBroadcast(
                        this, 0, permIntent, flags);
                mUsbManager.requestPermission(device, pi);
                Log.i(TAG, "Requested USB permission for " + device.getDeviceName()
                        + " VID=" + device.getVendorId() + " PID=" + device.getProductId());
            }
        }
    }

    /** Hand the fd to native for any known device we already have permission for. */
    private void handFdForAlreadyPermittedDevices() {
        if (mUsbManager == null) return;
        HashMap<String, UsbDevice> deviceList = mUsbManager.getDeviceList();
        if (deviceList == null) return;
        for (Map.Entry<String, UsbDevice> entry : deviceList.entrySet()) {
            UsbDevice device = entry.getValue();
            if (isKnownDevice(device) && mUsbManager.hasPermission(device)) {
                openUsbDeviceAndHandFd(device);
            }
        }
    }

    private void openUsbDeviceAndHandFd(UsbDevice device) {
        UsbDeviceConnection connection = mUsbManager.openDevice(device);
        if (connection == null) {
            Log.e(TAG, "openDevice() returned null for " + device.getDeviceName());
            return;
        }
        int fd = connection.getFileDescriptor();
        Log.i(TAG, "USB granted for " + device.getDeviceName() + ", fd=" + fd);
        if (fd >= 0) {
            try {
                nativeSetUsbFd(fd);
            } catch (UnsatisfiedLinkError e) {
                Log.e(TAG, "nativeSetUsbFd unavailable: " + e.getMessage());
            }
        }
        // Keep the connection alive: native code (libusb) now owns the fd, but
        // Android requires the UsbDeviceConnection to stay reachable so the fd
        // remains valid. We intentionally do not close it here; it is closed when
        // the native side is done (hsdaoh_close / app teardown).
    }

    @Override
    protected void onDestroy() {
        if (mReceiverRegistered) {
            try { unregisterReceiver(mUsbReceiver); } catch (Exception ignored) {}
            mReceiverRegistered = false;
        }
        super.onDestroy();
    }
}
