package com.danielseim.gbb;

import android.app.AlertDialog;
import android.app.ActivityOptions;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.hardware.SensorManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.util.Log;
import android.view.OrientationEventListener;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.Locale;

/** Android entry point; SDLActivity owns the native surface and lifecycle. */
public final class GbbActivity extends SDLActivity {
    public static final String EXTRA_ROM = "com.danielseim.gbb.ROM";
    private static final String TAG = "GBB updater";
    private static final String RELEASE_API =
            "https://api.github.com/repos/DanielSeim/go-bigger-boy/releases/latest";
    private static final String APK_ASSET = "go-bigger-boy-android.apk";
    private static final String ACTION_INSTALL_RESULT =
            "com.danielseim.gbb.INSTALL_UPDATE_RESULT";
    private static final long MAXIMUM_APK_SIZE = 256L * 1024L * 1024L;

    private File pendingUpdate;
    private boolean awaitingInstallPermission;
    private volatile int cameraOrientationDegrees;
    private OrientationEventListener cameraOrientationListener;

    private static native void nativeOpenRom(String rom);

    @Override
    protected String[] getArguments() {
        final String rom = getIntent().getStringExtra(EXTRA_ROM);
        return rom == null || rom.isEmpty() ? new String[0]
                                            : new String[]{rom};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // OrientationEventListener angles run opposite to display rotations.
        // Seed the sensor value from the display until the first event arrives.
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
        if (!handleInstallResult(getIntent())) checkForUpdates();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (!handleInstallResult(intent)) {
            final String rom = intent.getStringExtra(EXTRA_ROM);
            if (rom != null && !rom.isEmpty()) nativeOpenRom(rom);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (cameraOrientationListener != null &&
                cameraOrientationListener.canDetectOrientation()) {
            cameraOrientationListener.enable();
        }
        if (awaitingInstallPermission) {
            awaitingInstallPermission = false;
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O ||
                    getPackageManager().canRequestPackageInstalls()) {
                installPendingUpdate();
            } else {
                Toast.makeText(this, "Update installation was not enabled.",
                        Toast.LENGTH_LONG).show();
            }
        }
    }

    @Override
    protected void onPause() {
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

    private void checkForUpdates() {
        new Thread(() -> {
            try {
                final JSONObject release = readJson(RELEASE_API);
                final String latest = release.getString("tag_name");
                if (compareVersions(latest, installedVersion()) <= 0) return;

                final JSONArray assets = release.getJSONArray("assets");
                JSONObject apk = null;
                for (int index = 0; index < assets.length(); ++index) {
                    final JSONObject candidate = assets.getJSONObject(index);
                    if (APK_ASSET.equals(candidate.optString("name"))) {
                        apk = candidate;
                        break;
                    }
                }
                if (apk == null) {
                    Log.w(TAG, "Latest release has no Android APK");
                    return;
                }
                final String downloadUrl = apk.getString("browser_download_url");
                final String digest = apk.optString("digest");
                if (!digest.startsWith("sha256:") || digest.length() != 71) {
                    Log.w(TAG, "Latest Android APK has no SHA-256 digest");
                    return;
                }
                final String expectedSha256 = digest.substring(7);
                runOnUiThread(() -> offerUpdate(latest, downloadUrl,
                                                expectedSha256));
            } catch (Exception error) {
                // Updates are optional, so an offline startup remains quiet.
                Log.w(TAG, "Update check unavailable", error);
            }
        }, "gbb-update-check").start();
    }

    private void offerUpdate(String version, String url, String sha256) {
        if (isFinishing() || isDestroyed()) return;
        new AlertDialog.Builder(this)
                .setTitle("Go Bigger Boy update available")
                .setMessage("Go Bigger Boy " + version +
                        " is available. Download and install it now?")
                .setNegativeButton("Later", null)
                .setPositiveButton("Update now", (dialog, which) ->
                        downloadUpdate(version, url, sha256))
                .show();
    }

    private void downloadUpdate(String version, String url, String sha256) {
        Toast.makeText(this, "Downloading Go Bigger Boy " + version + "…",
                Toast.LENGTH_LONG).show();
        new Thread(() -> {
            try {
                final File directory = new File(getCacheDir(), "updates");
                if (!directory.exists() && !directory.mkdirs()) {
                    throw new IllegalStateException("Could not create update directory");
                }
                final File apk = new File(directory, APK_ASSET);
                download(url, apk);
                if (!sha256.equalsIgnoreCase(sha256(apk))) {
                    if (!apk.delete()) apk.deleteOnExit();
                    throw new SecurityException("Downloaded APK failed verification");
                }
                pendingUpdate = apk;
                runOnUiThread(this::requestUpdateInstallation);
            } catch (Exception error) {
                Log.e(TAG, "Update download failed", error);
                runOnUiThread(() -> new AlertDialog.Builder(this)
                        .setTitle("Update failed")
                        .setMessage("The update could not be downloaded and verified. " +
                                "Please try again later.")
                        .setPositiveButton("OK", null)
                        .show());
            }
        }, "gbb-update-download").start();
    }

    private void requestUpdateInstallation() {
        if (pendingUpdate == null || !pendingUpdate.isFile()) return;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O &&
                !getPackageManager().canRequestPackageInstalls()) {
            new AlertDialog.Builder(this)
                    .setTitle("Allow app updates")
                    .setMessage("Android must allow Go Bigger Boy to install its " +
                            "verified update. Enable ‘Allow from this source’, then return.")
                    .setNegativeButton("Cancel", null)
                    .setPositiveButton("Open settings", (dialog, which) -> {
                        awaitingInstallPermission = true;
                        final Intent settings = new Intent(
                                Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                                Uri.parse("package:" + getPackageName()));
                        startActivity(settings);
                    })
                    .show();
            return;
        }
        installPendingUpdate();
    }

    private void installPendingUpdate() {
        final File apk = pendingUpdate;
        pendingUpdate = null;
        if (apk == null || !apk.isFile()) return;
        new Thread(() -> stageAndCommitUpdate(apk), "gbb-update-install").start();
    }

    private void stageAndCommitUpdate(File apk) {
        PackageInstaller installer = null;
        int sessionId = -1;
        try {
            installer = getPackageManager().getPackageInstaller();
            final PackageInstaller.SessionParams parameters =
                    new PackageInstaller.SessionParams(
                            PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            parameters.setAppPackageName(getPackageName());
            sessionId = installer.createSession(parameters);
            try (PackageInstaller.Session session = installer.openSession(sessionId)) {
                try (InputStream input =
                             new BufferedInputStream(new FileInputStream(apk));
                     OutputStream output = session.openWrite(
                             "go-bigger-boy.apk", 0, apk.length())) {
                    final byte[] buffer = new byte[64 * 1024];
                    int count;
                    while ((count = input.read(buffer)) != -1) {
                        output.write(buffer, 0, count);
                    }
                    output.flush();
                    session.fsync(output);
                }

                final Intent result = new Intent(this, GbbActivity.class)
                        .setAction(ACTION_INSTALL_RESULT)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK |
                                  Intent.FLAG_ACTIVITY_CLEAR_TOP |
                                  Intent.FLAG_ACTIVITY_SINGLE_TOP);
                Bundle launchOptions = null;
                if (Build.VERSION.SDK_INT >= 34) {
                    launchOptions = ActivityOptions.makeBasic()
                            .setPendingIntentCreatorBackgroundActivityStartMode(
                                    ActivityOptions
                                            .MODE_BACKGROUND_ACTIVITY_START_ALLOWED)
                            .toBundle();
                }
                final PendingIntent pendingResult = PendingIntent.getActivity(
                        this, sessionId, result,
                        PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE,
                        launchOptions);
                session.commit(pendingResult.getIntentSender());
            }
        } catch (Exception error) {
            if (installer != null && sessionId != -1) {
                try {
                    installer.abandonSession(sessionId);
                } catch (Exception ignored) {
                    Log.w(TAG, "Could not abandon failed install session", ignored);
                }
            }
            pendingUpdate = apk;
            Log.e(TAG, "Could not start package installer", error);
            runOnUiThread(() -> new AlertDialog.Builder(this)
                    .setTitle("Update failed")
                    .setMessage("Android could not start the update installer. " +
                            "Please try again.")
                    .setNegativeButton("Cancel", null)
                    .setPositiveButton("Retry", (dialog, which) ->
                            installPendingUpdate())
                    .show());
        }
    }

    private boolean handleInstallResult(Intent intent) {
        if (intent == null || !ACTION_INSTALL_RESULT.equals(intent.getAction())) {
            return false;
        }
        final int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS,
                PackageInstaller.STATUS_FAILURE);
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            final Intent confirmation;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                confirmation = intent.getParcelableExtra(Intent.EXTRA_INTENT,
                                                         Intent.class);
            } else {
                //noinspection deprecation
                confirmation = intent.getParcelableExtra(Intent.EXTRA_INTENT);
            }
            if (confirmation != null) startActivity(confirmation);
        } else if (status != PackageInstaller.STATUS_SUCCESS) {
            final String message = intent.getStringExtra(
                    PackageInstaller.EXTRA_STATUS_MESSAGE);
            Log.e(TAG, "Package installation failed: " + message);
            Toast.makeText(this, "Go Bigger Boy update failed.",
                    Toast.LENGTH_LONG).show();
        } else {
            startActivity(new Intent(this, LibraryActivity.class)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK |
                              Intent.FLAG_ACTIVITY_CLEAR_TASK));
            finish();
        }
        return true;
    }

    private String installedVersion() throws Exception {
        final PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
        return info.versionName == null ? "0.0.0" : info.versionName;
    }

    private static int compareVersions(String left, String right) {
        final int[] a = parseVersion(left);
        final int[] b = parseVersion(right);
        for (int index = 0; index < a.length; ++index) {
            if (a[index] != b[index]) return Integer.compare(a[index], b[index]);
        }
        return 0;
    }

    private static int[] parseVersion(String value) {
        value = value.startsWith("v") || value.startsWith("V")
                ? value.substring(1) : value;
        final String[] pieces = value.split("[.+-]", 4);
        if (pieces.length < 3) throw new IllegalArgumentException("Invalid version");
        return new int[]{Integer.parseInt(pieces[0]), Integer.parseInt(pieces[1]),
                         Integer.parseInt(pieces[2])};
    }

    private static JSONObject readJson(String url) throws Exception {
        final HttpURLConnection connection = open(url);
        connection.setConnectTimeout(5000);
        connection.setReadTimeout(5000);
        connection.setRequestProperty("Accept", "application/vnd.github+json");
        connection.setRequestProperty("User-Agent", "Go-Bigger-Boy/Android");
        try (InputStream input = new BufferedInputStream(connection.getInputStream())) {
            final byte[] data = readLimited(input, 256 * 1024);
            return new JSONObject(new String(data, java.nio.charset.StandardCharsets.UTF_8));
        } finally {
            connection.disconnect();
        }
    }

    private static void download(String url, File destination) throws Exception {
        final HttpURLConnection connection = open(url);
        connection.setConnectTimeout(10000);
        connection.setReadTimeout(30000);
        connection.setRequestProperty("User-Agent", "Go-Bigger-Boy/Android");
        // getContentLengthLong() is unavailable on Android 5 and 6, which GBB
        // still supports. Release APKs are well within the signed-int range.
        final long size = connection.getContentLength();
        if (size > MAXIMUM_APK_SIZE) throw new IllegalStateException("APK is too large");
        long written = 0;
        try (InputStream input = new BufferedInputStream(connection.getInputStream());
             OutputStream output = new BufferedOutputStream(
                     new FileOutputStream(destination, false))) {
            final byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) {
                written += count;
                if (written > MAXIMUM_APK_SIZE) {
                    throw new IllegalStateException("APK is too large");
                }
                output.write(buffer, 0, count);
            }
        } finally {
            connection.disconnect();
        }
    }

    private static HttpURLConnection open(String url) throws Exception {
        final HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
        connection.setInstanceFollowRedirects(true);
        return connection;
    }

    private static byte[] readLimited(InputStream input, int limit) throws Exception {
        final java.io.ByteArrayOutputStream output = new java.io.ByteArrayOutputStream();
        final byte[] buffer = new byte[4096];
        int total = 0;
        int count;
        while ((count = input.read(buffer)) != -1) {
            total += count;
            if (total > limit) throw new IllegalStateException("Response is too large");
            output.write(buffer, 0, count);
        }
        return output.toByteArray();
    }

    private static String sha256(File file) throws Exception {
        final MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream input = new BufferedInputStream(new FileInputStream(file))) {
            final byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) digest.update(buffer, 0, count);
        }
        final StringBuilder result = new StringBuilder(64);
        for (byte value : digest.digest()) {
            result.append(String.format(Locale.ROOT, "%02x", value & 0xff));
        }
        return result.toString();
    }
}
