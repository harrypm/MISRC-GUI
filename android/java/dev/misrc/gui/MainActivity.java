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
import android.provider.Settings;
import android.text.Editable;
import android.text.InputType;
import android.util.Log;
import android.view.KeyEvent;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.Toast;
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

    /* CRITICAL: NativeActivity loads libmisrc_gui.so internally with dlopen
     * (loadNativeCode) for ANativeActivity_onCreate, but that path does NOT
     * call JNI_OnLoad and does NOT register the library with this class's
     * classloader. Without an explicit System.loadLibrary() here:
     *   - every `native` method on this class throws UnsatisfiedLinkError
     *     (nativeSetUsbFd / nativeRegisterActivity / nativeUsbPermissionResult /
     *     nativePickerResult / nativePushTextInput / nativeSetStoragePath), and
     *   - JNI_OnLoad never runs, so the native side has no JavaVM and the
     *     keyboard / picker / USB-permission bridges are completely dead.
     * Load order: libhsdaoh.so first (libmisrc_gui.so links against it). */
    static {
        try {
            System.loadLibrary("hsdaoh");
        } catch (Throwable t) {
            Log.e("MISRC", "System.loadLibrary(hsdaoh) failed: " + t.getMessage());
        }
        try {
            System.loadLibrary("misrc_gui");
            Log.i("MISRC", "System.loadLibrary(misrc_gui) OK (JNI bridge bound)");
        } catch (Throwable t) {
            Log.e("MISRC", "System.loadLibrary(misrc_gui) failed: " + t.getMessage());
        }
    }
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
    private NativeTextInputEditText mNativeTextInputView;

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
    /** Native bridge: forward Java text input into native UI text fields. */
    private native void nativePushTextInput(String text);
    /** Native bridge: report whether a known/UVC USB capture device is
     * physically attached, so native device enumeration only lists the
     * MS2130 entry when it is really present (the hsdaoh Android patch
     * cannot enumerate /dev/bus/usb without root, so Java owns presence). */
    private native void nativeSetUsbDevicePresent(boolean present);

    /** Hide the Android system bars; swipes can temporarily reveal them. */
    private void enforceImmersiveMode() {
        WindowInsetsController controller = getWindow().getInsetsController();
        if (controller != null) {
            controller.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            controller.hide(WindowInsets.Type.systemBars());
        }
    }

    /** Full-width immersive window. The native UI is no longer globally
     * shrunk/translated by display-cutout insets. Keep crucial controls away
     * from the camera's *physical* cutout in a future localized layout pass. */
    private void configureFullscreenDisplay() {
        WindowManager.LayoutParams params = getWindow().getAttributes();
        params.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        getWindow().setAttributes(params);
        getWindow().setDecorFitsSystemWindows(false);
        enforceImmersiveMode();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            enforceImmersiveMode();
        }
    }

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
        configureFullscreenDisplay();
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
        ensureNativeTextInputView();
        updateUsbDevicePresence();
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
        enforceImmersiveMode();
        try {
            nativeRegisterActivity(this);
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "nativeRegisterActivity unavailable on resume: " + e.getMessage());
        }
        // Re-request CAMERA (if previously denied) then USB permission, and
        // hand fd for any device already permitted. Covers the case where the
        // user granted CAMERA in settings while the app was backgrounded.
        updateUsbDevicePresence();
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
        // Keep UVC fallback for MS2130/MS2131 variants that present with
        // non-listed VID/PID tuples. requestPermissionForKnownDevices()
        // prefers known devices first, then falls back to UVC only when
        // no known device is currently attached.
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

    private void clearNativeUsbFd() {
        try {
            nativeSetUsbFd(-1);
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "nativeSetUsbFd(-1) unavailable: " + e.getMessage());
        }
    }

    /** Scan the USB device list and tell native whether a capture candidate
     * (known VID/PID or UVC-class) is physically attached right now. */
    private void updateUsbDevicePresence() {
        boolean present = false;
        if (mUsbManager != null) {
            HashMap<String, UsbDevice> deviceList = mUsbManager.getDeviceList();
            if (deviceList != null) {
                for (Map.Entry<String, UsbDevice> entry : deviceList.entrySet()) {
                    if (isPermissionCandidate(entry.getValue())) {
                        present = true;
                        break;
                    }
                }
            }
        }
        try {
            nativeSetUsbDevicePresent(present);
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "nativeSetUsbDevicePresent unavailable: " + e.getMessage());
        }
        Log.i(TAG, "USB capture device present: " + present);
    }

    private void closeUsbConnectionForDeviceName(String deviceName) {
        if (deviceName == null || deviceName.isEmpty()) return;
        UsbDeviceConnection conn = mOpenConnections.remove(deviceName);
        if (conn != null) {
            try { conn.close(); } catch (Exception ignored) {}
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
                    Toast.makeText(MainActivity.this,
                            "USB permission: granted=" + granted + " fdReady=" + fdReady,
                            Toast.LENGTH_LONG).show();
                    // Signal the native thread that may be blocked in
                    // android_request_usb_permission() waiting on the result.
                    signalNativeUsbPermissionResult(fdReady);
                } else if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(action)) {
                    UsbDevice device = (UsbDevice)
                            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
                    updateUsbDevicePresence();
                    if (device != null && isPermissionCandidate(device)) {
                        Log.i(TAG, "USB device attached: " + device.getDeviceName());
                        requestPermissionForKnownDevices();
                    }
                } else if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(action)) {
                    UsbDevice device = (UsbDevice)
                            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
                    updateUsbDevicePresence();
                    if (device != null && isPermissionCandidate(device)) {
                        Log.i(TAG, "USB device detached: " + device.getDeviceName());
                        closeUsbConnectionForDeviceName(device.getDeviceName());
                        clearNativeUsbFd();
                        signalNativeUsbPermissionResult(false);
                    }
                }
            }
        };
        IntentFilter filter = new IntentFilter(ACTION_USB_PERMISSION);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
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
    private void pushNativeTextToInput(String text) {
        if (text == null || text.isEmpty()) return;
        try {
            nativePushTextInput(text);
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "nativePushTextInput unavailable: " + e.getMessage());
        }
    }

    private void pushNativeBackspaces(int count) {
        if (count <= 0) return;
        StringBuilder sb = new StringBuilder(count);
        for (int i = 0; i < count; i++) {
            sb.append('\b');
        }
        pushNativeTextToInput(sb.toString());
    }

    private void ensureNativeTextInputView() {
        if (mNativeTextInputView != null) return;
        if (getWindow() == null) return;
        View decor = getWindow().getDecorView();
        if (!(decor instanceof ViewGroup)) return;

        NativeTextInputEditText input = new NativeTextInputEditText(this);
        input.setLayoutParams(new ViewGroup.LayoutParams(1, 1));
        input.setAlpha(0.0f);
        input.setCursorVisible(false);
        input.setBackground(null);
        input.setLongClickable(false);
        input.setTextIsSelectable(false);
        input.setSingleLine(true);
        input.setFocusable(true);
        input.setFocusableInTouchMode(true);
        input.setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN);
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        ((ViewGroup) decor).addView(input);
        mNativeTextInputView = input;
    }

    private final class NativeTextInputConnection extends BaseInputConnection {
        private String mComposingText = "";

        NativeTextInputConnection(View targetView) {
            super(targetView, true);
        }

        void resetState() {
            mComposingText = "";
        }

        private void pushDiff(String previous, String next) {
            String oldText = previous != null ? previous : "";
            String newText = next != null ? next : "";
            int max = Math.min(oldText.length(), newText.length());
            int common = 0;
            while (common < max && oldText.charAt(common) == newText.charAt(common)) {
                common++;
            }
            int removeCount = oldText.length() - common;
            if (removeCount > 0) {
                pushNativeBackspaces(removeCount);
            }
            if (newText.length() > common) {
                pushNativeTextToInput(newText.substring(common));
            }
        }

        @Override
        public Editable getEditable() {
            return mNativeTextInputView != null ? mNativeTextInputView.getEditableText() : null;
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            String committed = text != null ? text.toString() : "";
            pushDiff(mComposingText, committed);
            mComposingText = "";
            Editable editable = getEditable();
            if (editable != null) editable.clear();
            return true;
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition) {
            String next = text != null ? text.toString() : "";
            pushDiff(mComposingText, next);
            mComposingText = next;
            return true;
        }

        @Override
        public boolean finishComposingText() {
            mComposingText = "";
            return true;
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
            if (beforeLength > 0) {
                pushNativeBackspaces(beforeLength);
                if (!mComposingText.isEmpty()) {
                    int keep = Math.max(0, mComposingText.length() - beforeLength);
                    mComposingText = mComposingText.substring(0, keep);
                }
            }
            return true;
        }

        @Override
        public boolean sendKeyEvent(KeyEvent event) {
            if (event != null && event.getAction() == KeyEvent.ACTION_DOWN) {
                if (event.getKeyCode() == KeyEvent.KEYCODE_DEL) {
                    pushNativeBackspaces(1);
                    if (!mComposingText.isEmpty()) {
                        mComposingText = mComposingText.substring(0, mComposingText.length() - 1);
                    }
                    return true;
                }
                if (event.getKeyCode() == KeyEvent.KEYCODE_ENTER) {
                    pushNativeTextToInput("\n");
                    mComposingText = "";
                    return true;
                }
                int unicode = event.getUnicodeChar();
                if (unicode > 0 && !Character.isISOControl(unicode)) {
                    pushNativeTextToInput(new String(Character.toChars(unicode)));
                    return true;
                }
            }
            return super.sendKeyEvent(event);
        }
    }

    private final class NativeTextInputEditText extends EditText {
        private NativeTextInputConnection mBridgeConnection;

        NativeTextInputEditText(Context context) {
            super(context);
        }

        void resetBridgeState() {
            Editable editable = getText();
            if (editable != null) editable.clear();
            if (mBridgeConnection != null) mBridgeConnection.resetState();
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
            outAttrs.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
            outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN;
            mBridgeConnection = new NativeTextInputConnection(this);
            return mBridgeConnection;
        }
    }

    /** Called by native text input handlers to show/hide soft keyboard. */
    public void setKeyboardVisibleFromNative(final boolean visible) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                InputMethodManager imm =
                        (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
                if (imm == null) return;

                ensureNativeTextInputView();
                View target = mNativeTextInputView;
                if (target == null) {
                    target = getCurrentFocus();
                    if (target == null && getWindow() != null) {
                        target = getWindow().getDecorView();
                    }
                    if (target == null) return;
                }

                if (visible) {
                    if (mNativeTextInputView != null) {
                        mNativeTextInputView.resetBridgeState();
                        mNativeTextInputView.requestFocus();
                        target = mNativeTextInputView;
                    } else {
                        target.setFocusable(true);
                        target.setFocusableInTouchMode(true);
                        target.requestFocus();
                    }
                    imm.restartInput(target);
                    boolean shown = imm.showSoftInput(target, InputMethodManager.SHOW_FORCED);
                    if (!shown) {
                        imm.toggleSoftInput(InputMethodManager.SHOW_FORCED, 0);
                    }
                } else {
                    imm.hideSoftInputFromWindow(target.getWindowToken(), 0);
                    if (mNativeTextInputView != null) {
                        mNativeTextInputView.resetBridgeState();
                        mNativeTextInputView.clearFocus();
                    }
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
        boolean hasKnownAttached = false;
        for (Map.Entry<String, UsbDevice> entry : deviceList.entrySet()) {
            if (isKnownDevice(entry.getValue())) {
                hasKnownAttached = true;
                break;
            }
        }
        if (!hasKnownAttached) {
            Log.w(TAG, "No known VID/PID device detected; falling back to UVC candidate matching");
        }
        int candidateCount = 0;
        int requestCount = 0;
        int openedFdCount = 0;
        for (Map.Entry<String, UsbDevice> entry : deviceList.entrySet()) {
            UsbDevice device = entry.getValue();
            boolean known = isKnownDevice(device);
            boolean uvc = isUvcClassDevice(device);
            boolean candidate = hasKnownAttached ? known : (known || uvc);
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
                Toast.makeText(this, "USB permission requested for " + device.getDeviceName(),
                        Toast.LENGTH_LONG).show();
            }
        }
        if (openedFdCount > 0) {
            signalNativeUsbPermissionResult(true);
        } else if (candidateCount == 0) {
            Log.w(TAG, hasKnownAttached
                    ? "Known USB capture candidate missing from permission/open flow"
                    : "No known/UVC USB capture candidates found; cannot show USB permission dialog");
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
        // Always reopen on handoff to avoid stale/reused descriptors after
        // previous native sessions close wrapped libusb handles.
        closeUsbConnectionForDeviceName(key);
        UsbDeviceConnection connection = mUsbManager.openDevice(device);
        if (connection == null) {
            Log.e(TAG, "openDevice() returned null for " + device.getDeviceName());
            return false;
        }

        int fd = connection.getFileDescriptor();
        if (fd < 0) {
            try { connection.close(); } catch (Exception ignored) {}
            Log.e(TAG, "openDevice() returned invalid fd for " + device.getDeviceName());
            return false;
        }
        mOpenConnections.put(key, connection);

        Log.i(TAG, "USB granted for " + device.getDeviceName() + ", fd=" + fd);
        Toast.makeText(this, "USB fd handed to native: " + fd, Toast.LENGTH_LONG).show();
        try {
            nativeSetUsbFd(fd);
            return true;
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "nativeSetUsbFd unavailable: " + e.getMessage());
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
        try {
            startActivityForResult(intent, REQ_PICK_OUTPUT_DIR);
        } catch (Exception e) {
            Log.e(TAG, "Failed to launch output folder picker: " + e.getMessage());
            signalNativePickerResult(PICKER_KIND_OUTPUT_DIR, false, null);
        }
    }

    private void launchPlaybackFilePicker(int channel) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("audio/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"audio/flac", "audio/x-flac"});
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        int requestCode = channel == 1 ? REQ_PICK_PLAYBACK_B : REQ_PICK_PLAYBACK_A;
        try {
            startActivityForResult(intent, requestCode);
        } catch (Exception e) {
            Log.e(TAG, "Failed to launch playback file picker: " + e.getMessage());
            signalNativePickerResult(pickerKindForPlaybackChannel(channel), false, null);
        }
    }

    private boolean hasAllFilesAccess() {
        if (Build.VERSION.SDK_INT >= 30) {
            return Environment.isExternalStorageManager();
        }
        return true;
    }

    /** Open the system "All files access" toggle for this app. Required on
     * Android 11+ for native fopen() to write into user-picked folders under
     * /storage/emulated/0 — the regular "Files and media" permission does NOT
     * grant direct filesystem writes under scoped storage. */
    private void requestAllFilesAccess() {
        if (Build.VERSION.SDK_INT < 30) return;
        try {
            Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivity(intent);
        } catch (Exception e) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            } catch (Exception e2) {
                Log.e(TAG, "Cannot open All files access settings: " + e2.getMessage());
            }
        }
    }

    private void handleOutputDirPickerResult(int resultCode, Intent data) {
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            Toast.makeText(this, "Folder picker: no result (resultCode=" + resultCode + ")", Toast.LENGTH_LONG).show();
            signalNativePickerResult(PICKER_KIND_OUTPUT_DIR, false, null);
            return;
        }
        Uri uri = data.getData();
        persistGrantedUriPermissions(data, uri);

        String path = resolveWritablePathFromTreeUri(uri);
        if (path == null && !hasAllFilesAccess()) {
            /* The picked folder exists but native fopen() can't write there
             * without "All files access" (scoped storage). Send the user to
             * the Settings toggle, then they re-pick the folder. */
            Toast.makeText(this,
                    "Folder needs 'All files access' — enable it for MISRC, then pick the folder again",
                    Toast.LENGTH_LONG).show();
            requestAllFilesAccess();
            signalNativePickerResult(PICKER_KIND_OUTPUT_DIR, false, null);
            return;
        }
        if (path == null) {
            File ext = getExternalFilesDir(null);
            if (ext != null) {
                path = ext.getAbsolutePath();
                Log.w(TAG, "Selected folder is not directly writable by native fopen(); using app folder: " + path);
                Toast.makeText(this, "Folder not writable; using app folder: " + path, Toast.LENGTH_LONG).show();
            }
        } else {
            Toast.makeText(this, "Folder picked: " + path, Toast.LENGTH_LONG).show();
        }
        signalNativePickerResult(PICKER_KIND_OUTPUT_DIR, path != null && !path.isEmpty(), path);
    }

    private void handlePlaybackPickerResult(int channel, int resultCode, Intent data) {
        int kind = pickerKindForPlaybackChannel(channel);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            Toast.makeText(this, "File picker: no result (ch=" + channel + ")", Toast.LENGTH_LONG).show();
            signalNativePickerResult(kind, false, null);
            return;
        }
        Uri uri = data.getData();
        persistGrantedUriPermissions(data, uri);
        String copiedPath = copyDocumentToAppPlaybackFile(uri, channel);
        Toast.makeText(this, "File imported: " + (copiedPath != null ? copiedPath : "(failed)"), Toast.LENGTH_LONG).show();
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
        clearNativeUsbFd();
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
        if (mNativeTextInputView != null) {
            try {
                ViewGroup parent = (ViewGroup) mNativeTextInputView.getParent();
                if (parent != null) {
                    parent.removeView(mNativeTextInputView);
                }
            } catch (Exception ignored) {}
            mNativeTextInputView = null;
        }
        super.onDestroy();
    }
}
