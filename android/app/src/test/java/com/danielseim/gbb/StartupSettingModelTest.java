package com.danielseim.gbb;
import org.junit.Test;
import static org.junit.Assert.*;

public final class StartupSettingModelTest {
    static final class Store implements StartupSettingModel.Store {
        String value = "instant";
        int writes;
        public String read() { return value; }
        public void write(String value) { this.value = value; ++writes; }
    }
    @Test public void restoresWithoutWritingAndCanReturnToInitialChoice() {
        Store store = new Store();
        store.value = "replacement-dmg";
        StartupSettingModel model = new StartupSettingModel(store);
        assertEquals(1, model.selection());
        assertFalse(model.select(1));
        assertEquals(0, store.writes);
        assertTrue(model.select(0));
        assertEquals("instant", store.value);
        assertTrue(model.select(1));
        assertEquals("replacement-dmg", store.value);
        assertEquals(2, store.writes);
        assertFalse(model.select(-1));
        assertFalse(model.select(3));
    }
    @Test public void unknownValueUsesInstantWithoutRewriting() {
        Store store = new Store();
        store.value = "unknown";
        StartupSettingModel model = new StartupSettingModel(store);
        assertEquals(0, model.selection());
        assertEquals(0, store.writes);
    }
    @Test public void animatedBootRestoresAndPersists() {
        Store store = new Store();
        store.value = "animated-dmg";
        StartupSettingModel model = new StartupSettingModel(store);
        assertEquals(2, model.selection());
        assertFalse(model.select(2));
        assertTrue(model.select(0));
        assertTrue(model.select(2));
        assertEquals("animated-dmg", store.value);
        assertEquals(2, store.writes);
    }
    @Test public void failedWriteDoesNotChangeSelection() {
        StartupSettingModel model = new StartupSettingModel(new StartupSettingModel.Store() {
            public String read() { return "instant"; }
            public void write(String mode) { throw new IllegalStateException("failed"); }
        });
        try { model.select(1); fail("expected failure"); } catch (IllegalStateException expected) {}
        assertEquals(0, model.selection());
    }
}
