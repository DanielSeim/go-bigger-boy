package com.danielseim.gbb;

/** Presentation and persistence contract for the audio-generation switch. */
final class AudioSettingModel {
    static final String LABEL = "Generate audio";
    static final String DESCRIPTION =
            "Disable audio generation to keep the emulator quiet and reduce CPU use.";

    interface Store {
        boolean read();
        void write(boolean enabled);
    }

    private final Store store;
    private boolean enabled;

    AudioSettingModel(Store store) {
        this.store = store;
        enabled = store.read();
    }

    boolean isEnabled() {
        return enabled;
    }

    void setEnabled(boolean enabled) {
        if (this.enabled == enabled) return;
        this.enabled = enabled;
        store.write(enabled);
    }
}
