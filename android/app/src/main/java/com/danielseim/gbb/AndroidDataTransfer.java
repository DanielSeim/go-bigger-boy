package com.danielseim.gbb;

import android.content.Context;
import android.content.ContentResolver;
import android.content.SharedPreferences;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;
import java.util.Map;
import java.util.Properties;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/** Secure import/export of the app's private data through Android's picker. */
final class AndroidDataTransfer {
    private static final int BUFFER_SIZE = 32 * 1024;
    private static final long MAXIMUM_IMPORT_BYTES = 512L * 1024L * 1024L;
    private static final long MAXIMUM_SAVE_BYTES = 32L * 1024L * 1024L;
    private static final int MAXIMUM_ARCHIVE_ENTRIES = 10000;
    private static final String DASHBOARD_PREFERENCES = "dashboard-preferences.properties";

    private AndroidDataTransfer() {}

    static void exportBackup(Context context, Uri destination) throws IOException {
        final ContentResolver resolver = context.getContentResolver();
        try (OutputStream output = resolver.openOutputStream(destination);
             ZipOutputStream zip = new ZipOutputStream(
                     new BufferedOutputStream(requireStream(output)))) {
            addDirectory(zip, context.getFilesDir(), context.getFilesDir(), "");
            addDashboardPreferences(zip, context);
        }
    }

    static void exportSaves(Context context, Uri destination) throws IOException {
        final ContentResolver resolver = context.getContentResolver();
        try (OutputStream output = resolver.openOutputStream(destination);
             ZipOutputStream zip = new ZipOutputStream(
                     new BufferedOutputStream(requireStream(output)))) {
            final File files = context.getFilesDir();
            boolean found = addDirectory(zip, files, new File(files, "saves"),
                    "saves/");
            found |= addDirectory(zip, files, new File(files, "link-saves"),
                    "link-saves/");
            if (!found) throw new IOException("No save files are available yet");
        }
    }

    static String importBackup(Context context, Uri source) throws IOException {
        final ContentResolver resolver = context.getContentResolver();
        long total = 0;
        int entries = 0;
        try (InputStream input = resolver.openInputStream(source);
             ZipInputStream zip = new ZipInputStream(
                     new BufferedInputStream(requireStream(input)))) {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                if (++entries > MAXIMUM_ARCHIVE_ENTRIES) {
                    throw new IOException("Backup contains too many files");
                }
                final File target = safeArchiveTarget(context.getFilesDir(),
                        entry.getName());
                if (DASHBOARD_PREFERENCES.equals(entry.getName())) {
                    restoreDashboardPreferences(context, zip);
                    continue;
                }
                if (entry.isDirectory()) {
                    if (!target.exists() && !target.mkdirs()) {
                        throw new IOException("Could not create " + entry.getName());
                    }
                    continue;
                }
                final File parent = target.getParentFile();
                if (parent != null && !parent.exists() && !parent.mkdirs()) {
                    throw new IOException("Could not create backup directory");
                }
                try (OutputStream output = new BufferedOutputStream(
                        new FileOutputStream(target, false))) {
                    total += copyLimited(zip, output, MAXIMUM_IMPORT_BYTES - total);
                }
                if (total > MAXIMUM_IMPORT_BYTES) {
                    throw new IOException("Backup is too large");
                }
            }
        }
        return "Backup restored (" + entries + " files)";
    }

    static String importSave(Context context, Uri source, String displayName)
            throws IOException {
        String name = sanitizeFileName(displayName);
        if (name.isEmpty()) name = "imported-save.sav";
        if (!name.toLowerCase(Locale.ROOT).endsWith(".sav")) name += ".sav";
        final File directory = new File(context.getFilesDir(), "saves");
        if (!directory.exists() && !directory.mkdirs()) {
            throw new IOException("Could not create save directory");
        }
        final File target = new File(directory, name);
        try (InputStream input = context.getContentResolver().openInputStream(source);
             OutputStream output = new BufferedOutputStream(
                     new FileOutputStream(target, false))) {
            copyLimited(requireStream(input), output, MAXIMUM_SAVE_BYTES);
        }
        return "Imported " + name;
    }

    private static OutputStream requireStream(OutputStream stream) throws IOException {
        if (stream == null) throw new IOException("Could not open destination");
        return stream;
    }

    private static InputStream requireStream(InputStream stream) throws IOException {
        if (stream == null) throw new IOException("Could not open source");
        return stream;
    }

    private static long copyLimited(InputStream input, OutputStream output,
                                    long remaining) throws IOException {
        if (remaining < 0) throw new IOException("File is too large");
        final byte[] buffer = new byte[BUFFER_SIZE];
        long copied = 0;
        int count;
        while ((count = input.read(buffer)) != -1) {
            if (count > remaining - copied) throw new IOException("File is too large");
            output.write(buffer, 0, count);
            copied += count;
        }
        return copied;
    }

    private static boolean addDirectory(ZipOutputStream zip, File root, File current,
                                        String prefix) throws IOException {
        if (!current.exists()) return false;
        if (current.isFile()) {
            addFile(zip, root, current, prefix);
            return true;
        }
        final File[] children = current.listFiles();
        if (children == null) throw new IOException("Could not read " + current);
        boolean found = false;
        for (File child : children) {
            found |= addDirectory(zip, root, child, prefix + child.getName() +
                    (child.isDirectory() ? "/" : ""));
        }
        return found;
    }

    private static void addFile(ZipOutputStream zip, File root, File file,
                                String archiveName) throws IOException {
        final String name = archiveName.replace('\\', '/');
        final String rootPath = root.getCanonicalPath();
        final String filePath = file.getCanonicalPath();
        if (!filePath.equals(rootPath) &&
                !filePath.startsWith(rootPath + File.separator)) {
            throw new IOException("Refusing file outside app storage");
        }
        zip.putNextEntry(new ZipEntry(name));
        try (InputStream input = new BufferedInputStream(new FileInputStream(file))) {
            final byte[] buffer = new byte[BUFFER_SIZE];
            int count;
            while ((count = input.read(buffer)) != -1) zip.write(buffer, 0, count);
        }
        zip.closeEntry();
    }

    private static void addDashboardPreferences(ZipOutputStream zip,
                                                Context context) throws IOException {
        final Properties properties = new Properties();
        final SharedPreferences preferences = context.getSharedPreferences(
                "dashboard", Context.MODE_PRIVATE);
        for (Map.Entry<String, ?> entry : preferences.getAll().entrySet()) {
            final Object value = entry.getValue();
            if (value instanceof Boolean || value instanceof String ||
                    value instanceof Integer || value instanceof Long ||
                    value instanceof Float) {
                properties.setProperty(entry.getKey(), String.valueOf(value));
            }
        }
        final ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        properties.store(bytes, "Go Bigger Boy dashboard preferences");
        zip.putNextEntry(new ZipEntry(DASHBOARD_PREFERENCES));
        zip.write(bytes.toByteArray());
        zip.closeEntry();
    }

    private static void restoreDashboardPreferences(Context context,
                                                     InputStream input)
            throws IOException {
        final ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        copyLimited(input, bytes, MAXIMUM_SAVE_BYTES);
        final Properties properties = new Properties();
        properties.load(new ByteArrayInputStream(bytes.toByteArray()));
        final SharedPreferences.Editor editor = context.getSharedPreferences(
                "dashboard", Context.MODE_PRIVATE).edit();
        for (String key : properties.stringPropertyNames()) {
            final String value = properties.getProperty(key);
            if ("true".equalsIgnoreCase(value) || "false".equalsIgnoreCase(value)) {
                editor.putBoolean(key, Boolean.parseBoolean(value));
            } else {
                editor.putString(key, value);
            }
        }
        if (!editor.commit()) throw new IOException("Could not restore preferences");
    }

    private static File safeArchiveTarget(File root, String rawName) throws IOException {
        if (rawName == null || rawName.isEmpty() || rawName.indexOf('\0') >= 0) {
            throw new IOException("Invalid backup path");
        }
        final String name = rawName.replace('\\', '/');
        if (name.startsWith("/") || name.startsWith("../") || name.contains("/../") ||
                name.endsWith("/..") || name.equals("..")) {
            throw new IOException("Invalid backup path");
        }
        final File target = new File(root, name);
        final String rootPath = root.getCanonicalPath();
        final String targetPath = target.getCanonicalPath();
        if (!targetPath.equals(rootPath) &&
                !targetPath.startsWith(rootPath + File.separator)) {
            throw new IOException("Invalid backup path");
        }
        return target;
    }

    private static String sanitizeFileName(String rawName) {
        if (rawName == null) return "";
        final int slash = Math.max(rawName.lastIndexOf('/'), rawName.lastIndexOf('\\'));
        String name = slash >= 0 ? rawName.substring(slash + 1) : rawName;
        name = name.replaceAll("[^A-Za-z0-9._-]", "_");
        if (name.length() > 160) name = name.substring(0, 160);
        return name;
    }

    static String displayName(Context context, Uri uri) {
        try (android.database.Cursor cursor = context.getContentResolver().query(
                uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                final int column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (column >= 0) return cursor.getString(column);
            }
        } catch (Exception ignored) {
        }
        return "";
    }
}
