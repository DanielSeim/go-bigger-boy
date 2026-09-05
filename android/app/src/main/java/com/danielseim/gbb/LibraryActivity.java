package com.danielseim.gbb;

import android.annotation.SuppressLint;
import android.app.ActionBar;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Build;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

/** Native Android library and settings dashboard. */
public final class LibraryActivity extends Activity {
    static final String EXTRA_RETURN_TO_GAME =
            "com.danielseim.gbb.RETURN_TO_GAME";
    private static final int OPEN_ROM = 1;
    static {
        System.loadLibrary("SDL3");
        System.loadLibrary("main");
    }

    static native String[] nativeLibraryEntries(String directory);
    static native boolean nativeRemoveLibraryEntry(
            String directory, String fingerprint);
    static native float nativeTouchControlScale(String directory);
    static native float nativeTouchControlOpacity(String directory);
    static native boolean nativeTouchVoxelOrbitEnabled(String directory);
    static native boolean nativeTouchMenuTopRight(String directory);
    static native float[] nativeTouchControlLayout(String directory);
    static native void nativeSetTouchControlSettings(
            String directory, float scale, float opacity);
    static native void nativeSetTouchVoxelOrbitEnabled(
            String directory, boolean enabled);
    static native void nativeSetTouchMenuTopRight(
            String directory, boolean topRight);
    static native void nativeSetTouchControlLayout(
            String directory, float[] positions);
    static native void nativeResetTouchControlLayout(String directory);
    static native String nativeVideoMode(String directory);
    static native void nativeSetVideoMode(String directory, String mode);
    static native String nativeLinkRemoteHost(String directory);
    static native String nativeLinkRemoteBind(String directory);
    static native int nativeLinkRemotePort(String directory);
    static native boolean nativeLinkLanDiscovery(String directory);
    static native void nativeSetLinkSettings(String directory, String host,
            String bind, int port, boolean discovery);

    private ArtworkService artworkService;
    private LinearLayout content;
    private ScrollView scrollView;
    private SharedPreferences preferences;
    private boolean settingsVisible;
    private boolean returnToGame;
    private int libraryScrollY;
    private int settingsScrollY;
    private AndroidUpdateManager updateManager;
    private OnBackInvokedCallback backCallback;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final ActionBar actionBar = getActionBar();
        if (actionBar != null) actionBar.hide();
        preferences = getSharedPreferences("dashboard", MODE_PRIVATE);
        artworkService = new ArtworkService(this);
        returnToGame = getIntent().getBooleanExtra(EXTRA_RETURN_TO_GAME, false);
        updateManager = new AndroidUpdateManager(this);
        settingsVisible = savedInstanceState != null &&
                savedInstanceState.getBoolean("settings_visible", false);
        if (savedInstanceState != null) {
            returnToGame = savedInstanceState.getBoolean("return_to_game", returnToGame);
            libraryScrollY = savedInstanceState.getInt("library_scroll_y", 0);
            settingsScrollY = savedInstanceState.getInt("settings_scroll_y", 0);
        }
        showDashboard(settingsVisible);
        updateManager.checkForUpdates();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            backCallback = this::handleBack;
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    OnBackInvokedDispatcher.PRIORITY_DEFAULT, backCallback);
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        returnToGame = intent.getBooleanExtra(EXTRA_RETURN_TO_GAME, false);
        showDashboard(settingsVisible);
    }

    @Override
    protected void onSaveInstanceState(Bundle state) {
        state.putBoolean("settings_visible", settingsVisible);
        state.putBoolean("return_to_game", returnToGame);
        state.putInt("library_scroll_y", libraryScrollY);
        state.putInt("settings_scroll_y", settingsScrollY);
        super.onSaveInstanceState(state);
    }

    @Override
    protected void onDestroy() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
                backCallback != null) {
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(
                    backCallback);
            backCallback = null;
        }
        if (artworkService != null) artworkService.shutdown();
        super.onDestroy();
    }

    /** Handle Back as navigation first, and only offer exit at the app root. */
    @Override
    @SuppressLint("GestureBackNavigation")
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        handleBack();
    }

    private void handleBack() {
        if (settingsVisible) {
            showDashboard(false);
        } else if (returnToGame) {
            finish();
        } else {
            confirmExit();
        }
    }

    private void confirmExit() {
        new AlertDialog.Builder(this)
                .setTitle("Exit Go Bigger Boy?")
                .setMessage("Are you sure you want to close Go Bigger Boy?")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Exit", (dialog, which) -> finishAffinity())
                .show();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (updateManager != null) updateManager.onResume();
    }

    @Override
    protected void onPause() {
        if (updateManager != null) updateManager.onPause();
        super.onPause();
    }

    int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    TextView text(String value, float size, int color) {
        final TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        return view;
    }

    void showDashboard(boolean settings) {
        settingsVisible = settings;
        if (scrollView != null) {
            if (settingsVisible) settingsScrollY = scrollView.getScrollY();
            else libraryScrollY = scrollView.getScrollY();
        }
        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(246, 247, 251));

        final LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.VERTICAL);
        header.setBackgroundColor(Color.WHITE);
        header.setPadding(dp(8), dp(8), dp(8), 0);

        final LinearLayout toolbar = new LinearLayout(this);
        toolbar.setGravity(Gravity.CENTER_VERTICAL);
        final Button back = new Button(this);
        back.setText("‹");
        back.setTextSize(30);
        back.setAllCaps(false);
        back.setMinWidth(dp(48));
        back.setMinHeight(dp(48));
        back.setContentDescription(settings ? "Back to library"
                : returnToGame ? "Back to game" : "Exit");
        back.setOnClickListener(view -> handleBack());
        toolbar.addView(back, new LinearLayout.LayoutParams(
                dp(48), dp(48)));
        final TextView brand = text(settings ? "Settings" : "Library",
                22, Color.rgb(24, 29, 39));
        brand.setTypeface(null, android.graphics.Typeface.BOLD);
        brand.setGravity(Gravity.CENTER_VERTICAL);
        toolbar.addView(brand, new LinearLayout.LayoutParams(
                0, dp(48), 1));
        header.addView(toolbar);

        final LinearLayout tabs = new LinearLayout(this);
        tabs.setGravity(Gravity.CENTER);
        final Button libraryTab = tabButton("Library", !settings);
        libraryTab.setOnClickListener(view -> showDashboard(false));
        final Button settingsTab = tabButton("Settings", settings);
        settingsTab.setOnClickListener(view -> showDashboard(true));
        tabs.addView(libraryTab, new LinearLayout.LayoutParams(
                0, dp(44), 1));
        tabs.addView(settingsTab, new LinearLayout.LayoutParams(
                0, dp(44), 1));
        header.addView(tabs);
        root.addView(header);

        final ScrollView scroll = new ScrollView(this);
        scrollView = scroll;
        content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(20), dp(8), dp(20), dp(28));
        scroll.addView(content);
        root.addView(scroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(root);
        scroll.post(() -> scroll.scrollTo(0,
                settings ? settingsScrollY : libraryScrollY));

        if (settings) {
            new SettingsScreen(this, content, preferences).populate();
        } else {
            new LibraryScreen(this, content, preferences, artworkService).populate();
        }
    }

    private Button tabButton(String label, boolean selected) {
        final Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setTextSize(15);
        button.setTextColor(selected ? Color.rgb(32, 74, 135)
                : Color.rgb(80, 88, 102));
        button.setTypeface(null, selected ? android.graphics.Typeface.BOLD
                : android.graphics.Typeface.NORMAL);
        button.setBackgroundColor(selected ? Color.rgb(224, 235, 252)
                : Color.TRANSPARENT);
        button.setContentDescription(label + (selected ? ", selected" : ""));
        return button;
    }

    void openRomPicker() {
        final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("application/octet-stream");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{
                "application/octet-stream", "application/x-gameboy-rom",
                "application/x-gameboy-color-rom"
        });
        startActivityForResult(intent, OPEN_ROM);
    }

    @Override
    @SuppressLint("WrongConstant")
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != OPEN_ROM || resultCode != RESULT_OK ||
                data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        final int flags = data.getFlags() &
                (Intent.FLAG_GRANT_READ_URI_PERMISSION |
                 Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try {
            getContentResolver().takePersistableUriPermission(uri, flags);
        } catch (SecurityException ignored) {
        }
        launchRom(uri.toString(), displayName(uri));
    }

    private String displayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(
                uri, new String[]{OpenableColumns.DISPLAY_NAME},
                null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                final int column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (column >= 0) return cursor.getString(column);
            }
        } catch (Exception ignored) {
        }
        return "";
    }

    void launchRom(String path, String displayName) {
        startActivity(new Intent(this, GbbActivity.class)
                .putExtra(GbbActivity.EXTRA_ROM, path)
                .putExtra(GbbActivity.EXTRA_ROM_NAME, displayName));
    }

}
