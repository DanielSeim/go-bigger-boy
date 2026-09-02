package com.danielseim.gbb;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.nio.file.Files;

import org.junit.Test;

public final class PaletteSettingsTest {
    private static final String[] IDS = {
            "grayscale", "classic", "pocket", "amber", "cgb-auto"
    };

    @Test
    public void readsPaletteFromCanonicalSettingsFile() throws IOException {
        final File directory = Files.createTempDirectory("gbb-settings").toFile();
        try (FileWriter output = new FileWriter(new File(directory, "settings.ini"))) {
            output.write("# palette = grayscale\n");
            output.write("palette = amber ; selected by the user\n");
            output.write("video.Mode = nearest\n");
        }
        assertEquals(3, PaletteSettings.read(directory, IDS));
        delete(directory);
    }

    @Test
    public void writesPaletteWithoutDiscardingOtherSettings() throws IOException {
        final File directory = Files.createTempDirectory("gbb-settings").toFile();
        final File settings = new File(directory, "settings.ini");
        try (FileWriter output = new FileWriter(settings)) {
            output.write("palette = grayscale\nvideo.Mode = voxel\n");
            output.write("touch.VoxelOrbit = false\n");
        }
        assertTrue(PaletteSettings.write(directory, "classic"));
        assertEquals(1, PaletteSettings.read(directory, IDS));
        final String contents = new String(Files.readAllBytes(settings.toPath()));
        assertTrue(contents.contains("video.Mode = voxel"));
        assertTrue(contents.contains("touch.VoxelOrbit = false"));
        assertFalse(contents.contains("palette = grayscale"));
        delete(directory);
    }

    @Test
    public void createsSettingsFileWhenItDoesNotExist() throws IOException {
        final File directory = Files.createTempDirectory("gbb-settings").toFile();
        assertTrue(PaletteSettings.write(directory, "pocket"));
        assertEquals(2, PaletteSettings.read(directory, IDS));
        delete(directory);
    }

    private static void delete(File file) {
        if (file.isDirectory()) {
            final File[] children = file.listFiles();
            if (children != null) {
                for (File child : children) delete(child);
            }
        }
        file.delete();
    }
}
