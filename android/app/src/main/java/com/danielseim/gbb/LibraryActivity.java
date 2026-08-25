package com.danielseim.gbb;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.net.Uri;
import android.os.Bundle;
import android.os.Build;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.text.DateFormat;
import java.util.Date;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.zip.CRC32;

/** Native Android library and settings dashboard. */
public final class LibraryActivity extends Activity {
    private static final int OPEN_ROM = 1;
    private static final String[] PALETTE_NAMES = {
            "Grayscale", "Classic green", "Game Boy Pocket", "Amber",
            "Game Boy Color (automatic)"
    };
    private static final String[] PALETTE_IDS = {
            "grayscale", "classic", "pocket", "amber", "cgb-auto"
    };
    private static final char FIELD_SEPARATOR = 0x1f;

    static {
        System.loadLibrary("SDL3");
        System.loadLibrary("main");
    }

    private static native String[] nativeLibraryEntries(String directory);
    private static native boolean nativeRemoveLibraryEntry(
            String directory, String fingerprint);
    private static native float nativeTouchControlScale(String directory);
    private static native float nativeTouchControlOpacity(String directory);
    private static native float[] nativeTouchControlLayout(String directory);
    private static native void nativeSetTouchControlSettings(
            String directory, float scale, float opacity);
    private static native void nativeSetTouchControlLayout(
            String directory, float[] positions);
    private static native void nativeResetTouchControlLayout(String directory);

    private final ExecutorService artworkExecutor =
            Executors.newFixedThreadPool(2);
    private LinearLayout content;
    private SharedPreferences preferences;
    private boolean settingsVisible;
    private AndroidUpdateManager updateManager;
    private OnBackInvokedCallback backCallback;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        preferences = getSharedPreferences("dashboard", MODE_PRIVATE);
        updateManager = new AndroidUpdateManager(this);
        settingsVisible = savedInstanceState != null &&
                savedInstanceState.getBoolean("settings_visible", false);
        showDashboard(settingsVisible);
        updateManager.checkForUpdates();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            backCallback = this::confirmExit;
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    OnBackInvokedDispatcher.PRIORITY_DEFAULT, backCallback);
        }
    }

    @Override
    protected void onSaveInstanceState(Bundle state) {
        state.putBoolean("settings_visible", settingsVisible);
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
        artworkExecutor.shutdownNow();
        super.onDestroy();
    }

    /**
     * The SDL activity remains underneath the native library so ROMs can be
     * reopened without starting a second SDL loop. Finishing this activity
     * alone would expose that blank SDL surface. Close the whole task from
     * the library instead, after asking for confirmation once.
     */
    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        confirmExit();
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
        if (content != null) showDashboard(settingsVisible);
        if (updateManager != null) updateManager.onResume();
    }

    @Override
    protected void onPause() {
        if (updateManager != null) updateManager.onPause();
        super.onPause();
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private TextView text(String value, float size, int color) {
        final TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        return view;
    }

    private void showDashboard(boolean settings) {
        settingsVisible = settings;
        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(246, 247, 251));

        final LinearLayout header = new LinearLayout(this);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(dp(20), dp(18), dp(12), dp(12));
        final TextView brand = text("Go Bigger Boy", 24, Color.rgb(24, 29, 39));
        brand.setTypeface(null, android.graphics.Typeface.BOLD);
        header.addView(brand, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        final Button libraryTab = new Button(this);
        libraryTab.setText("Library");
        libraryTab.setEnabled(settings);
        libraryTab.setOnClickListener(view -> showDashboard(false));
        header.addView(libraryTab);
        final Button settingsTab = new Button(this);
        settingsTab.setText("Settings");
        settingsTab.setEnabled(!settings);
        settingsTab.setOnClickListener(view -> showDashboard(true));
        header.addView(settingsTab);
        root.addView(header);

        final ScrollView scroll = new ScrollView(this);
        content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(20), dp(8), dp(20), dp(28));
        scroll.addView(content);
        root.addView(scroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(root);

        if (settings) populateSettings(); else populateLibrary();
    }

    private void populateLibrary() {
        final TextView heading = text("Recently played", 22, Color.rgb(24, 29, 39));
        heading.setTypeface(null, android.graphics.Typeface.BOLD);
        content.addView(heading);
        final TextView help = text(
                "Choose a game to continue, or add a ROM from your device.",
                15, Color.DKGRAY);
        help.setPadding(0, dp(4), 0, dp(14));
        content.addView(help);

        final Button open = new Button(this);
        open.setText("Open ROM");
        open.setOnClickListener(view -> openRomPicker());
        final LinearLayout.LayoutParams openParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        openParams.bottomMargin = dp(16);
        content.addView(open, openParams);

        final String[] entries = nativeLibraryEntries(getFilesDir().getAbsolutePath());
        if (entries == null || entries.length == 0) {
            final TextView empty = text(
                    "No games yet\n\nOpen a Game Boy or Game Boy Color ROM to add it here.",
                    17, Color.GRAY);
            empty.setGravity(Gravity.CENTER);
            empty.setPadding(dp(16), dp(52), dp(16), dp(52));
            content.addView(empty);
            return;
        }
        for (String encoded : entries) addGameCard(encoded);
    }

    private void addGameCard(String encoded) {
        final String[] fields = encoded.split(String.valueOf(FIELD_SEPARATOR), -1);
        if (fields.length != 9) return;

        final LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.HORIZONTAL);
        card.setGravity(Gravity.CENTER_VERTICAL);
        card.setPadding(dp(12), dp(12), dp(12), dp(12));
        card.setBackgroundColor(Color.WHITE);
        card.setElevation(dp(2));
        card.setClickable(true);
        card.setFocusable(true);
        card.setOnClickListener(view -> launchRom(fields[2], ""));
        final LinearLayout.LayoutParams cardParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        cardParams.bottomMargin = dp(12);

        final ImageView cover = new ImageView(this);
        cover.setScaleType(ImageView.ScaleType.CENTER_CROP);
        cover.setBackgroundColor(Color.rgb(225, 228, 235));
        card.addView(cover, new LinearLayout.LayoutParams(dp(82), dp(108)));

        final LinearLayout details = new LinearLayout(this);
        details.setOrientation(LinearLayout.VERTICAL);
        details.setPadding(dp(16), 0, 0, 0);
        final TextView title = text(fields[3], 19, Color.rgb(24, 29, 39));
        title.setTypeface(null, android.graphics.Typeface.BOLD);
        details.addView(title);
        final TextView platform = text(fields[4], 15, Color.rgb(69, 91, 171));
        platform.setPadding(0, dp(7), 0, dp(3));
        details.addView(platform);
        final TextView language = text("Language: " + fields[5], 14, Color.DKGRAY);
        details.addView(language);
        details.addView(text("Last played: " + formattedLastPlayed(fields[8]),
                13, Color.GRAY));
        final Button remove = new Button(this);
        remove.setText("Remove from list");
        remove.setOnClickListener(view -> new AlertDialog.Builder(this)
                .setTitle("Remove recent game?")
                .setMessage("The ROM file and saved game will not be deleted.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Remove", (dialog, which) -> {
                    if (nativeRemoveLibraryEntry(
                            getFilesDir().getAbsolutePath(), fields[0])) {
                        showDashboard(false);
                    } else {
                        Toast.makeText(this, "Could not remove recent game",
                                Toast.LENGTH_SHORT).show();
                    }
                }).show());
        details.addView(remove);
        card.addView(details, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        content.addView(card, cardParams);

        resolveMetadata(cover, title, language, fields);
    }

    private void populateSettings() {
        final TextView heading = text("Settings", 22, Color.rgb(24, 29, 39));
        heading.setTypeface(null, android.graphics.Typeface.BOLD);
        content.addView(heading);
        final TextView display = text("Display", 17, Color.DKGRAY);
        display.setPadding(0, dp(22), 0, dp(8));
        content.addView(display);

        final Spinner palette = new Spinner(this);
        palette.setAdapter(new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_dropdown_item, PALETTE_NAMES));
        palette.setSelection(currentPalette());
        palette.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view,
                                       int position, long id) {
                savePalette(position);
            }
            @Override public void onNothingSelected(AdapterView<?> parent) {}
        });
        content.addView(palette, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        final Switch artwork = new Switch(this);
        artwork.setText("Download game cover artwork");
        artwork.setTextSize(16);
        artwork.setPadding(0, dp(24), 0, dp(8));
        artwork.setChecked(preferences.getBoolean("cover_artwork", true));
        artwork.setOnCheckedChangeListener((button, enabled) ->
                preferences.edit().putBoolean("cover_artwork", enabled).apply());
        content.addView(artwork);
        final TextView privacy = text(
                "Artwork is fetched from Libretro's public thumbnail service " +
                "and cached on this device. ROM contents are never uploaded.",
                13, Color.GRAY);
        privacy.setPadding(0, 0, 0, dp(20));
        content.addView(privacy);

        final TextView input = text("Input", 17, Color.DKGRAY);
        input.setPadding(0, dp(10), 0, dp(8));
        content.addView(input);
        content.addView(text(
                "Touch controls are shown while playing. Connected controllers " +
                "use the standard Game Boy layout.", 15, Color.DKGRAY));

        final TextView touchHeading = text("Touch controls", 17, Color.DKGRAY);
        touchHeading.setPadding(0, dp(22), 0, dp(4));
        content.addView(touchHeading);
        content.addView(text(
                "Adjust the on-screen button size and visibility for your phone " +
                "or tablet. Portrait and landscape layouts are independent; " +
                "the D-pad is always moved as one control.", 15, Color.DKGRAY));

        final String settingsDirectory = getFilesDir().getAbsolutePath();
        final float[] touchValues = {
                nativeTouchControlScale(settingsDirectory),
                nativeTouchControlOpacity(settingsDirectory)};
        final TextView sizeLabel = text("Button size: " +
                Math.round(touchValues[0] * 100) + "%", 15, Color.DKGRAY);
        sizeLabel.setPadding(0, dp(16), 0, 0);
        content.addView(sizeLabel);
        final SeekBar size = new SeekBar(this);
        size.setMax(100);
        size.setProgress(Math.round((touchValues[0] - 0.8f) / 1.2f * 100));
        size.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar bar, int progress,
                                           boolean fromUser) {
                touchValues[0] = 0.8f + progress / 100f * 1.2f;
                sizeLabel.setText("Button size: " +
                        Math.round(touchValues[0] * 100) + "%");
                nativeSetTouchControlSettings(settingsDirectory,
                        touchValues[0], touchValues[1]);
            }
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {}
        });
        content.addView(size, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        final TextView opacityLabel = text("Button opacity: " +
                Math.round(touchValues[1] * 100) + "%", 15, Color.DKGRAY);
        opacityLabel.setPadding(0, dp(12), 0, 0);
        content.addView(opacityLabel);
        final SeekBar opacity = new SeekBar(this);
        opacity.setMax(100);
        opacity.setProgress(Math.round((touchValues[1] - 0.2f) / 0.8f * 100));
        opacity.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar bar, int progress,
                                           boolean fromUser) {
                touchValues[1] = 0.2f + progress / 100f * 0.8f;
                opacityLabel.setText("Button opacity: " +
                        Math.round(touchValues[1] * 100) + "%");
                nativeSetTouchControlSettings(settingsDirectory,
                        touchValues[0], touchValues[1]);
            }
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {}
        });
        content.addView(opacity, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        final Button editLayout = new Button(this);
        editLayout.setText("Customize button layout");
        editLayout.setOnClickListener(view -> showTouchLayoutEditor());
        content.addView(editLayout);

        final Button resetTouch = new Button(this);
        resetTouch.setText("Reset touch controls");
        resetTouch.setOnClickListener(view -> {
            size.setProgress(Math.round((1.35f - 0.8f) / 1.2f * 100));
            opacity.setProgress(Math.round((0.78f - 0.2f) / 0.8f * 100));
            nativeResetTouchControlLayout(settingsDirectory);
        });
        content.addView(resetTouch);
    }

    private void showTouchLayoutEditor() {
        final String settingsDirectory = getFilesDir().getAbsolutePath();
        final float[] initial = nativeTouchControlLayout(settingsDirectory);
        final TouchLayoutView editor = new TouchLayoutView(
                initial == null ? defaultTouchLayout() : initial);
        final LinearLayout container = new LinearLayout(this);
        container.setOrientation(LinearLayout.VERTICAL);
        final Spinner orientation = new Spinner(this);
        orientation.setAdapter(new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_dropdown_item,
                new String[]{"Portrait layout", "Landscape layout"}));
        final boolean startsLandscape =
                getResources().getConfiguration().orientation == 2;
        editor.setLandscape(startsLandscape);
        orientation.setSelection(startsLandscape ? 1 : 0);
        orientation.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view,
                                       int position, long id) {
                editor.setLandscape(position == 1);
                final ViewGroup.LayoutParams params = editor.getLayoutParams();
                if (params != null) {
                    params.height = dp(position == 1 ? 220 : 360);
                    editor.setLayoutParams(params);
                }
            }
            @Override public void onNothingSelected(AdapterView<?> parent) {}
        });
        container.addView(orientation, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        // Landscape dialogs have substantially less vertical space than the
        // portrait dashboard. Keep the editor proportional to its landscape
        // canvas, and put the complete editor contents in a scroll container
        // so the help/reset controls and dialog actions remain reachable.
        final int editorHeight = startsLandscape ? dp(220) : dp(360);
        container.addView(editor, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, editorHeight));
        final TextView help = text(
                "Drag the D-pad, A, B, Select, and Start to their preferred " +
                "positions. Controls may sit beside or below the game screen.",
                13, Color.GRAY);
        help.setPadding(0, dp(8), 0, 0);
        container.addView(help);
        final Button reset = new Button(this);
        reset.setText("Reset positions");
        reset.setOnClickListener(view -> editor.setLayout(defaultTouchLayout()));
        container.addView(reset);
        final ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(false);
        scroll.addView(container, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        final AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("Customize touch controls")
                .setView(scroll)
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Save", null)
                .create();
        dialog.setOnShowListener(ignored -> dialog.getButton(
                AlertDialog.BUTTON_POSITIVE).setOnClickListener(view -> {
                    nativeSetTouchControlLayout(settingsDirectory,
                            editor.getLayout());
                    dialog.dismiss();
                }));
        dialog.show();
    }

    private static float[] defaultTouchLayout() {
        return new float[]{
                0.27f, 0.82f, 0.74f, 0.79f, 0.74f, 0.90f, 0.43f, 0.96f,
                0.57f, 0.96f,
                0.12f, 0.50f, 0.88f, 0.42f, 0.88f, 0.62f, 0.42f, 0.92f,
                0.58f, 0.92f};
    }

    private final class TouchLayoutView extends View {
        private final float[] WIDTHS = {42, 24, 24, 22, 22};
        private final float[] HEIGHTS = {42, 24, 24, 10, 10};
        private final String[] LABELS = {
                "D-pad", "A", "B", "Select", "Start"};
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private float[] layout;
        private int selected = -1;
        private float dragOffsetX;
        private float dragOffsetY;
        private boolean landscape;

        TouchLayoutView(float[] initial) {
            super(LibraryActivity.this);
            layout = new float[20];
            if (initial == null || initial.length < layout.length) {
                layout = defaultTouchLayout();
            } else {
                System.arraycopy(initial, 0, layout, 0, layout.length);
            }
            setBackgroundColor(Color.rgb(26, 31, 42));
        }

        float[] getLayout() {
            return layout.clone();
        }

        void setLayout(float[] values) {
            if (values == null || values.length < layout.length) return;
            System.arraycopy(values, 0, layout, 0, layout.length);
            invalidate();
        }

        void setLandscape(boolean value) {
            landscape = value;
            selected = -1;
            invalidate();
        }

        private float designWidth() {
            return landscape ? 320f : 180f;
        }

        private float designHeight() {
            return landscape ? 180f : 320f;
        }

        private float logicalScale() {
            return Math.min(getWidth() / designWidth(),
                    getHeight() / designHeight());
        }

        private float offsetX() {
            return (getWidth() - designWidth() * logicalScale()) * 0.5f;
        }

        private float offsetY() {
            return (getHeight() - designHeight() * logicalScale()) * 0.5f;
        }

        private int positionIndex(int control) {
            return (landscape ? 10 : 0) + control * 2;
        }

        private RectF bounds(int index) {
            final float scale = logicalScale();
            final int position = positionIndex(index);
            final float centerX = offsetX() + layout[position] *
                    designWidth() * scale;
            final float centerY = offsetY() + layout[position + 1] *
                    designHeight() * scale;
            final float width = WIDTHS[index] * scale;
            final float height = HEIGHTS[index] * scale;
            return new RectF(centerX - width * 0.5f, centerY - height * 0.5f,
                    centerX + width * 0.5f, centerY + height * 0.5f);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            final float scale = logicalScale();
            final float screenLeft = offsetX() + (landscape ? 80f : 10f) * scale;
            final float screenTop = offsetY() + (landscape ? 18f : 10f) * scale;
            final RectF screen = new RectF(screenLeft, screenTop,
                    screenLeft + 160f * scale, screenTop + 144f * scale);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(Math.max(1f, scale));
            paint.setColor(Color.rgb(151, 170, 132));
            canvas.drawRect(screen, paint);
            paint.setStyle(Paint.Style.FILL);
            for (int index = 0; index < 5; ++index) {
                final RectF bounds = bounds(index);
                paint.setColor(index == selected
                        ? Color.rgb(255, 215, 92) : Color.rgb(124, 183, 140));
                canvas.drawRoundRect(bounds, 5f * scale, 5f * scale, paint);
                paint.setColor(Color.rgb(24, 35, 38));
                paint.setTextAlign(Paint.Align.CENTER);
                paint.setTextSize(Math.max(9f, 8f * scale));
                canvas.drawText(LABELS[index], bounds.centerX(),
                        bounds.centerY() - (paint.ascent() + paint.descent()) * 0.5f,
                        paint);
            }
        }

        private int hit(float x, float y) {
            for (int index = 4; index >= 0; --index) {
                if (bounds(index).contains(x, y)) return index;
            }
            return -1;
        }

        @Override
        public boolean onTouchEvent(MotionEvent event) {
            final float scale = logicalScale();
            switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                selected = hit(event.getX(), event.getY());
                if (selected < 0) return false;
                dragOffsetX = event.getX() - bounds(selected).centerX();
                dragOffsetY = event.getY() - bounds(selected).centerY();
                invalidate();
                return true;
            case MotionEvent.ACTION_MOVE:
                if (selected < 0 || scale <= 0) return true;
                final float centerX = event.getX() - dragOffsetX;
                final float centerY = event.getY() - dragOffsetY;
                final int position = positionIndex(selected);
                layout[position] = Math.max(0.02f, Math.min(0.98f,
                        (centerX - offsetX()) / (designWidth() * scale)));
                layout[position + 1] = Math.max(0.02f, Math.min(0.98f,
                        (centerY - offsetY()) / (designHeight() * scale)));
                invalidate();
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                selected = -1;
                invalidate();
                return true;
            default:
                return true;
            }
        }
    }

    private void openRomPicker() {
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

    private void launchRom(String path, String displayName) {
        startActivity(new Intent(this, GbbActivity.class)
                .putExtra(GbbActivity.EXTRA_ROM, path)
                .putExtra(GbbActivity.EXTRA_ROM_NAME, displayName));
    }

    private int currentPalette() {
        final File file = new File(getFilesDir(), "palette.txt");
        try (InputStream input = new FileInputStream(file)) {
            final byte[] bytes = new byte[32];
            final int count = input.read(bytes);
            final String current = count > 0
                    ? new String(bytes, 0, count, StandardCharsets.UTF_8).trim()
                    : "";
            for (int index = 0; index < PALETTE_IDS.length; ++index) {
                if (PALETTE_IDS[index].equals(current)) return index;
            }
        } catch (Exception ignored) {
        }
        return 0;
    }

    private void savePalette(int position) {
        if (position < 0 || position >= PALETTE_IDS.length) return;
        try (FileOutputStream output = new FileOutputStream(
                new File(getFilesDir(), "palette.txt"), false)) {
            output.write((PALETTE_IDS[position] + "\n")
                    .getBytes(StandardCharsets.UTF_8));
        } catch (Exception error) {
            Toast.makeText(this, "Could not save display setting",
                    Toast.LENGTH_SHORT).show();
        }
    }

    private static String pathSegment(String value) throws Exception {
        return URLEncoder.encode(value, "UTF-8").replace("+", "%20");
    }

    private void loadCover(ImageView view, String fingerprint,
                           String system, String name) {
        final File directory = new File(getCacheDir(), "covers");
        final File cached = new File(directory, fingerprint + ".png");
        artworkExecutor.execute(() -> {
            Bitmap bitmap = BitmapFactory.decodeFile(cached.getAbsolutePath());
            if (bitmap == null) {
                HttpURLConnection connection = null;
                try {
                    if (!directory.exists() && !directory.mkdirs()) return;
                    final URL url = new URL("https://thumbnails.libretro.com/" +
                            pathSegment(system) + "/Named_Boxarts/" +
                            pathSegment(thumbnailName(name)) + ".png");
                    connection = (HttpURLConnection) url.openConnection();
                    connection.setConnectTimeout(5000);
                    connection.setReadTimeout(8000);
                    connection.setRequestProperty("User-Agent", "Go Bigger Boy");
                    if (connection.getResponseCode() != 200 ||
                            connection.getContentLength() > 5 * 1024 * 1024) return;
                    try (InputStream input = new BufferedInputStream(
                                 connection.getInputStream());
                         FileOutputStream output = new FileOutputStream(cached)) {
                        final byte[] buffer = new byte[16 * 1024];
                        long total = 0;
                        int count;
                        while ((count = input.read(buffer)) != -1) {
                            total += count;
                            if (total > 5 * 1024 * 1024) return;
                            output.write(buffer, 0, count);
                        }
                    }
                    bitmap = BitmapFactory.decodeFile(cached.getAbsolutePath());
                } catch (Exception ignored) {
                    if (cached.exists()) cached.delete();
                } finally {
                    if (connection != null) connection.disconnect();
                }
            }
            final Bitmap result = bitmap;
            if (result != null) runOnUiThread(() -> view.setImageBitmap(result));
        });
    }

    private void resolveMetadata(ImageView cover, TextView title,
                                 TextView language, String[] fields) {
        artworkExecutor.execute(() -> {
            long crc = 0;
            try {
                crc = Long.parseLong(fields[1], 16);
            } catch (NumberFormatException ignored) {
            }
            if (crc == 0) crc = crcForFile(fields[2]);
            final LibretroMetadata.Record record =
                    LibretroMetadata.find(this, fields[6], crc);
            String coverName = fields[7];
            if (record != null) {
                coverName = record.name;
                runOnUiThread(() -> {
                    title.setText(displayTitle(record.name));
                    language.setText("Language: " + record.language);
                });
            }
            if (preferences.getBoolean("cover_artwork", true)) {
                loadCover(cover, fields[0], fields[6], coverName);
            }
        });
    }

    private long crcForFile(String path) {
        final CRC32 crc = new CRC32();
        try (InputStream input = new BufferedInputStream(new FileInputStream(path))) {
            final byte[] buffer = new byte[16 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) crc.update(buffer, 0, count);
            return crc.getValue();
        } catch (Exception ignored) {
            return 0;
        }
    }

    private static String displayTitle(String canonicalName) {
        final int tags = canonicalName.indexOf(" (");
        return tags < 0 ? canonicalName : canonicalName.substring(0, tags);
    }

    private static String formattedLastPlayed(String timestamp) {
        try {
            return DateFormat.getDateTimeInstance(DateFormat.MEDIUM,
                    DateFormat.SHORT).format(
                    new Date(Long.parseLong(timestamp) * 1000L));
        } catch (Exception ignored) {
            return "Unknown";
        }
    }

    private static String thumbnailName(String name) {
        return name.replaceAll("[&*/:`<>?\\\\|]", "_");
    }
}
