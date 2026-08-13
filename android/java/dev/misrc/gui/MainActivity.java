package dev.misrc.gui;

import android.app.NativeActivity;

/**
 * Minimal Java entry point for the MISRC GUI Android build.
 *
 * Extends the framework NativeActivity, which loads libmisrc_gui.so and calls
 * ANativeActivity_onCreate (provided by raylib rcore_android.c). The GUI's
 * existing main() is then invoked via raylib's android_main().
 *
 * A real Java class (and thus a classes.dex) is required even though all the
 * work happens in native code: Android's package installer rejects APKs that
 * contain no classes.dex with "package appears to be invalid". This stub also
 * is the future hook for getExternalFilesDir()/USB-Host intent forwarding via
 * JNI (see android/ANDROID_SUPPORT_PLAN.md work breakdown #5).
 */
public class MainActivity extends NativeActivity {
}
