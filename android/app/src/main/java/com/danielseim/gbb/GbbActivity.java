package com.danielseim.gbb;

import android.annotation.SuppressLint;
import android.content.Intent;
import android.hardware.SensorManager;
import android.os.Build;
import android.os.Bundle;
import android.view.OrientationEventListener;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

import org.libsdl.app.SDLActivity;

/** Android entry point; SDLActivity owns the native surface and lifecycle. */
public final class GbbActivity extends SDLActivity {
    public static final String EXTRA_ROM = "com.danielseim.gbb.ROM";
    public static final String EXTRA_ROM_NAME = "com.danielseim.gbb.ROM_NAME";
    static final String ACTION_INSTALL_RESULT =
            "com.danielseim.gbb.INSTALL_UPDATE_RESULT";

    private AndroidUpdateManager updateManager;
    private volatile int cameraOrientationDegrees;
    private OrientationEventListener cameraOrientationListener;
    private OnBackInvokedCallback backCallback;

    private static native void nativeOpenRom(String rom, String displayName);
    private static native void nativeAndroidBackPressed();

    @Override
    protected String[] getArguments() {
        final String rom = getIntent().getStringExtra(EXTRA_ROM);
        final String name = getIntent().getStringExtra(EXTRA_ROM_NAME);
        return rom == null || rom.isEmpty() ? new String[0]
                : new String[]{rom, name == null ? "" : name};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        hideSystemBars();
        // OrientationEventListener angles run opposite to display rotations.
        cameraOrientationDegrees =
                (360 - SDLActivity.getCurrentRotation()) % 360;
        cameraOrientationListener = new OrientationEventListener(
                this, SensorManager.SENSOR_DELAY_NORMAL) {
            @Override
            public void onOrientationChanged(int orientation) {
                if (orientation == ORIENTATION_UNKNOWN) return;
                cameraOrientationDegrees = ((orientation + 45) / 90 % 4) * 90;
            }
        };
        updateManager = new AndroidUpdateManager(this);
        updateManager.checkForUpdates();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            backCallback = this::requestNativeBack;
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    OnBackInvokedDispatcher.PRIORITY_DEFAULT, backCallback);
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        final String rom = intent.getStringExtra(EXTRA_ROM);
        final String name = intent.getStringExtra(EXTRA_ROM_NAME);
        if (rom != null && !rom.isEmpty()) {
            nativeOpenRom(rom, name == null ? "" : name);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
        if (cameraOrientationListener != null &&
                cameraOrientationListener.canDetectOrientation()) {
            cameraOrientationListener.enable();
        }
        if (updateManager != null) updateManager.onResume();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    private void hideSystemBars() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            final WindowInsetsController controller =
                    getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
            return;
        }
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
                View.SYSTEM_UI_FLAG_FULLSCREEN |
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    @Override
    protected void onPause() {
        if (updateManager != null) updateManager.onPause();
        if (cameraOrientationListener != null) cameraOrientationListener.disable();
        super.onPause();
    }

    /**
     * Keep Android's back action on the SDL main thread.  SDL's native loop
     * flushes battery-backed RAM (including Game Boy Camera images) before it
     * displays the exit confirmation or shuts down.
     */
    @Override
    @SuppressLint("GestureBackNavigation")
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        requestNativeBack();
    }

    private void requestNativeBack() {
        nativeAndroidBackPressed();
    }

    @Override
    protected void onDestroy() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
                backCallback != null) {
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(
                    backCallback);
            backCallback = null;
        }
        super.onDestroy();
    }

    /**
     * Queried by the native camera path while the SDL window stays landscape.
     * The orientation sensor increases counter to the display-rotation API,
     * so convert it before comparing it with SDL's current display rotation.
     */
    public int getCameraOrientationCorrectionDegrees() {
        final int physicalDisplayRotation =
                (360 - cameraOrientationDegrees) % 360;
        return physicalDisplayRotation - SDLActivity.getCurrentRotation();
    }

    /** Opens the native library, optionally keeping the running game underneath. */
    public void openLibrary(boolean returnToGame) {
        startActivity(new Intent(this, LibraryActivity.class)
                .putExtra(LibraryActivity.EXTRA_RETURN_TO_GAME, returnToGame)
                .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT));
    }
}
