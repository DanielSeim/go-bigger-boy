package com.danielseim.gbb;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/** Small, device-independent settings.ini palette reader/writer. */
final class PaletteSettings {
    private PaletteSettings() {}

    static int read(File directory, String[] paletteIds) {
        if (directory == null || paletteIds == null) return 0;
        final File settings = new File(directory, "settings.ini");
        try (BufferedReader input = new BufferedReader(new InputStreamReader(
                new FileInputStream(settings), StandardCharsets.UTF_8))) {
            String line;
            while ((line = input.readLine()) != null) {
                final String value = settingValue(line, "palette");
                if (value == null) continue;
                for (int index = 0; index < paletteIds.length; ++index) {
                    if (paletteIds[index].equals(value)) return index;
                }
                return 0;
            }
        } catch (IOException ignored) {
        }
        return 0;
    }

    static boolean write(File directory, String paletteId) {
        if (directory == null || paletteId == null || paletteId.isEmpty()) {
            return false;
        }
        if (!directory.exists() && !directory.mkdirs()) return false;
        final File settings = new File(directory, "settings.ini");
        final List<String> lines = new ArrayList<>();
        boolean replaced = false;
        try {
            if (settings.isFile()) {
                try (BufferedReader input = new BufferedReader(new InputStreamReader(
                        new FileInputStream(settings), StandardCharsets.UTF_8))) {
                    String line;
                    while ((line = input.readLine()) != null) {
                        if (settingValue(line, "palette") != null) {
                            line = "palette = " + paletteId;
                            replaced = true;
                        }
                        lines.add(line);
                    }
                }
            }
            if (!replaced) lines.add("palette = " + paletteId);
            final File temporary = new File(directory, "settings.ini.tmp");
            try (BufferedWriter output = new BufferedWriter(new OutputStreamWriter(
                    new FileOutputStream(temporary, false), StandardCharsets.UTF_8))) {
                for (String line : lines) {
                    output.write(line);
                    output.newLine();
                }
            }
            if (settings.exists() && !settings.delete()) return false;
            if (!temporary.renameTo(settings)) {
                temporary.delete();
                return false;
            }
            return true;
        } catch (IOException ignored) {
            return false;
        }
    }

    private static String settingValue(String line, String expectedKey) {
        final int comment = firstComment(line);
        final String content = line.substring(0, comment).trim();
        final int separator = content.indexOf('=');
        if (separator < 0 || !content.substring(0, separator).trim()
                .equals(expectedKey)) return null;
        return content.substring(separator + 1).trim();
    }

    private static int firstComment(String line) {
        final int hash = line.indexOf('#');
        final int semicolon = line.indexOf(';');
        if (hash < 0) return semicolon < 0 ? line.length() : semicolon;
        if (semicolon < 0) return hash;
        return Math.min(hash, semicolon);
    }
}
