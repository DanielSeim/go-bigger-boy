package com.danielseim.gbb;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public final class AndroidUpdateManagerTest {
    @Test
    public void recognizesGooglePlayInstaller() {
        assertTrue(AndroidUpdateManager.isPlayStoreInstaller("com.android.vending"));
    }

    @Test
    public void keepsDirectInstallersOnDirectUpdateChannel() {
        assertFalse(AndroidUpdateManager.isPlayStoreInstaller(null));
        assertFalse(AndroidUpdateManager.isPlayStoreInstaller("com.google.android.packageinstaller"));
        assertFalse(AndroidUpdateManager.isPlayStoreInstaller("com.example.store"));
    }
}
