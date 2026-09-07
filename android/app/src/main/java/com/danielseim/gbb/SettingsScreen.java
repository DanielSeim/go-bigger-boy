package com.danielseim.gbb;

import android.app.AlertDialog;
import android.Manifest;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.content.pm.PackageManager;
import android.os.Build;
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

import java.util.UUID;

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

        final LinearLayout dataCard = sectionCard("Data and saves");
        dataCard.addView(activity.text(
                "Back up your private app data to a ZIP file that you can copy " +
                "to a computer. ROMs, battery saves, quick states, settings, " +
                "and the library are included. Save exports preserve the " +
                "fingerprint-based filenames needed to restore them.",
                15, Color.DKGRAY));

        final Button exportBackup = new Button(activity);
        exportBackup.setText("Export full backup (ZIP)");
        exportBackup.setOnClickListener(view -> activity.exportBackup());
        dataCard.addView(exportBackup);

        final Button importBackup = new Button(activity);
        importBackup.setText("Import full backup (ZIP)");
        importBackup.setOnClickListener(view -> new AlertDialog.Builder(activity)
                .setTitle("Restore backup?")
                .setMessage("Existing ROMs, saves, states, and settings with " +
                        "the same names will be replaced.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Restore", (dialog, which) ->
                        activity.importBackup())
                .show());
        dataCard.addView(importBackup);

        final Button exportSaves = new Button(activity);
        exportSaves.setText("Export save files (ZIP)");
        exportSaves.setOnClickListener(view -> activity.exportSaves());
        dataCard.addView(exportSaves);

        final Button importSave = new Button(activity);
        importSave.setText("Import a .sav file");
        importSave.setOnClickListener(view -> activity.importSave());
        dataCard.addView(importSave);

        final TextView saveHelp = activity.text(
                "Imported .sav files keep their filename and are placed in the " +
                "main save folder. For a ROM to use one, the filename must " +
                "match that ROM's fingerprint.", 13, Color.GRAY);
        saveHelp.setPadding(0, activity.dp(4), 0, 0);
        dataCard.addView(saveHelp);

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

        final LinearLayout linkCard = sectionCard("Remote link cable");
        linkCard.addView(activity.text(
                "Choose one connection method. TCP is intended for the same " +
                "Wi-Fi/LAN; Bluetooth uses a paired Bluetooth Classic device. " +
                "Use the in-game link menu to host, join, or discover a host.",
                15, Color.DKGRAY));
        linkCard.addView(settingLabel("Connection method"));
        final Spinner transport = new Spinner(activity);
        transport.setAdapter(new ArrayAdapter<>(activity,
                android.R.layout.simple_spinner_dropdown_item,
                new String[]{"TCP (Wi-Fi / LAN)", "Bluetooth Classic"}));
        transport.setSelection("bluetooth".equalsIgnoreCase(
                LibraryActivity.nativeLinkTransport(settingsDirectory)) ? 1 : 0);
        linkCard.addView(transport, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        final TextView tcpHeading = activity.text("TCP connection", 16,
                Color.rgb(24, 29, 39));
        tcpHeading.setTypeface(null, android.graphics.Typeface.BOLD);
        tcpHeading.setPadding(0, activity.dp(16), 0, 0);
        linkCard.addView(tcpHeading);
        final TextView tcpDescription = activity.text(
                "The joiner enters the host computer's LAN address. The host " +
                "usually binds to 0.0.0.0 when discovery is enabled.",
                14, Color.GRAY);
        linkCard.addView(tcpDescription);
        final EditText host = linkField("Remote host address (joiner)",
                LibraryActivity.nativeLinkRemoteHost(settingsDirectory),
                InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        final EditText bind = linkField("Host bind address",
                LibraryActivity.nativeLinkRemoteBind(settingsDirectory),
                InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        final EditText port = linkField("TCP port (1–65535)",
                Integer.toString(LibraryActivity.nativeLinkRemotePort(
                        settingsDirectory)), InputType.TYPE_CLASS_NUMBER);
        linkCard.addView(host);
        linkCard.addView(bind);
        linkCard.addView(port);

        final Switch discovery = new Switch(activity);
        discovery.setText("Advertise this host for LAN discovery");
        discovery.setTextSize(16);
        discovery.setPadding(0, activity.dp(8), 0, activity.dp(8));
        discovery.setChecked(LibraryActivity.nativeLinkLanDiscovery(
                settingsDirectory));
        linkCard.addView(discovery);

        final TextView bluetoothHeading = activity.text("Bluetooth connection", 16,
                Color.rgb(24, 29, 39));
        bluetoothHeading.setTypeface(null, android.graphics.Typeface.BOLD);
        bluetoothHeading.setPadding(0, activity.dp(16), 0, 0);
        linkCard.addView(bluetoothHeading);
        final TextView bluetoothDescription = activity.text(
                "Pair the devices in Android/Windows first. The host does not " +
                "need an address; the joiner enters the host adapter address.",
                14, Color.GRAY);
        linkCard.addView(bluetoothDescription);
        final EditText bluetoothAddress = linkField(
                "Host Bluetooth address (joiner only)",
                LibraryActivity.nativeLinkBluetoothAddress(settingsDirectory),
                InputType.TYPE_CLASS_TEXT);
        final EditText bluetoothUuid = linkField("Shared service UUID",
                LibraryActivity.nativeLinkBluetoothServiceUuid(settingsDirectory),
                InputType.TYPE_CLASS_TEXT);
        linkCard.addView(bluetoothAddress);
        final Button chooseBluetooth = new Button(activity);
        chooseBluetooth.setText("Choose paired Bluetooth device");
        chooseBluetooth.setOnClickListener(view ->
                chooseBluetoothDevice(bluetoothAddress));
        linkCard.addView(chooseBluetooth);
        linkCard.addView(bluetoothUuid);

        final Button saveLink = new Button(activity);
        saveLink.setText("Save link settings");
        saveLink.setOnClickListener(view -> {
            final boolean bluetooth = transport.getSelectedItemPosition() == 1;
            int selectedPort = LibraryActivity.nativeLinkRemotePort(
                    settingsDirectory);
            if (!bluetooth) try {
                selectedPort = Integer.parseInt(port.getText().toString().trim());
            } catch (NumberFormatException error) {
                port.setError("Enter a port from 1 to 65535");
                return;
            }
            final String hostValue = host.getText().toString().trim();
            final String bindValue = bind.getText().toString().trim();
            final String uuidValue = bluetoothUuid.getText().toString().trim();
            if ((!bluetooth && (selectedPort < 1 || selectedPort > 65535 ||
                    hostValue.isEmpty() || bindValue.isEmpty())) ||
                    (bluetooth && !validServiceUuid(uuidValue))) {
                Toast.makeText(activity,
                        bluetooth ? "Enter a valid service UUID"
                                  : "Enter a host, bind address, and port",
                        Toast.LENGTH_SHORT).show();
                return;
            }
            LibraryActivity.nativeSetLinkSettings(settingsDirectory,
                    hostValue, bindValue, selectedPort,
                    !bluetooth && discovery.isChecked());
            LibraryActivity.nativeSetBluetoothLinkSettings(
                    settingsDirectory, bluetooth ? "bluetooth" : "tcp",
                    bluetoothAddress.getText().toString().trim(), uuidValue);
            Toast.makeText(activity, "Link settings saved",
                    Toast.LENGTH_SHORT).show();
        });
        linkCard.addView(saveLink);

        final AdapterView.OnItemSelectedListener transportListener =
                new AdapterView.OnItemSelectedListener() {
                    @Override public void onItemSelected(AdapterView<?> parent,
                                                         View view, int position,
                                                         long id) {
                        final boolean bluetooth = position == 1;
                        final int visibility = bluetooth ? View.GONE : View.VISIBLE;
                        tcpHeading.setVisibility(visibility);
                        tcpDescription.setVisibility(visibility);
                        host.setVisibility(visibility);
                        bind.setVisibility(visibility);
                        port.setVisibility(visibility);
                        discovery.setVisibility(visibility);
                        bluetoothHeading.setVisibility(
                                bluetooth ? View.VISIBLE : View.GONE);
                        bluetoothDescription.setVisibility(
                                bluetooth ? View.VISIBLE : View.GONE);
                        bluetoothAddress.setVisibility(
                                bluetooth ? View.VISIBLE : View.GONE);
                        chooseBluetooth.setVisibility(
                                bluetooth ? View.VISIBLE : View.GONE);
                        bluetoothUuid.setVisibility(
                                bluetooth ? View.VISIBLE : View.GONE);
                    }
                    @Override public void onNothingSelected(AdapterView<?> parent) {}
                };
        transport.setOnItemSelectedListener(transportListener);
        transportListener.onItemSelected(transport, null,
                transport.getSelectedItemPosition(), 0);
    }

    private static boolean validServiceUuid(String value) {
        try {
            UUID.fromString(value);
            return true;
        } catch (IllegalArgumentException error) {
            return false;
        }
    }

    private void chooseBluetoothDevice(EditText target) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
                activity.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) !=
                        PackageManager.PERMISSION_GRANTED) {
            activity.requestPermissions(new String[]{
                    Manifest.permission.BLUETOOTH_CONNECT}, 47);
            Toast.makeText(activity,
                    "Allow nearby devices, then choose a device again",
                    Toast.LENGTH_LONG).show();
            return;
        }
        final BluetoothManager manager = (BluetoothManager) activity
                .getSystemService(android.content.Context.BLUETOOTH_SERVICE);
        final BluetoothAdapter adapter = manager == null ? null : manager.getAdapter();
        if (adapter == null || !adapter.isEnabled()) {
            Toast.makeText(activity, "Enable Bluetooth first",
                    Toast.LENGTH_SHORT).show();
            return;
        }
        final java.util.ArrayList<BluetoothDevice> devices =
                new java.util.ArrayList<>(adapter.getBondedDevices());
        if (devices.isEmpty()) {
            Toast.makeText(activity,
                    "Pair the other device in Android settings first",
                    Toast.LENGTH_LONG).show();
            return;
        }
        final String[] labels = new String[devices.size()];
        for (int index = 0; index < devices.size(); ++index) {
            final BluetoothDevice device = devices.get(index);
            labels[index] = device.getName() + "\n" + device.getAddress();
        }
        new AlertDialog.Builder(activity)
                .setTitle("Choose paired device")
                .setItems(labels, (dialog, which) ->
                        target.setText(devices.get(which).getAddress()))
                .setNegativeButton("Cancel", null)
                .show();
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
