package com.danielseim.gbb;

import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.text.InputType;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

/** Builds display, artwork, and touch settings independently from navigation. */
final class SettingsScreen {
    private static final String[] PALETTE_NAMES = {
            "Grayscale", "Classic green", "Game Boy Pocket", "Amber",
            "Game Boy Color (automatic)"
    };
    private static final String[] PALETTE_IDS = {
            "grayscale", "classic", "pocket", "amber", "cgb-auto"
    };
    private static final String[] VIDEO_MODE_NAMES = {
            "Nearest neighbor", "Bilinear", "Integer scaling", "LCD shader",
            "Voxel diorama", "Voxel diorama (shape-aware)",
            "Voxel pop-up book"
    };
    private static final String[] VIDEO_MODE_IDS = {
            "nearest", "bilinear", "integer", "lcd", "voxel", "voxel_shape",
            "voxel_popup"
    };
    private static final String[] MENU_POSITION_NAMES = {
            "Top left", "Top right"
    };

    private final LibraryActivity activity;
    private final LinearLayout content;
    private final SharedPreferences preferences;

    SettingsScreen(LibraryActivity activity, LinearLayout content,
                   SharedPreferences preferences) {
        this.activity = activity;
        this.content = content;
        this.preferences = preferences;
    }

    void populate() {
        final LinearLayout display = sectionCard("Display");
        final Spinner palette = new Spinner(activity);
        palette.setAdapter(new ArrayAdapter<>(activity,
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
        display.addView(settingLabel("Color palette"));
        display.addView(palette, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        display.addView(settingLabel("Video pipeline"));
        final Spinner video = new Spinner(activity);
        video.setAdapter(new ArrayAdapter<>(activity,
                android.R.layout.simple_spinner_dropdown_item,
                VIDEO_MODE_NAMES));
        video.setSelection(currentVideoMode());
        video.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view,
                                       int position, long id) {
                saveVideoMode(position);
            }
            @Override public void onNothingSelected(AdapterView<?> parent) {}
        });
        display.addView(video, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        final LinearLayout artworkCard = sectionCard("Artwork");
        final Switch artwork = new Switch(activity);
        artwork.setText("Download game cover artwork");
        artwork.setTextSize(16);
        artwork.setPadding(0, activity.dp(8), 0, activity.dp(8));
        artwork.setChecked(preferences.getBoolean("cover_artwork", true));
        artwork.setOnCheckedChangeListener((button, enabled) ->
                preferences.edit().putBoolean("cover_artwork", enabled).apply());
        artworkCard.addView(artwork);
        final TextView privacy = activity.text(
                "Artwork is fetched from Libretro's public thumbnail service " +
                "and cached on this device. ROM contents are never uploaded.",
                13, Color.GRAY);
        privacy.setPadding(0, 0, 0, activity.dp(20));
        artworkCard.addView(privacy);

        final LinearLayout touchCard = sectionCard("Touch controls");
        touchCard.addView(activity.text(
                "Touch controls are shown while playing. Connected controllers " +
                "use the standard Game Boy layout.", 15, Color.DKGRAY));
        touchCard.addView(activity.text(
                "Adjust size and visibility independently for your phone " +
                "or tablet. Portrait and landscape layouts are independent; " +
                "the D-pad is always moved as one control. When a voxel mode " +
                "is active, a touch that starts outside a button can orbit the " +
                "camera.", 15, Color.DKGRAY));

        final String settingsDirectory = activity.getFilesDir().getAbsolutePath();
        final Spinner menuPosition = new Spinner(activity);
        menuPosition.setAdapter(new ArrayAdapter<>(activity,
                android.R.layout.simple_spinner_dropdown_item,
                MENU_POSITION_NAMES));
        menuPosition.setSelection(LibraryActivity.nativeTouchMenuTopRight(
                settingsDirectory) ? 1 : 0);
        menuPosition.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view,
                                       int position, long id) {
                LibraryActivity.nativeSetTouchMenuTopRight(settingsDirectory,
                        position == 1);
            }
            @Override public void onNothingSelected(AdapterView<?> parent) {}
        });
        touchCard.addView(settingLabel("In-game menu button"));
        touchCard.addView(menuPosition, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        final Switch voxelOrbit = new Switch(activity);
        voxelOrbit.setText("Enable voxel touch orbit");
        voxelOrbit.setTextSize(16);
        voxelOrbit.setPadding(0, activity.dp(10), 0, activity.dp(8));
        voxelOrbit.setChecked(LibraryActivity.nativeTouchVoxelOrbitEnabled(
                settingsDirectory));
        voxelOrbit.setOnCheckedChangeListener((button, enabled) ->
                LibraryActivity.nativeSetTouchVoxelOrbitEnabled(
                        settingsDirectory, enabled));
        touchCard.addView(voxelOrbit);

        final float[] touchValues = {
                LibraryActivity.nativeTouchControlScale(settingsDirectory),
                LibraryActivity.nativeTouchControlOpacity(settingsDirectory)};
        final TextView sizeLabel = activity.text("Button size: " +
                Math.round(touchValues[0] * 100) + "%", 15, Color.DKGRAY);
        sizeLabel.setPadding(0, activity.dp(16), 0, 0);
        touchCard.addView(sizeLabel);
        final SeekBar size = new SeekBar(activity);
        size.setMax(100);
        size.setProgress(Math.round((touchValues[0] - 0.8f) / 1.2f * 100));
        size.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar bar, int progress,
                                           boolean fromUser) {
                touchValues[0] = 0.8f + progress / 100f * 1.2f;
                sizeLabel.setText("Button size: " +
                        Math.round(touchValues[0] * 100) + "%");
                LibraryActivity.nativeSetTouchControlSettings(settingsDirectory,
                        touchValues[0], touchValues[1]);
            }
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {}
        });
        touchCard.addView(size, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        final TextView opacityLabel = activity.text("Button opacity: " +
                Math.round(touchValues[1] * 100) + "%", 15, Color.DKGRAY);
        opacityLabel.setPadding(0, activity.dp(12), 0, 0);
        touchCard.addView(opacityLabel);
        final SeekBar opacity = new SeekBar(activity);
        opacity.setMax(100);
        opacity.setProgress(Math.round((touchValues[1] - 0.2f) / 0.8f * 100));
        opacity.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar bar, int progress,
                                           boolean fromUser) {
                touchValues[1] = 0.2f + progress / 100f * 0.8f;
                opacityLabel.setText("Button opacity: " +
                        Math.round(touchValues[1] * 100) + "%");
                LibraryActivity.nativeSetTouchControlSettings(settingsDirectory,
                        touchValues[0], touchValues[1]);
            }
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {}
        });
        touchCard.addView(opacity, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        final Button editLayout = new Button(activity);
        editLayout.setText("Customize button layout");
        editLayout.setOnClickListener(view -> showTouchLayoutEditor());
        final LinearLayout.LayoutParams actionParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        actionParams.topMargin = activity.dp(10);
        touchCard.addView(editLayout, actionParams);

        final Button resetTouch = new Button(activity);
        resetTouch.setText("Reset touch controls");
        resetTouch.setOnClickListener(view -> {
            size.setProgress(Math.round((1.35f - 0.8f) / 1.2f * 100));
            opacity.setProgress(Math.round((0.78f - 0.2f) / 0.8f * 100));
            LibraryActivity.nativeResetTouchControlLayout(settingsDirectory);
        });
        touchCard.addView(resetTouch);

        final LinearLayout linkCard = sectionCard("LAN link cable");
        linkCard.addView(activity.text(
                "Connect two devices running compatible Game Boy games over the same network. " +
                "Use the in-game menu to host, join, or discover a host.",
                15, Color.DKGRAY));
        final EditText host = linkField("Host address",
                LibraryActivity.nativeLinkRemoteHost(settingsDirectory),
                InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        final EditText bind = linkField("Host bind address",
                LibraryActivity.nativeLinkRemoteBind(settingsDirectory),
                InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        final EditText port = linkField("TCP port",
                Integer.toString(LibraryActivity.nativeLinkRemotePort(
                        settingsDirectory)), InputType.TYPE_CLASS_NUMBER);
        linkCard.addView(host);
        linkCard.addView(bind);
        linkCard.addView(port);
        final Switch discovery = new Switch(activity);
        discovery.setText("Advertise and discover hosts on the LAN");
        discovery.setTextSize(16);
        discovery.setPadding(0, activity.dp(8), 0, activity.dp(8));
        discovery.setChecked(LibraryActivity.nativeLinkLanDiscovery(
                settingsDirectory));
        linkCard.addView(discovery);
        final Button saveLink = new Button(activity);
        saveLink.setText("Save link settings");
        saveLink.setOnClickListener(view -> {
            final int selectedPort;
            try {
                selectedPort = Integer.parseInt(port.getText().toString().trim());
            } catch (NumberFormatException error) {
                Toast.makeText(activity, "TCP port must be 1–65535",
                        Toast.LENGTH_SHORT).show();
                return;
            }
            if (selectedPort < 1 || selectedPort > 65535 ||
                    host.getText().toString().trim().isEmpty() ||
                    bind.getText().toString().trim().isEmpty()) {
                Toast.makeText(activity, "Enter host, bind address, and a valid port",
                        Toast.LENGTH_SHORT).show();
                return;
            }
            LibraryActivity.nativeSetLinkSettings(settingsDirectory,
                    host.getText().toString().trim(),
                    bind.getText().toString().trim(), selectedPort,
                    discovery.isChecked());
            Toast.makeText(activity, "Link settings saved",
                    Toast.LENGTH_SHORT).show();
        });
        linkCard.addView(saveLink);
    }

    private EditText linkField(String hint, String value, int inputType) {
        final EditText field = new EditText(activity);
        field.setHint(hint);
        field.setText(value == null ? "" : value);
        field.setSingleLine(true);
        field.setInputType(inputType);
        field.setPadding(0, activity.dp(8), 0, activity.dp(4));
        return field;
    }

    private LinearLayout sectionCard(String title) {
        final LinearLayout card = new LinearLayout(activity);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(activity.dp(16), activity.dp(14),
                activity.dp(16), activity.dp(16));
        final GradientDrawable background = new GradientDrawable();
        background.setColor(Color.WHITE);
        background.setCornerRadius(activity.dp(12));
        card.setBackground(background);
        card.setElevation(activity.dp(2));
        final LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        params.bottomMargin = activity.dp(14);
        content.addView(card, params);
        final TextView heading = activity.text(title, 18,
                Color.rgb(24, 29, 39));
        heading.setTypeface(null, android.graphics.Typeface.BOLD);
        card.addView(heading);
        return card;
    }

    private TextView settingLabel(String value) {
        final TextView label = activity.text(value, 14, Color.DKGRAY);
        label.setPadding(0, activity.dp(16), 0, activity.dp(4));
        return label;
    }

    private void showTouchLayoutEditor() {
        final String settingsDirectory = activity.getFilesDir().getAbsolutePath();
        final float[] initial = LibraryActivity.nativeTouchControlLayout(
                settingsDirectory);
        final TouchLayoutView editor = new TouchLayoutView(activity,
                initial == null ? TouchLayoutView.defaultLayout() : initial);
        final LinearLayout container = new LinearLayout(activity);
        container.setOrientation(LinearLayout.VERTICAL);
        final Spinner orientation = new Spinner(activity);
        orientation.setAdapter(new ArrayAdapter<>(activity,
                android.R.layout.simple_spinner_dropdown_item,
                new String[]{"Portrait layout", "Landscape layout"}));
        final boolean startsLandscape = activity.getResources().getConfiguration()
                .orientation == 2;
        editor.setLandscape(startsLandscape);
        orientation.setSelection(startsLandscape ? 1 : 0);
        orientation.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view,
                                       int position, long id) {
                editor.setLandscape(position == 1);
                final ViewGroup.LayoutParams params = editor.getLayoutParams();
                if (params != null) {
                    params.height = activity.dp(position == 1 ? 280 : 420);
                    editor.setLayoutParams(params);
                }
            }
            @Override public void onNothingSelected(AdapterView<?> parent) {}
        });
        container.addView(orientation, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        final int editorHeight = startsLandscape ? activity.dp(280) : activity.dp(420);
        container.addView(editor, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, editorHeight));
        final TextView help = activity.text(
                "Tap a control, then drag it. The highlighted area is larger " +
                "than the visible button so it is easy to position precisely.",
                13, Color.GRAY);
        help.setPadding(0, activity.dp(8), 0, 0);
        container.addView(help);
        final Button reset = new Button(activity);
        reset.setText("Reset positions");
        reset.setOnClickListener(view ->
                editor.setLayout(TouchLayoutView.defaultLayout()));
        container.addView(reset);
        final ScrollView scroll = new ScrollView(activity);
        scroll.setFillViewport(false);
        scroll.addView(container, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        final AlertDialog dialog = new AlertDialog.Builder(activity)
                .setTitle("Customize touch controls")
                .setView(scroll)
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Save", null)
                .create();
        dialog.setOnShowListener(ignored -> dialog.getButton(
                AlertDialog.BUTTON_POSITIVE).setOnClickListener(view -> {
                    LibraryActivity.nativeSetTouchControlLayout(
                            settingsDirectory, editor.getLayout());
                    dialog.dismiss();
                }));
        dialog.show();
        final Window window = dialog.getWindow();
        if (window != null) {
            window.setLayout(ViewGroup.LayoutParams.MATCH_PARENT,
                    Math.round(activity.getResources().getDisplayMetrics()
                            .heightPixels * 0.9f));
        }
    }

    private int currentPalette() {
        return PaletteSettings.read(activity.getFilesDir(), PALETTE_IDS);
    }

    private void savePalette(int position) {
        if (position < 0 || position >= PALETTE_IDS.length) return;
        if (!PaletteSettings.write(activity.getFilesDir(), PALETTE_IDS[position])) {
            Toast.makeText(activity, "Could not save display setting",
                    Toast.LENGTH_SHORT).show();
        }
    }

    private int currentVideoMode() {
        try {
            final String current = LibraryActivity.nativeVideoMode(
                    activity.getFilesDir().getAbsolutePath());
            for (int index = 0; index < VIDEO_MODE_IDS.length; ++index) {
                if (VIDEO_MODE_IDS[index].equals(current)) return index;
            }
        } catch (Exception ignored) {
        }
        return 0;
    }

    private void saveVideoMode(int position) {
        if (position < 0 || position >= VIDEO_MODE_IDS.length) return;
        try {
            LibraryActivity.nativeSetVideoMode(
                    activity.getFilesDir().getAbsolutePath(),
                    VIDEO_MODE_IDS[position]);
        } catch (Exception error) {
            Toast.makeText(activity, "Could not save video setting",
                    Toast.LENGTH_SHORT).show();
        }
    }
}
