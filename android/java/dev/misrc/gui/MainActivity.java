package dev.misrc.gui;

import android.app.NativeActivity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.util.Log;
import android.view.View;
import android.view.inputmethod.InputMethodManager;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
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
    private static final int REQ_MEDIA = 0x4d45;   // 'ME' -> media runtime request code
    private static final int REQ_PICK_OUTPUT_DIR = 0x4f44;  // 'OD'
    private static final int REQ_PICK_PLAYBACK_A = 0x5041;  // 'PA'
    private static final int REQ_PICK_PLAYBACK_B = 0x5042;  // 'PB'

    private static final int PICKER_KIND_OUTPUT_DIR = 1;
    private static final int PICKER_KIND_PLAYBACK_A = 2;
    private static final int PICKER_KIND_PLAYBACK_B = 3;

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
    private final HashMap<String, UsbDeviceConnection> mOpenConnections = new HashMap<>();
    private int mPendingPlaybackChannel = -1;

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
    /** Native bridge: resolve async Android picker responses in native code. */
    private native void nativePickerResult(int kind, boolean accepted, String path);

    @Override
    public void onCreate(android.os.Bundle savedInstanceState) {
        // Initialize Java-side USB services first.
        mUsbManager = (UsbManager) getSystemService(Context.USB_SERVICE);
        String startupStoragePath = null;
        java.io.File extDir = getExternalFilesDir(null);
        if (extDir != null) {
            startupStoragePath = extDir.getAbsolutePath();
            Log.i(TAG, "App external files dir (capture output): " + startupStoragePath);
        }
        try {
            registerUsbReceiver();
        } catch (Exception e) {
            Log.e(TAG, "USB receiver setup failed (app will still open): " + e.getMessage());
        }
        // Start NativeActivity (loads libmisrc_gui.so, runs android_main -> main()).
        super.onCreate(savedInstanceState);
        // Native symbols are guaranteed after super.onCreate(); push startup
        // storage path now so native fopen() uses app-private external storage.
        if (startupStoragePath != null) {
            try {
                nativeSetStoragePath(startupStoragePath);
            } catch (UnsatisfiedLinkError e) {
                Log.e(TAG, "nativeSetStoragePath unavailable: " + e.getMessage());
            }
        }
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
        // Request runtime permissions only after Activity/native startup is
        // complete so callback paths (nativeSetUsbFd/nativeUsbPermissionResult)
        // are live when the USB dialog result arrives.
        requestCameraPermissionThenUsb();
        requestMediaPermissionIfNeeded();
    }

    /** Request dangerous capture permissions at runtime before USB permission. */
    private void requestCameraPermissionThenUsb() {
        boolean cameraGranted = checkSelfPermission(android.Manifest.permission.CAMERA)
                == PackageManager.PERMISSION_GRANTED;
        boolean audioGranted = checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)
                == PackageManager.PERMISSION_GRANTED;
        if (cameraGranted && audioGranted) {
            Log.i(TAG, "Capture permissions already granted; requesting USB permission");
            requestPermissionForKnownDevices();
            return;
        }
        Log.i(TAG, "Requesting CAMERA + RECORD_AUDIO permissions before USB access");
        requestPermissions(new String[]{
                android.Manifest.permission.CAMERA,
                android.Manifest.permission.RECORD_AUDIO
        }, REQ_CAMERA);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_CAMERA) {
            boolean granted = grantResults.length > 0;
            for (int result : grantResults) {
                if (result != PackageManager.PERMISSION_GRANTED) {
                    granted = false;
                    break;
                }
            }
            Log.i(TAG, "Capture permissions result: granted=" + granted);
            // Whether or not all capture permissions were granted, still
            // attempt USB permission so retry behavior stays deterministic.
            requestPermissionForKnownDevices();
        } else if (requestCode == REQ_MEDIA) {
            boolean granted = grantResults.length > 0;
            for (int result : grantResults) {
                if (result != PackageManager.PERMISSION_GRANTED) {
                    granted = false;
                    break;
                }
            }
            Log.i(TAG, "Media permission result: granted=" + granted);
            if (!granted && mPendingPlaybackChannel >= 0) {
                int pendingChannel = mPendingPlaybackChannel;
                mPendingPlaybackChannel = -1;
                signalNativePickerResult(pickerKindForPlaybackChannel(pendingChannel), false, null);
                return;
            }
            if (granted && mPendingPlaybackChannel >= 0) {
                int pendingChannel = mPendingPlaybackChannel;
                mPendingPlaybackChannel = -1;
                launchPlaybackFilePicker(pendingChannel);
            }
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_PICK_OUTPUT_DIR) {
            handleOutputDirPickerResult(resultCode, data);
            return;
        }
        if (requestCode == REQ_PICK_PLAYBACK_A || requestCode == REQ_PICK_PLAYBACK_B) {
            int channel = (requestCode == REQ_PICK_PLAYBACK_B) ? 1 : 0;
            handlePlaybackPickerResult(channel, resultCode, data);
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

    private boolean isUvcClassDevice(UsbDevice device) {
        if (device == null) return false;
        if (device.getDeviceClass() == UsbConstants.USB_CLASS_VIDEO) return true;
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            android.hardware.usb.UsbInterface intf = device.getInterface(i);
            if (intf != null && intf.getInterfaceClass() == UsbConstants.USB_CLASS_VIDEO) {
                return true;
            }
        }
        return false;
    }

    private boolean isPermissionCandidate(UsbDevice device) {
        return isKnownDevice(device) || isUvcClassDevice(device);
    }

    private void signalNativeUsbPermissionResult(boolean granted) {
        try {
            nativeUsbPermissionResult(granted);
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "nativeUsbPermissionResult unavailable: " + e.getMessage());
        }
    }

    private void signalNativePickerResult(int kind, boolean accepted, String path) {
        try {
            nativePickerResult(kind, accepted, path);
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "nativePickerResult unavailable: " + e.getMessage());
        }
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
                    Log.i(TAG, "USB permission broadcast: granted=" + granted
                            + (device != null ? (" device=" + device.getDeviceName()) : " device=null"));
                    boolean fdReady = false;
                    if (granted && device != null) {
                        fdReady = openUsbDeviceAndHandFd(device);
                    }
                    // Signal the native thread that may be blocked in
                    // android_request_usb_permission() waiting on the result.
                    signalNativeUsbPermissionResult(fdReady);
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

    /** Called by native settings UI to choose output folder. */
    public void requestOutputFolderFromNative() {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                launchOutputFolderPicker();
            }
        });
    }

    /** Called by native settings UI to choose playback file for channel 0/1. */
    public void requestPlaybackFileFromNative(final int channel) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (channel != 0 && channel != 1) {
                    signalNativePickerResult(pickerKindForPlaybackChannel(channel), false, null);
                    return;
                }
                if (!hasMediaPermission()) {
                    mPendingPlaybackChannel = channel;
                    requestMediaPermissionIfNeeded();
                    return;
                }
                launchPlaybackFilePicker(channel);
            }
        });
    }

    /** Called by native text input handlers to show/hide soft keyboard. */
    public void setKeyboardVisibleFromNative(final boolean visible) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                View view = getWindow() != null ? getWindow().getDecorView() : null;
                if (view == null) return;
                InputMethodManager imm =
                        (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
                if (imm == null) return;
                if (visible) {
                    view.requestFocus();
                    boolean shown = imm.showSoftInput(view, InputMethodManager.SHOW_IMPLICIT);
                    if (!shown) {
                        imm.toggleSoftInput(InputMethodManager.SHOW_FORCED, 0);
                    }
                } else {
                    imm.hideSoftInputFromWindow(view.getWindowToken(), 0);
                }
            }
        });
    }

    /** Request USB permission for every known hsdaoh device currently attached. */
    private void requestPermissionForKnownDevices() {
        if (mUsbManager == null) { Log.e(TAG, "requestPermission: no UsbManager"); return; }
        HashMap<String, UsbDevice> deviceList = mUsbManager.getDeviceList();
        if (deviceList == null) { Log.e(TAG, "requestPermission: deviceList null"); return; }
        Log.i(TAG, "requestPermission: " + deviceList.size() + " USB device(s) attached");
        int candidateCount = 0;
        int requestCount = 0;
        int openedFdCount = 0;
        for (Map.Entry<String, UsbDevice> entry : deviceList.entrySet()) {
            UsbDevice device = entry.getValue();
            boolean known = isKnownDevice(device);
            boolean uvc = isUvcClassDevice(device);
            boolean candidate = isPermissionCandidate(device);
            Log.i(TAG, "  USB dev: " + device.getDeviceName()
                    + " VID=0x" + Integer.toHexString(device.getVendorId())
                    + " PID=0x" + Integer.toHexString(device.getProductId())
                    + " class=" + device.getDeviceClass()
                    + " ifaces=" + device.getInterfaceCount()
                    + (known ? " (KNOWN)" : (uvc ? " (UVC)" : " (ignored)")));
            if (!candidate) continue;
            candidateCount++;
            if (mUsbManager.hasPermission(device)) {
                if (openUsbDeviceAndHandFd(device)) {
                    openedFdCount++;
                }
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
                requestCount++;
                Log.i(TAG, "Requested USB permission for " + device.getDeviceName()
                        + " VID=" + device.getVendorId() + " PID=" + device.getProductId());
            }
        }
        if (openedFdCount > 0) {
            signalNativeUsbPermissionResult(true);
        } else if (candidateCount == 0) {
            Log.w(TAG, "No known/UVC USB capture candidates found; cannot show USB permission dialog");
            signalNativeUsbPermissionResult(false);
        } else if (requestCount == 0) {
            Log.w(TAG, "Candidate USB devices found, but no permission request/open path was taken");
            signalNativeUsbPermissionResult(false);
        }
    }

    /** Hand the fd to native for any candidate device we already have permission for. */
    private void handFdForAlreadyPermittedDevices() {
        if (mUsbManager == null) return;
        HashMap<String, UsbDevice> deviceList = mUsbManager.getDeviceList();
        if (deviceList == null) return;
        for (Map.Entry<String, UsbDevice> entry : deviceList.entrySet()) {
            UsbDevice device = entry.getValue();
            if (isPermissionCandidate(device) && mUsbManager.hasPermission(device)) {
                openUsbDeviceAndHandFd(device);
            }
        }
    }

    private boolean openUsbDeviceAndHandFd(UsbDevice device) {
        if (device == null || mUsbManager == null) return false;
        String key = device.getDeviceName();
        UsbDeviceConnection connection = mOpenConnections.get(key);
        if (connection == null) {
            connection = mUsbManager.openDevice(device);
        }
        if (connection == null) {
            Log.e(TAG, "openDevice() returned null for " + device.getDeviceName());
            return false;
        }
        mOpenConnections.put(key, connection);

        int fd = connection.getFileDescriptor();
        if (fd < 0) {
            try { connection.close(); } catch (Exception ignored) {}
            mOpenConnections.remove(key);
            connection = mUsbManager.openDevice(device);
            if (connection == null) {
                Log.e(TAG, "openDevice() retry failed for " + device.getDeviceName());
                return false;
            }
            mOpenConnections.put(key, connection);
            fd = connection.getFileDescriptor();
        }

        Log.i(TAG, "USB granted for " + device.getDeviceName() + ", fd=" + fd);
        if (fd >= 0) {
            try {
                nativeSetUsbFd(fd);
                return true;
            } catch (UnsatisfiedLinkError e) {
                Log.e(TAG, "nativeSetUsbFd unavailable: " + e.getMessage());
            }
        }
        return false;
        // Keep the connection alive: native code (libusb) now owns the fd, but
        // Android requires the UsbDeviceConnection to stay reachable so the fd
        // remains valid. We intentionally do not close it here; it is closed when
        // the native side is done (hsdaoh_close / app teardown).
    }

    private int pickerKindForPlaybackChannel(int channel) {
        return channel == 1 ? PICKER_KIND_PLAYBACK_B : PICKER_KIND_PLAYBACK_A;
    }

    private String[] mediaPermissionList() {
        if (Build.VERSION.SDK_INT >= 33) {
            return new String[]{android.Manifest.permission.READ_MEDIA_AUDIO};
        }
        return new String[]{android.Manifest.permission.READ_EXTERNAL_STORAGE};
    }

    private boolean hasMediaPermission() {
        String[] perms = mediaPermissionList();
        for (String perm : perms) {
            if (checkSelfPermission(perm) != PackageManager.PERMISSION_GRANTED) {
                return false;
            }
        }
        return true;
    }

    private void requestMediaPermissionIfNeeded() {
        if (hasMediaPermission()) return;
        Log.i(TAG, "Requesting media read permission for Android file picker");
        requestPermissions(mediaPermissionList(), REQ_MEDIA);
    }

    private void launchOutputFolderPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, REQ_PICK_OUTPUT_DIR);
    }

    private void launchPlaybackFilePicker(int channel) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("audio/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"audio/flac", "audio/x-flac"});
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, channel == 1 ? REQ_PICK_PLAYBACK_B : REQ_PICK_PLAYBACK_A);
    }

    private void handleOutputDirPickerResult(int resultCode, Intent data) {
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            signalNativePickerResult(PICKER_KIND_OUTPUT_DIR, false, null);
            return;
        }
        Uri uri = data.getData();
        persistGrantedUriPermissions(data, uri);

        String path = resolveWritablePathFromTreeUri(uri);
        if (path == null) {
            File ext = getExternalFilesDir(null);
            if (ext != null) {
                path = ext.getAbsolutePath();
                Log.w(TAG, "Selected folder is not directly writable by native fopen(); using app folder: " + path);
            }
        }
        signalNativePickerResult(PICKER_KIND_OUTPUT_DIR, path != null && !path.isEmpty(), path);
    }

    private void handlePlaybackPickerResult(int channel, int resultCode, Intent data) {
        int kind = pickerKindForPlaybackChannel(channel);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            signalNativePickerResult(kind, false, null);
            return;
        }
        Uri uri = data.getData();
        persistGrantedUriPermissions(data, uri);
        String copiedPath = copyDocumentToAppPlaybackFile(uri, channel);
        signalNativePickerResult(kind, copiedPath != null && !copiedPath.isEmpty(), copiedPath);
    }

    private void persistGrantedUriPermissions(Intent data, Uri uri) {
        if (data == null || uri == null) return;
        int flags = data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        int persistFlags = flags & (Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        if (persistFlags == 0) return;
        try {
            getContentResolver().takePersistableUriPermission(uri, persistFlags);
        } catch (SecurityException e) {
            Log.w(TAG, "Persist URI permission failed: " + e.getMessage());
        }
    }

    private String resolveWritablePathFromTreeUri(Uri treeUri) {
        if (treeUri == null) return null;
        String docId;
        try {
            docId = DocumentsContract.getTreeDocumentId(treeUri);
        } catch (Exception e) {
            Log.w(TAG, "Unable to parse tree URI: " + e.getMessage());
            return null;
        }
        if (docId == null || docId.isEmpty()) return null;

        String[] parts = docId.split(":", 2);
        if (parts.length == 0) return null;

        String volume = parts[0];
        String rel = parts.length > 1 ? parts[1] : "";
        String root;
        if ("primary".equalsIgnoreCase(volume)) {
            root = Environment.getExternalStorageDirectory().getAbsolutePath();
        } else if (volume != null && !volume.isEmpty()) {
            root = "/storage/" + volume;
        } else {
            return null;
        }
        String fullPath = rel.isEmpty() ? root : root + "/" + rel;
        return isDirectPathWritable(fullPath) ? fullPath : null;
    }

    private boolean isDirectPathWritable(String path) {
        try {
            File dir = new File(path);
            if (!dir.exists() || !dir.isDirectory()) return false;
            if (!dir.canWrite()) return false;
            File probe = new File(dir, ".misrc_probe_" + System.currentTimeMillis());
            FileOutputStream fos = new FileOutputStream(probe);
            fos.write(0);
            fos.close();
            //noinspection ResultOfMethodCallIgnored
            probe.delete();
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    private String copyDocumentToAppPlaybackFile(Uri uri, int channel) {
        if (uri == null) return null;
        File baseDir = getExternalFilesDir(null);
        if (baseDir == null) baseDir = getFilesDir();
        if (baseDir == null) return null;

        File playbackDir = new File(baseDir, "playback_imports");
        if (!playbackDir.exists() && !playbackDir.mkdirs()) {
            Log.e(TAG, "Failed to create playback import directory: " + playbackDir.getAbsolutePath());
            return null;
        }

        String name = (channel == 1 ? "playback_b_" : "playback_a_")
                + System.currentTimeMillis() + ".flac";
        File outFile = new File(playbackDir, name);
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(outFile)) {
            if (in == null) return null;
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
            out.flush();
            Log.i(TAG, "Imported playback file to " + outFile.getAbsolutePath());
            return outFile.getAbsolutePath();
        } catch (Exception e) {
            Log.e(TAG, "Playback import failed: " + e.getMessage());
            return null;
        }
    }

    @Override
    protected void onDestroy() {
        for (Map.Entry<String, UsbDeviceConnection> entry : mOpenConnections.entrySet()) {
            UsbDeviceConnection conn = entry.getValue();
            if (conn != null) {
                try { conn.close(); } catch (Exception ignored) {}
            }
        }
        mOpenConnections.clear();
        if (mReceiverRegistered) {
            try { unregisterReceiver(mUsbReceiver); } catch (Exception ignored) {}
            mReceiverRegistered = false;
        }
        super.onDestroy();
    }
}
