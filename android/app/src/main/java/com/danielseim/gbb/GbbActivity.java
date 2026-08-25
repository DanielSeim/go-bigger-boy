package com.danielseim.gbb;

import android.content.Intent;
import android.hardware.SensorManager;
import android.os.Bundle;
import android.view.OrientationEventListener;

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

    private static native void nativeOpenRom(String rom, String displayName);

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
        if (cameraOrientationListener != null &&
                cameraOrientationListener.canDetectOrientation()) {
            cameraOrientationListener.enable();
        }
        if (updateManager != null) updateManager.onResume();
    }

    @Override
    protected void onPause() {
        if (updateManager != null) updateManager.onPause();
        if (cameraOrientationListener != null) cameraOrientationListener.disable();
        super.onPause();
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

    /** Opens the native library while preserving the running game underneath. */
    public void openLibrary() {
        startActivity(new Intent(this, LibraryActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT));
    }
}
