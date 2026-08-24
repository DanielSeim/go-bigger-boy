package com.danielseim.gbb;

import android.content.Context;

import java.io.BufferedInputStream;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/** CRC-based lookup against Libretro's No-Intro metadata database. */
final class LibretroMetadata {
    static final class Record {
        final String name;
        final String language;

        Record(String name, String language) {
            this.name = name;
            this.language = language;
        }
    }

    private static final long MAXIMUM_DATABASE_SIZE = 3L * 1024L * 1024L;
    private static final long CACHE_LIFETIME = 7L * 24L * 60L * 60L * 1000L;
    private static final Map<String, Map<Long, Record>> databases = new HashMap<>();

    private LibretroMetadata() {}

    static synchronized Record find(Context context, String system, long crc32) {
        if (crc32 == 0) return null;
        Map<Long, Record> database = databases.get(system);
        if (database == null) {
            try {
                final File file = cachedDatabase(context, system);
                database = parse(file);
                databases.put(system, database);
            } catch (Exception ignored) {
                return null;
            }
        }
        return database.get(crc32 & 0xffffffffL);
    }

    private static File cachedDatabase(Context context, String system)
            throws Exception {
        final File directory = new File(context.getCacheDir(), "metadata");
        if (!directory.exists() && !directory.mkdirs()) {
            throw new IllegalStateException("Could not create metadata cache");
        }
        final File cached = new File(directory,
                system.replaceAll("[^A-Za-z0-9]+", "-") + ".dat");
        if (cached.isFile() && cached.length() > 0 &&
                System.currentTimeMillis() - cached.lastModified() < CACHE_LIFETIME) {
            return cached;
        }

        final String encoded = URLEncoder.encode(system + ".dat", "UTF-8")
                .replace("+", "%20");
        final URL url = new URL(
                "https://raw.githubusercontent.com/libretro/libretro-database/" +
                "master/metadat/no-intro/" + encoded);
        final HttpURLConnection connection = (HttpURLConnection) url.openConnection();
        connection.setConnectTimeout(6000);
        connection.setReadTimeout(12000);
        connection.setRequestProperty("User-Agent", "Go-Bigger-Boy/Android");
        final File temporary = new File(directory, cached.getName() + ".download");
        try {
            if (connection.getResponseCode() != 200 ||
                    connection.getContentLength() > MAXIMUM_DATABASE_SIZE) {
                throw new IllegalStateException("Metadata service unavailable");
            }
            try (InputStream input = new BufferedInputStream(connection.getInputStream());
                 FileOutputStream output = new FileOutputStream(temporary, false)) {
                final byte[] buffer = new byte[16 * 1024];
                long total = 0;
                int count;
                while ((count = input.read(buffer)) != -1) {
                    total += count;
                    if (total > MAXIMUM_DATABASE_SIZE) {
                        throw new IllegalStateException("Metadata database too large");
                    }
                    output.write(buffer, 0, count);
                }
            }
            if (cached.exists() && !cached.delete()) {
                throw new IllegalStateException("Could not replace metadata cache");
            }
            if (!temporary.renameTo(cached)) {
                throw new IllegalStateException("Could not store metadata cache");
            }
            return cached;
        } catch (Exception error) {
            if (temporary.exists()) temporary.delete();
            if (cached.isFile() && cached.length() > 0) return cached;
            throw error;
        } finally {
            connection.disconnect();
        }
    }

    private static Map<Long, Record> parse(File file) throws Exception {
        final Map<Long, Record> records = new HashMap<>();
        String name = "";
        String region = "";
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                final String trimmed = line.trim();
                if (trimmed.equals("game (")) {
                    name = "";
                    region = "";
                } else if (trimmed.startsWith("name \"") && name.isEmpty()) {
                    name = quotedValue(trimmed);
                } else if (trimmed.startsWith("region \"")) {
                    region = quotedValue(trimmed);
                } else if (trimmed.startsWith("rom (")) {
                    final int crc = trimmed.indexOf(" crc ");
                    if (crc >= 0 && crc + 14 <= trimmed.length() && !name.isEmpty()) {
                        try {
                            final long value = Long.parseLong(
                                    trimmed.substring(crc + 5, crc + 13), 16);
                            records.put(value, new Record(name,
                                    inferLanguage(name, region)));
                        } catch (NumberFormatException ignored) {
                        }
                    }
                }
            }
        }
        return records;
    }

    private static String quotedValue(String line) {
        final int first = line.indexOf('"');
        final int last = line.lastIndexOf('"');
        return first >= 0 && last > first ? line.substring(first + 1, last) : "";
    }

    private static String inferLanguage(String name, String region) {
        final String lower = name.toLowerCase(Locale.ROOT);
        final String[][] tags = {
                {"(de", "German"}, {",de", "German"},
                {"(en", "English"}, {",en", "English"},
                {"(fr", "French"}, {",fr", "French"},
                {"(es", "Spanish"}, {",es", "Spanish"},
                {"(it", "Italian"}, {",it", "Italian"},
                {"(nl", "Dutch"}, {",nl", "Dutch"},
                {"(ja", "Japanese"}, {",ja", "Japanese"}
        };
        for (String[] tag : tags) {
            if (lower.contains(tag[0])) return tag[1];
        }
        final String normalized = region.toLowerCase(Locale.ROOT);
        if (normalized.contains("germany")) return "German";
        if (normalized.contains("france")) return "French";
        if (normalized.contains("spain")) return "Spanish";
        if (normalized.contains("italy")) return "Italian";
        if (normalized.contains("netherlands")) return "Dutch";
        if (normalized.contains("japan")) return "Japanese";
        if (normalized.contains("usa") || normalized.contains("europe") ||
                normalized.contains("australia") || normalized.contains("canada")) {
            return "English";
        }
        return "International";
    }
}
