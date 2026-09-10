package com.danielseim.gbb;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public final class AudioSettingModelTest {
    private static final class FakeStore implements AudioSettingModel.Store {
        boolean enabled;
        int writes;

        @Override
        public boolean read() {
            return enabled;
        }

        @Override
        public void write(boolean enabled) {
            this.enabled = enabled;
            ++writes;
        }
    }

    @Test
    public void switchUsesPersistedValueOnConstruction() {
        final FakeStore store = new FakeStore();
        store.enabled = false;
        final AudioSettingModel model = new AudioSettingModel(store);

        assertFalse(model.isEnabled());
        assertEquals(0, store.writes);
    }

    @Test
    public void switchPersistsOnlyChangedValues() {
        final FakeStore store = new FakeStore();
        store.enabled = true;
        final AudioSettingModel model = new AudioSettingModel(store);

        model.setEnabled(true);
        assertTrue(model.isEnabled());
        assertEquals(0, store.writes);

        model.setEnabled(false);
        assertFalse(model.isEnabled());
        assertFalse(store.enabled);
        assertEquals(1, store.writes);

        model.setEnabled(true);
        assertTrue(model.isEnabled());
        assertTrue(store.enabled);
        assertEquals(2, store.writes);
    }

    @Test
    public void switchPresentationRemainsStable() {
        assertEquals("Generate audio", AudioSettingModel.LABEL);
        assertTrue(AudioSettingModel.DESCRIPTION.contains("quiet"));
        assertTrue(AudioSettingModel.DESCRIPTION.contains("CPU"));
    }
}
