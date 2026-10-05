package com.danielseim.gbb;

/** Startup preferences change the next core construction, never a running core. */
final class StartupSettingModel {
    static final String[] IDS = {"instant", "replacement-dmg", "animated-dmg"};
    static final String[] NAMES = {"Instant startup", "GBB fast boot", "GBB animated boot"};
    interface Store {
        String read();
        void write(String mode);
    }
    private final Store store;
    private int selection;
    StartupSettingModel(Store store) {
        this.store = store;
        final String saved = store.read();
        for (int i = 0; i < IDS.length; ++i) if (IDS[i].equals(saved)) selection = i;
    }
    int selection() { return selection; }
    boolean select(int index) {
        if (index < 0 || index >= IDS.length || index == selection) return false;
        store.write(IDS[index]);
        selection = index;
        return true;
    }
}
