package com.danielseim.gbb;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

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

    private final ExecutorService artworkExecutor =
            Executors.newFixedThreadPool(2);
    private LinearLayout content;
    private SharedPreferences preferences;
    private boolean settingsVisible;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        preferences = getSharedPreferences("dashboard", MODE_PRIVATE);
        showDashboard(false);
    }

    @Override
    protected void onDestroy() {
        artworkExecutor.shutdownNow();
        super.onDestroy();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (content != null) showDashboard(settingsVisible);
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
        if (fields.length != 7) return;

        final LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.HORIZONTAL);
        card.setGravity(Gravity.CENTER_VERTICAL);
        card.setPadding(dp(12), dp(12), dp(12), dp(12));
        card.setBackgroundColor(Color.WHITE);
        card.setElevation(dp(2));
        card.setClickable(true);
        card.setFocusable(true);
        card.setOnClickListener(view -> launchRom(fields[1]));
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
        final TextView title = text(fields[2], 19, Color.rgb(24, 29, 39));
        title.setTypeface(null, android.graphics.Typeface.BOLD);
        details.addView(title);
        final TextView platform = text(fields[3], 15, Color.rgb(69, 91, 171));
        platform.setPadding(0, dp(7), 0, dp(3));
        details.addView(platform);
        details.addView(text("Language: " + fields[4], 14, Color.DKGRAY));
        card.addView(details, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        content.addView(card, cardParams);

        if (preferences.getBoolean("cover_artwork", true)) {
            loadCover(cover, fields[0], fields[5], fields[6]);
        }
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
        launchRom(uri.toString());
    }

    private void launchRom(String path) {
        startActivity(new Intent(this, GbbActivity.class)
                .putExtra(GbbActivity.EXTRA_ROM, path));
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
                            pathSegment(name) + ".png");
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
}
