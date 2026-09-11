package com.danielseim.gbb;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

/** Covers the Android library's return-to-running-ROM decision. */
public final class RomLaunchPolicyTest {
    @Test
    public void matchingRunningRomResumesOnlyFromReturnToGameLibrary() {
        assertTrue(RomLaunchPolicy.shouldResumeExistingRom(
                true, "/data/user/0/gbb/files/roms/game.gb",
                "/data/user/0/gbb/files/roms/game.gb"));
        assertFalse(RomLaunchPolicy.shouldResumeExistingRom(
                false, "/data/user/0/gbb/files/roms/game.gb",
                "/data/user/0/gbb/files/roms/game.gb"));
        assertFalse(RomLaunchPolicy.shouldResumeExistingRom(
                true, "/data/user/0/gbb/files/roms/other.gb",
                "/data/user/0/gbb/files/roms/game.gb"));
    }
}
