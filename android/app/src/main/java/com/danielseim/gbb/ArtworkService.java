package com.danielseim.gbb;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Handler;
import android.os.Looper;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.zip.CRC32;

/** Resolves Libretro metadata and caches cover artwork off the UI thread. */
final class ArtworkService {
    interface Callback {
        void onMetadata(String title, String language);
        void onArtwork(Bitmap bitmap);
    }

    private static final long MAX_DOWNLOAD_BYTES = 5L * 1024L * 1024L;
    private final Context context;
    private final ExecutorService executor = Executors.newFixedThreadPool(2);
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    ArtworkService(Context context) {
        this.context = context.getApplicationContext();
    }

    void shutdown() {
        executor.shutdownNow();
    }

    void resolve(String[] fields, boolean downloadArtwork, Callback callback) {
        executor.execute(() -> {
            long crc = 0;
            try {
                crc = Long.parseLong(fields[1], 16);
            } catch (NumberFormatException ignored) {
            }
            if (crc == 0) crc = crcForFile(fields[2]);
            final LibretroMetadata.Record record =
                    LibretroMetadata.find(context, fields[6], crc);
            String coverName = fields[7];
            if (record != null) {
                coverName = record.name;
                final String title = displayTitle(record.name);
                final String language = "Language: " + record.language;
                mainHandler.post(() -> callback.onMetadata(title, language));
            }
            if (downloadArtwork) {
                final Bitmap bitmap = loadCover(fields[0], fields[6], coverName);
                if (bitmap != null) mainHandler.post(() -> callback.onArtwork(bitmap));
            }
        });
    }

    private Bitmap loadCover(String fingerprint, String system, String name) {
        final File directory = new File(context.getCacheDir(), "covers");
        final File cached = new File(directory, fingerprint + ".png");
        Bitmap bitmap = BitmapFactory.decodeFile(cached.getAbsolutePath());
        if (bitmap != null) return bitmap;
        HttpURLConnection connection = null;
        try {
            if (!directory.exists() && !directory.mkdirs()) return null;
            final URL url = new URL("https://thumbnails.libretro.com/" +
                    pathSegment(system) + "/Named_Boxarts/" +
                    pathSegment(thumbnailName(name)) + ".png");
            connection = (HttpURLConnection) url.openConnection();
            connection.setConnectTimeout(5000);
            connection.setReadTimeout(8000);
            connection.setRequestProperty("User-Agent", "Go Bigger Boy");
            if (connection.getResponseCode() != 200 ||
                    connection.getContentLength() > MAX_DOWNLOAD_BYTES) return null;
            try (InputStream input = new BufferedInputStream(connection.getInputStream());
                 FileOutputStream output = new FileOutputStream(cached)) {
                final byte[] buffer = new byte[16 * 1024];
                long total = 0;
                int count;
                while ((count = input.read(buffer)) != -1) {
                    total += count;
                    if (total > MAX_DOWNLOAD_BYTES) return null;
                    output.write(buffer, 0, count);
                }
            }
            return BitmapFactory.decodeFile(cached.getAbsolutePath());
        } catch (Exception ignored) {
            if (cached.exists()) cached.delete();
            return null;
        } finally {
            if (connection != null) connection.disconnect();
        }
    }

    private long crcForFile(String path) {
        final CRC32 crc = new CRC32();
        try (InputStream input = new BufferedInputStream(new FileInputStream(path))) {
            final byte[] buffer = new byte[16 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) crc.update(buffer, 0, count);
            return crc.getValue();
        } catch (Exception ignored) {
            return 0;
        }
    }

    private static String pathSegment(String value) throws Exception {
        return URLEncoder.encode(value, "UTF-8").replace("+", "%20");
    }

    private static String displayTitle(String canonicalName) {
        final int tags = canonicalName.indexOf(" (");
        return tags < 0 ? canonicalName : canonicalName.substring(0, tags);
    }

    private static String thumbnailName(String name) {
        return name.replaceAll("[&*/:`<>?\\\\|]", "_");
    }
}
