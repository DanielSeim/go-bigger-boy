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
    public void recognizesPlayAsInstallingPackage() {
        assertTrue(AndroidUpdateManager.isPlayStoreSource(
                "com.android.vending", null, null, null));
    }

    @Test
    public void recognizesPlayAsInitiatingPackage() {
        assertTrue(AndroidUpdateManager.isPlayStoreSource(
                null, "com.android.vending", null, null));
    }

    @Test
    public void recognizesPlayAsOriginatingPackage() {
        assertTrue(AndroidUpdateManager.isPlayStoreSource(
                null, null, "com.android.vending", null));
    }

    @Test
    public void recognizesPlayAsUpdateOwner() {
        assertTrue(AndroidUpdateManager.isPlayStoreSource(
                null, null, null, "com.android.vending"));
    }

    @Test
    public void keepsDirectInstallersOnDirectUpdateChannel() {
        assertFalse(AndroidUpdateManager.isPlayStoreSource(
                null, null, null, null));
        assertFalse(AndroidUpdateManager.isPlayStoreSource(
                "com.google.android.packageinstaller", null, null, null));
        assertFalse(AndroidUpdateManager.isPlayStoreSource(
                null, "com.example.store", null, null));
    }
}
