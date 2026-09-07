package com.danielseim.gbb;

import android.app.Activity;
import android.app.ActivityOptions;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.pm.InstallSourceInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.util.Log;
import android.widget.Toast;

import com.google.android.play.core.appupdate.AppUpdateInfo;
import com.google.android.play.core.appupdate.AppUpdateManager;
import com.google.android.play.core.appupdate.AppUpdateManagerFactory;
import com.google.android.play.core.appupdate.AppUpdateOptions;
import com.google.android.play.core.install.InstallStateUpdatedListener;
import com.google.android.play.core.install.model.AppUpdateType;
import com.google.android.play.core.install.model.InstallStatus;
import com.google.android.play.core.install.model.UpdateAvailability;

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

/** Checks for direct-download updates; Play Store builds use Play for updates. */
final class AndroidUpdateManager {
    private static final String TAG = "GBB updater";
    private static final String PLAY_STORE_PACKAGE = "com.android.vending";
    private static final String RELEASE_API =
            "https://api.github.com/repos/DanielSeim/go-bigger-boy/releases/latest";
    private static final String APK_ASSET = "go-bigger-boy-android.apk";
    private static final long MAXIMUM_APK_SIZE = 256L * 1024L * 1024L;
    private static boolean checkStarted;
    private static boolean playCheckStarted;

    private final Activity activity;
    private AppUpdateManager playUpdateManager;
    private InstallStateUpdatedListener playUpdateListener;
    private boolean playUpdateListenerRegistered;
    private boolean playUpdatePromptShown;
    private boolean playUpdateReadyPromptShown;
    private AppUpdateInfo pendingPlayUpdate;
    private File pendingUpdate;
    private boolean awaitingInstallPermission;
    private boolean resumed;
    private UpdateOffer pendingOffer;

    private static final class UpdateOffer {
        final String version;
        final String url;
        final String sha256;

        UpdateOffer(String version, String url, String sha256) {
            this.version = version;
            this.url = url;
            this.sha256 = sha256;
        }
    }

    AndroidUpdateManager(Activity activity) {
        this.activity = activity;
    }

    void checkForUpdates() {
        // Installer metadata can be populated or corrected after the activity
        // is created (for example when Play updates a previously sideloaded
        // copy), so evaluate it for every check rather than caching it in the
        // constructor.
        if (installedFromPlayStore()) {
            checkForPlayStoreUpdate();
            return;
        }
        synchronized (AndroidUpdateManager.class) {
            if (checkStarted) return;
            checkStarted = true;
        }
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
                activity.runOnUiThread(() -> {
                    final UpdateOffer offer = new UpdateOffer(
                            latest, downloadUrl, expectedSha256);
                    if (resumed) {
                        offerUpdate(offer);
                    } else {
                        pendingOffer = offer;
                    }
                });
            } catch (Exception error) {
                // Updates are optional, so an offline startup remains quiet.
                Log.w(TAG, "Update check unavailable", error);
            }
        }, "gbb-update-check").start();
    }

    void onResume() {
        resumed = true;
        registerPlayUpdateListener();
        refreshPlayUpdateState();
        if (pendingOffer != null) {
            final UpdateOffer offer = pendingOffer;
            pendingOffer = null;
            offerUpdate(offer);
        }
        if (pendingPlayUpdate != null) {
            final AppUpdateInfo update = pendingPlayUpdate;
            pendingPlayUpdate = null;
            offerPlayStoreUpdate(update);
        }
        if (!awaitingInstallPermission) return;
        awaitingInstallPermission = false;
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O ||
                activity.getPackageManager().canRequestPackageInstalls()) {
            installPendingUpdate();
        } else {
            Toast.makeText(activity, "Update installation was not enabled.",
                    Toast.LENGTH_LONG).show();
        }
    }

    void onPause() {
        unregisterPlayUpdateListener();
        resumed = false;
    }

    private void checkForPlayStoreUpdate() {
        synchronized (AndroidUpdateManager.class) {
            if (playCheckStarted) return;
            playCheckStarted = true;
        }
        ensurePlayUpdateManager();
        playUpdateManager.getAppUpdateInfo()
                .addOnSuccessListener(this::handlePlayUpdateInfo)
                .addOnFailureListener(error ->
                        Log.w(TAG, "Play Store update check unavailable", error));
    }

    private void ensurePlayUpdateManager() {
        if (playUpdateManager != null) return;
        playUpdateManager = AppUpdateManagerFactory.create(activity);
        playUpdateListener = state -> {
            if (state.installStatus() == InstallStatus.DOWNLOADED) {
                offerPlayStoreUpdateReady();
            }
        };
        registerPlayUpdateListener();
    }

    private void registerPlayUpdateListener() {
        if (playUpdateManager == null || playUpdateListenerRegistered) return;
        playUpdateManager.registerListener(playUpdateListener);
        playUpdateListenerRegistered = true;
    }

    private void unregisterPlayUpdateListener() {
        if (playUpdateManager == null || !playUpdateListenerRegistered) return;
        playUpdateManager.unregisterListener(playUpdateListener);
        playUpdateListenerRegistered = false;
    }

    private void refreshPlayUpdateState() {
        if (playUpdateManager == null) return;
        playUpdateManager.getAppUpdateInfo()
                .addOnSuccessListener(update -> {
                    if (update.installStatus() == InstallStatus.DOWNLOADED) {
                        offerPlayStoreUpdateReady();
                    } else {
                        handlePlayUpdateInfo(update);
                    }
                })
                .addOnFailureListener(error ->
                        Log.w(TAG, "Could not refresh Play Store update state", error));
    }

    private void handlePlayUpdateInfo(AppUpdateInfo update) {
        if (update.updateAvailability() ==
                UpdateAvailability.DEVELOPER_TRIGGERED_UPDATE_IN_PROGRESS) {
            if (resumed && update.isUpdateTypeAllowed(AppUpdateType.FLEXIBLE)) {
                startPlayStoreUpdate(update);
            } else {
                pendingPlayUpdate = update;
            }
            return;
        }
        if (update.updateAvailability() != UpdateAvailability.UPDATE_AVAILABLE ||
                !update.isUpdateTypeAllowed(AppUpdateType.FLEXIBLE)) {
            Log.i(TAG, "No flexible Play Store update available");
            return;
        }
        Log.i(TAG, "Play Store update available; prompting for flexible update");
        if (resumed) {
            offerPlayStoreUpdate(update);
        } else {
            pendingPlayUpdate = update;
        }
    }

    private void offerPlayStoreUpdate(AppUpdateInfo update) {
        if (playUpdatePromptShown || activity.isFinishing() || activity.isDestroyed()) return;
        playUpdatePromptShown = true;
        new AlertDialog.Builder(activity)
                .setTitle("Go Bigger Boy update available")
                .setMessage("A newer version is available on Google Play.")
                .setNegativeButton("Later", null)
                .setPositiveButton("Update", (dialog, which) -> startPlayStoreUpdate(update))
                .show();
    }

    private void startPlayStoreUpdate(AppUpdateInfo update) {
        ensurePlayUpdateManager();
        try {
            playUpdateManager.startUpdateFlowForResult(
                    update,
                    activity,
                    AppUpdateOptions.newBuilder(AppUpdateType.FLEXIBLE).build(),
                    0x4742);
        } catch (Exception error) {
            playUpdatePromptShown = false;
            Log.w(TAG, "Could not start Play Store update", error);
        }
    }

    private void offerPlayStoreUpdateReady() {
        if (playUpdateReadyPromptShown || !resumed ||
                activity.isFinishing() || activity.isDestroyed()) return;
        playUpdateReadyPromptShown = true;
        new AlertDialog.Builder(activity)
                .setTitle("Update downloaded")
                .setMessage("Restart Go Bigger Boy to install the update.")
                .setNegativeButton("Later", null)
                .setPositiveButton("Restart", (dialog, which) -> completePlayStoreUpdate())
                .show();
    }

    private void completePlayStoreUpdate() {
        if (playUpdateManager == null) return;
        playUpdateManager.completeUpdate()
                .addOnFailureListener(error -> {
                    playUpdateReadyPromptShown = false;
                    Log.w(TAG, "Could not complete Play Store update", error);
                    activity.runOnUiThread(() -> Toast.makeText(activity,
                            "The update could not be installed yet.", Toast.LENGTH_LONG).show());
                });
    }

    private void offerUpdate(UpdateOffer offer) {
        if (activity.isFinishing() || activity.isDestroyed()) return;
        new AlertDialog.Builder(activity)
                .setTitle("Go Bigger Boy update available")
                .setMessage("Go Bigger Boy " + offer.version +
                        " is available. Download and install it now?")
                .setNegativeButton("Later", null)
                .setPositiveButton("Update now", (dialog, which) ->
                        downloadUpdate(offer.version, offer.url, offer.sha256))
                .show();
    }

    private void downloadUpdate(String version, String url, String sha256) {
        Toast.makeText(activity, "Downloading Go Bigger Boy " + version + "…",
                Toast.LENGTH_LONG).show();
        new Thread(() -> {
            try {
                final File directory = new File(activity.getCacheDir(), "updates");
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
                activity.runOnUiThread(this::requestUpdateInstallation);
            } catch (Exception error) {
                Log.e(TAG, "Update download failed", error);
                activity.runOnUiThread(() -> new AlertDialog.Builder(activity)
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
                !activity.getPackageManager().canRequestPackageInstalls()) {
            new AlertDialog.Builder(activity)
                    .setTitle("Allow app updates")
                    .setMessage("Android must allow Go Bigger Boy to install its " +
                            "verified update. Enable ‘Allow from this source’, then return.")
                    .setNegativeButton("Cancel", null)
                    .setPositiveButton("Open settings", (dialog, which) -> {
                        awaitingInstallPermission = true;
                        final Intent settings = new Intent(
                                Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                                Uri.parse("package:" + activity.getPackageName()));
                        activity.startActivity(settings);
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
            installer = activity.getPackageManager().getPackageInstaller();
            final PackageInstaller.SessionParams parameters =
                    new PackageInstaller.SessionParams(
                            PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            parameters.setAppPackageName(activity.getPackageName());
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

                final Intent result = new Intent(activity, UpdateResultActivity.class)
                        .setAction(GbbActivity.ACTION_INSTALL_RESULT)
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
                        activity, sessionId, result,
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
            activity.runOnUiThread(() -> new AlertDialog.Builder(activity)
                    .setTitle("Update failed")
                    .setMessage("Android could not start the update installer. " +
                            "Please try again.")
                    .setNegativeButton("Cancel", null)
                    .setPositiveButton("Retry", (dialog, which) ->
                            installPendingUpdate())
                    .show());
        }
    }

    private String installedVersion() throws Exception {
        final PackageInfo info = activity.getPackageManager().getPackageInfo(
                activity.getPackageName(), 0);
        return info.versionName == null ? "0.0.0" : info.versionName;
    }

    private boolean installedFromPlayStore() {
        try {
            final PackageManager packageManager = activity.getPackageManager();
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                final InstallSourceInfo source =
                        packageManager.getInstallSourceInfo(activity.getPackageName());
                if (source == null) {
                    Log.i(TAG, "Install source unavailable; using direct update channel");
                    return false;
                }
                final String installing = source.getInstallingPackageName();
                final String initiating = source.getInitiatingPackageName();
                final String originating = source.getOriginatingPackageName();
                final String updateOwner = source.getUpdateOwnerPackageName();
                final boolean playStore = isPlayStoreSource(
                        installing, initiating, originating, updateOwner);
                Log.i(TAG, "Install source installing=" + installing +
                        " initiating=" + initiating +
                        " originating=" + originating +
                        " updateOwner=" + updateOwner +
                        " playStore=" + playStore);
                return playStore;
            }
            @SuppressWarnings("deprecation")
            final String installer = packageManager.getInstallerPackageName(
                    activity.getPackageName());
            final boolean playStore = isPlayStoreInstaller(installer);
            Log.i(TAG, "Install source installer=" + installer +
                    " playStore=" + playStore);
            return playStore;
        } catch (Exception error) {
            // Unknown installers are treated as direct builds so their updater
            // remains available rather than silently disabling updates.
            Log.d(TAG, "Could not identify app installer", error);
            return false;
        }
    }

    static boolean isPlayStoreInstaller(String installerPackage) {
        return PLAY_STORE_PACKAGE.equals(installerPackage);
    }

    static boolean isPlayStoreSource(String installingPackage, String initiatingPackage,
            String originatingPackage, String updateOwnerPackage) {
        return isPlayStoreInstaller(installingPackage) ||
                isPlayStoreInstaller(initiatingPackage) ||
                isPlayStoreInstaller(originatingPackage) ||
                isPlayStoreInstaller(updateOwnerPackage);
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
        connection.setRequestProperty("User-Agent", "Go Bigger-Boy/Android");
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
        connection.setRequestProperty("User-Agent", "Go Bigger-Boy/Android");
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
        final HttpURLConnection connection =
                (HttpURLConnection) new URL(url).openConnection();
        connection.setInstanceFollowRedirects(true);
        return connection;
    }

    private static byte[] readLimited(InputStream input, int limit) throws Exception {
        final java.io.ByteArrayOutputStream output =
                new java.io.ByteArrayOutputStream();
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
