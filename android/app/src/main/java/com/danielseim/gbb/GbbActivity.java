package com.danielseim.gbb;

import android.annotation.SuppressLint;
import android.app.AlertDialog;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothServerSocket;
import android.bluetooth.BluetoothSocket;
import android.graphics.Color;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.hardware.SensorManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Looper;
import android.net.wifi.WifiManager;
import android.view.OrientationEventListener;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.text.InputType;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.CheckBox;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.UUID;
import java.util.Locale;
import java.util.concurrent.ConcurrentLinkedQueue;
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

    // Bluetooth Classic RFCOMM data path. The worker owns all blocking socket
    // operations; native code only observes the state and exchanges complete
    // 11-byte protocol frames through these queues.
    private final ConcurrentLinkedQueue<byte[]> bluetoothIncoming =
            new ConcurrentLinkedQueue<>();
    private final ConcurrentLinkedQueue<byte[]> bluetoothOutgoing =
            new ConcurrentLinkedQueue<>();
    private volatile BluetoothServerSocket bluetoothServer;
    private volatile BluetoothSocket bluetoothSocket;
    private volatile Thread bluetoothThread;
    private volatile boolean bluetoothStopping;
    private volatile int bluetoothLinkState;
    private final Object bluetoothLifecycleLock = new Object();
    private long bluetoothGeneration;
    private static final int BLUETOOTH_PERMISSION_REQUEST = 47;
    private static final int LAN_PERMISSION_REQUEST = 48;
    private final Object lanDiscoveryLock = new Object();
    private WifiManager.MulticastLock lanMulticastLock;

    private static native void nativeOpenRom(String rom, String displayName);
    private static native void nativeAndroidBackPressed();
    private static native void nativeAndroidLinkSettingsChanged();

    /**
     * Opens the link configuration without leaving the running game. The
     * native SDL menu invokes this method through the activity instance so
     * Android supplies a real text-input dialog and soft keyboard.
     */
    public void showLinkSettingsDialog() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            runOnUiThread(this::showLinkSettingsDialog);
            return;
        }
        final String directory = getFilesDir().getAbsolutePath();
        final LinearLayout form = new LinearLayout(this);
        form.setOrientation(LinearLayout.VERTICAL);
        final int padding = Math.round(20 *
                getResources().getDisplayMetrics().density);
        form.setPadding(padding, 0, padding, 0);
        final ScrollView formScroll = new ScrollView(this);
        formScroll.addView(form);

        final TextView intro = new TextView(this);
        intro.setText("Choose one connection method. TCP is for the same " +
                "Wi-Fi/LAN; Bluetooth uses a paired Bluetooth Classic device.");
        intro.setTextColor(Color.DKGRAY);
        intro.setPadding(0, 0, 0, padding / 2);
        form.addView(intro);

        final TextView transportLabel = new TextView(this);
        transportLabel.setText("Connection method");
        transportLabel.setTextColor(Color.DKGRAY);
        form.addView(transportLabel);
        final Spinner transport = new Spinner(this);
        transport.setAdapter(new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_dropdown_item,
                new String[]{"TCP (Wi-Fi / LAN)", "Bluetooth Classic"}));
        transport.setSelection("bluetooth".equalsIgnoreCase(
                LibraryActivity.nativeLinkTransport(directory)) ? 1 : 0);
        form.addView(transport);

        final TextView tcpHeading = new TextView(this);
        tcpHeading.setText("TCP connection");
        tcpHeading.setTextColor(Color.rgb(24, 29, 39));
        tcpHeading.setTypeface(null, android.graphics.Typeface.BOLD);
        tcpHeading.setPadding(0, padding / 2, 0, 0);
        form.addView(tcpHeading);
        final TextView tcpDescription = new TextView(this);
        tcpDescription.setText("The joiner enters the host computer's LAN " +
                "address. The host usually binds to 0.0.0.0 for discovery.");
        tcpDescription.setTextColor(Color.GRAY);
        form.addView(tcpDescription);
        final EditText host = linkField("Host address",
                LibraryActivity.nativeLinkRemoteHost(directory),
                InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        final EditText bind = linkField("Host bind address",
                LibraryActivity.nativeLinkRemoteBind(directory),
                InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        final EditText port = linkField("TCP port",
                Integer.toString(LibraryActivity.nativeLinkRemotePort(directory)),
                InputType.TYPE_CLASS_NUMBER);
        form.addView(host);
        form.addView(bind);
        form.addView(port);

        final CheckBox discovery = new CheckBox(this);
        discovery.setText("Advertise this host for LAN discovery");
        discovery.setTextColor(Color.DKGRAY);
        discovery.setChecked(LibraryActivity.nativeLinkLanDiscovery(directory));
        form.addView(discovery);

        final TextView bluetoothHeading = new TextView(this);
        bluetoothHeading.setText("Bluetooth connection");
        bluetoothHeading.setTextColor(Color.rgb(24, 29, 39));
        bluetoothHeading.setTypeface(null, android.graphics.Typeface.BOLD);
        bluetoothHeading.setPadding(0, padding / 2, 0, 0);
        form.addView(bluetoothHeading);
        final TextView bluetoothDescription = new TextView(this);
        bluetoothDescription.setText("Pair the devices first. The host does " +
                "not need an address; the joiner enters the host adapter address.");
        bluetoothDescription.setTextColor(Color.GRAY);
        form.addView(bluetoothDescription);
        final EditText bluetoothAddress = linkField("Bluetooth device address",
                LibraryActivity.nativeLinkBluetoothAddress(directory),
                InputType.TYPE_CLASS_TEXT);
        final EditText bluetoothUuid = linkField("Bluetooth service UUID",
                LibraryActivity.nativeLinkBluetoothServiceUuid(directory),
                InputType.TYPE_CLASS_TEXT);
        form.addView(bluetoothAddress);
        final Button chooseBluetooth = new Button(this);
        chooseBluetooth.setText("Choose paired Bluetooth device");
        chooseBluetooth.setOnClickListener(view -> chooseBluetoothDevice(bluetoothAddress));
        form.addView(chooseBluetooth);
        form.addView(bluetoothUuid);

        final AdapterView.OnItemSelectedListener transportListener =
                new AdapterView.OnItemSelectedListener() {
                    @Override public void onItemSelected(AdapterView<?> parent,
                                                         View view, int position,
                                                         long id) {
                        final boolean bluetooth = position == 1;
                        final int tcpVisibility = bluetooth ? View.GONE : View.VISIBLE;
                        tcpHeading.setVisibility(tcpVisibility);
                        tcpDescription.setVisibility(tcpVisibility);
                        host.setVisibility(tcpVisibility);
                        bind.setVisibility(tcpVisibility);
                        port.setVisibility(tcpVisibility);
                        discovery.setVisibility(tcpVisibility);
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

        final AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("Link settings")
                .setView(formScroll)
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Save", null)
                .create();
        dialog.setOnShowListener(ignored -> dialog.getButton(
                AlertDialog.BUTTON_POSITIVE).setOnClickListener(view -> {
                    final boolean bluetooth = transport.getSelectedItemPosition() == 1;
                    int selectedPort = LibraryActivity.nativeLinkRemotePort(directory);
                    if (!bluetooth) try {
                        selectedPort = Integer.parseInt(
                                port.getText().toString().trim());
                    } catch (NumberFormatException error) {
                        port.setError("Enter a port from 1 to 65535");
                        return;
                    }
                    final String hostValue = host.getText().toString().trim();
                    final String bindValue = bind.getText().toString().trim();
                    final String addressValue = bluetoothAddress.getText().toString().trim();
                    final String uuidValue = bluetoothUuid.getText().toString().trim();
                    boolean validUuid = false;
                    try {
                        UUID.fromString(uuidValue);
                        validUuid = true;
                    } catch (IllegalArgumentException error) {
                        // Keep the default false for malformed UUID input.
                    }
                    if ((!bluetooth && (selectedPort < 1 || selectedPort > 65535 ||
                            hostValue.isEmpty() || bindValue.isEmpty())) ||
                            (bluetooth && !validUuid)) {
                        Toast.makeText(this,
                                bluetooth ? "Enter a valid service UUID"
                                          : "Enter a host, bind address, and port",
                                Toast.LENGTH_SHORT).show();
                        return;
                    }
                    LibraryActivity.nativeSetLinkSettings(
                            directory, hostValue, bindValue, selectedPort,
                            !bluetooth && discovery.isChecked());
                    LibraryActivity.nativeSetBluetoothLinkSettings(
                            directory, bluetooth ? "bluetooth" : "tcp",
                            addressValue, uuidValue);
                    nativeAndroidLinkSettingsChanged();
                    Toast.makeText(this, "Link settings saved",
                            Toast.LENGTH_SHORT).show();
                    dialog.dismiss();
                }));
        dialog.show();
    }

    private boolean bluetoothPermission(String permission) {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.M ||
                checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
    }

    private boolean ensureBluetoothPermissions(boolean host) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return true;
        final java.util.ArrayList<String> missing = new java.util.ArrayList<>();
        if (!bluetoothPermission(android.Manifest.permission.BLUETOOTH_CONNECT))
            missing.add(android.Manifest.permission.BLUETOOTH_CONNECT);
        if (host && !bluetoothPermission(android.Manifest.permission.BLUETOOTH_ADVERTISE))
            missing.add(android.Manifest.permission.BLUETOOTH_ADVERTISE);
        if (missing.isEmpty()) return true;
        runOnUiThread(() -> {
            requestPermissions(missing.toArray(new String[0]),
                    BLUETOOTH_PERMISSION_REQUEST);
            Toast.makeText(this, "Allow Bluetooth, then retry the link action",
                    Toast.LENGTH_LONG).show();
        });
        return false;
    }

    /**
     * Enables reception of LAN discovery broadcasts for the native UDP
     * socket. Android Wi-Fi normally filters multicast/broadcast traffic;
     * keeping this lock scoped to an active link session avoids a permanent
     * battery cost. Android 16's opt-in local-network protection also maps
     * raw LAN sockets to the Nearby devices permission.
     */
    public boolean startLanDiscovery() {
        if (Build.VERSION.SDK_INT >= 36 &&
                checkSelfPermission(android.Manifest.permission.NEARBY_WIFI_DEVICES) !=
                        PackageManager.PERMISSION_GRANTED) {
            runOnUiThread(() -> {
                requestPermissions(new String[]{
                        android.Manifest.permission.NEARBY_WIFI_DEVICES},
                        LAN_PERMISSION_REQUEST);
                Toast.makeText(this,
                        "Allow Nearby devices, then retry the link action",
                        Toast.LENGTH_LONG).show();
            });
            return false;
        }
        synchronized (lanDiscoveryLock) {
            if (lanMulticastLock != null && lanMulticastLock.isHeld()) return true;
            final WifiManager manager =
                    (WifiManager)getApplicationContext().getSystemService(WIFI_SERVICE);
            if (manager == null) return false;
            try {
                final WifiManager.MulticastLock lock =
                        manager.createMulticastLock("gbb-lan-discovery");
                lock.setReferenceCounted(false);
                lock.acquire();
                lanMulticastLock = lock;
                return true;
            } catch (RuntimeException error) {
                lanMulticastLock = null;
                return false;
            }
        }
    }

    /** Releases the LAN discovery Wi-Fi lock, if one is held. */
    public void stopLanDiscovery() {
        synchronized (lanDiscoveryLock) {
            if (lanMulticastLock == null) return;
            try {
                if (lanMulticastLock.isHeld()) lanMulticastLock.release();
            } catch (RuntimeException ignored) { }
            lanMulticastLock = null;
        }
    }

    private void chooseBluetoothDevice(EditText target) {
        if (!ensureBluetoothPermissions(false)) return;
        final BluetoothManager manager =
                (BluetoothManager)getSystemService(BLUETOOTH_SERVICE);
        final BluetoothAdapter adapter = manager == null ? null : manager.getAdapter();
        if (adapter == null || !adapter.isEnabled()) {
            Toast.makeText(this, "Enable Bluetooth first", Toast.LENGTH_SHORT).show();
            return;
        }
        final java.util.ArrayList<BluetoothDevice> devices =
                new java.util.ArrayList<>(adapter.getBondedDevices());
        if (devices.isEmpty()) {
            Toast.makeText(this, "Pair the other device in Android settings first",
                    Toast.LENGTH_LONG).show();
            return;
        }
        final String[] labels = new String[devices.size()];
        for (int index = 0; index < devices.size(); ++index) {
            final BluetoothDevice device = devices.get(index);
            labels[index] = device.getName() + "\n" + device.getAddress();
        }
        new AlertDialog.Builder(this)
                .setTitle("Choose paired device")
                .setItems(labels, (dialog, which) ->
                        target.setText(devices.get(which).getAddress()))
                .setNegativeButton("Cancel", null)
                .show();
    }

    /** Starts an RFCOMM host. Called from the native link channel. */
    public boolean bluetoothStartHost(String serviceUuid) {
        if (!ensureBluetoothPermissions(true)) return false;
        final BluetoothManager manager = (BluetoothManager)getSystemService(BLUETOOTH_SERVICE);
        final BluetoothAdapter adapter = manager == null ? null : manager.getAdapter();
        if (adapter == null || !adapter.isEnabled()) return false;
        bluetoothStop();
        synchronized (bluetoothLifecycleLock) {
            bluetoothStopping = false;
            final long generation = ++bluetoothGeneration;
            bluetoothLinkState = 1;
            bluetoothThread = new Thread(() -> bluetoothWorker(
                    adapter, true, null, serviceUuid, generation),
                    "GBB-Bluetooth-Link");
            bluetoothThread.setDaemon(true);
            bluetoothThread.start();
        }
        return true;
    }

    /** Starts an RFCOMM join to a paired device address. */
    public boolean bluetoothStartJoin(String address, String serviceUuid) {
        if (!ensureBluetoothPermissions(false)) return false;
        final BluetoothManager manager = (BluetoothManager)getSystemService(BLUETOOTH_SERVICE);
        final BluetoothAdapter adapter = manager == null ? null : manager.getAdapter();
        if (adapter == null || !adapter.isEnabled() || address == null || address.isEmpty())
            return false;
        bluetoothStop();
        synchronized (bluetoothLifecycleLock) {
            bluetoothStopping = false;
            final long generation = ++bluetoothGeneration;
            bluetoothLinkState = 2;
            bluetoothThread = new Thread(() -> bluetoothWorker(
                    adapter, false, address, serviceUuid, generation),
                    "GBB-Bluetooth-Link");
            bluetoothThread.setDaemon(true);
            bluetoothThread.start();
        }
        return true;
    }

    public int bluetoothState() { return bluetoothLinkState; }

    public boolean bluetoothSend(byte[] bytes) {
        if (bytes == null || bluetoothLinkState != 3) return false;
        bluetoothOutgoing.offer(bytes.clone());
        return true;
    }

    public byte[] bluetoothReceive() { return bluetoothIncoming.poll(); }

    public void bluetoothStop() {
        final BluetoothServerSocket server;
        final BluetoothSocket socket;
        final Thread thread;
        synchronized (bluetoothLifecycleLock) {
            bluetoothStopping = true;
            ++bluetoothGeneration;
            bluetoothLinkState = 0;
            server = bluetoothServer;
            socket = bluetoothSocket;
            thread = bluetoothThread;
            bluetoothServer = null;
            bluetoothSocket = null;
            bluetoothThread = null;
        }
        try { if (server != null) server.close(); } catch (IOException ignored) { }
        try { if (socket != null) socket.close(); } catch (IOException ignored) { }
        if (thread != null && thread != Thread.currentThread()) thread.interrupt();
        bluetoothIncoming.clear();
        bluetoothOutgoing.clear();
    }

    private boolean bluetoothSessionCurrent(long generation) {
        synchronized (bluetoothLifecycleLock) {
            return !bluetoothStopping && bluetoothGeneration == generation;
        }
    }

    private void bluetoothWorker(BluetoothAdapter adapter, boolean host,
                                 String address, String serviceUuid,
                                 long generation) {
        BluetoothSocket socket = null;
        try {
            final UUID uuid = UUID.fromString(serviceUuid);
            if (host) {
                final BluetoothServerSocket server =
                        adapter.listenUsingRfcommWithServiceRecord(
                                "Go Bigger Boy Link", uuid);
                synchronized (bluetoothLifecycleLock) {
                    if (!bluetoothSessionCurrent(generation)) {
                        server.close();
                        return;
                    }
                    bluetoothServer = server;
                    bluetoothLinkState = 1;
                }
                socket = server.accept();
                server.close();
                synchronized (bluetoothLifecycleLock) {
                    if (bluetoothGeneration == generation) bluetoothServer = null;
                }
            } else {
                final BluetoothDevice device = adapter.getRemoteDevice(address);
                socket = device.createRfcommSocketToServiceRecord(uuid);
                synchronized (bluetoothLifecycleLock) {
                    if (!bluetoothSessionCurrent(generation)) {
                        socket.close();
                        return;
                    }
                    bluetoothSocket = socket;
                    bluetoothLinkState = 2;
                }
                socket.connect();
            }
            synchronized (bluetoothLifecycleLock) {
                if (!bluetoothSessionCurrent(generation)) {
                    socket.close();
                    return;
                }
                bluetoothSocket = socket;
                bluetoothLinkState = 3;
            }
            final InputStream input = socket.getInputStream();
            final OutputStream output = socket.getOutputStream();
            final ByteArrayOutputStream pending = new ByteArrayOutputStream();
            final byte[] readBuffer = new byte[1024];
            while (bluetoothSessionCurrent(generation)) {
                final int available = input.available();
                if (available > 0) {
                    final int count = input.read(readBuffer, 0,
                            Math.min(available, readBuffer.length));
                    if (count < 0) break;
                    pending.write(readBuffer, 0, count);
                    final byte[] bytes = pending.toByteArray();
                    int offset = 0;
                    while (bytes.length - offset >= 11) {
                        final byte[] frame = new byte[11];
                        System.arraycopy(bytes, offset, frame, 0, 11);
                        bluetoothIncoming.offer(frame);
                        offset += 11;
                    }
                    pending.reset();
                    if (offset < bytes.length) pending.write(bytes, offset,
                            bytes.length - offset);
                }
                byte[] outgoing;
                while ((outgoing = bluetoothOutgoing.poll()) != null) {
                    output.write(outgoing);
                    output.flush();
                }
                Thread.sleep(2);
            }
        } catch (Exception error) {
            if (bluetoothSessionCurrent(generation)) bluetoothLinkState = 4;
        } finally {
            try { if (socket != null) socket.close(); } catch (IOException ignored) { }
            synchronized (bluetoothLifecycleLock) {
                if (bluetoothGeneration == generation) {
                    bluetoothSocket = null;
                    bluetoothThread = null;
                    if (bluetoothStopping) bluetoothLinkState = 0;
                    else if (bluetoothLinkState == 3) bluetoothLinkState = 4;
                }
            }
        }
    }

    private EditText linkField(String hint, String value, int inputType) {
        final EditText field = new EditText(this);
        field.setHint(hint);
        field.setText(value == null ? "" : value);
        field.setSingleLine(true);
        field.setInputType(inputType);
        return field;
    }

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
        stopLanDiscovery();
        bluetoothStop();
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
