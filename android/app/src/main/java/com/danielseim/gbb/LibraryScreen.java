package com.danielseim.gbb;

import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.text.DateFormat;
import java.util.Date;

/** Builds the recently-played ROM screen independently from activity chrome. */
final class LibraryScreen {
    private static final char FIELD_SEPARATOR = 0x1f;

    private final LibraryActivity activity;
    private final LinearLayout content;
    private final SharedPreferences preferences;
    private final ArtworkService artworkService;

    LibraryScreen(LibraryActivity activity, LinearLayout content,
                  SharedPreferences preferences, ArtworkService artworkService) {
        this.activity = activity;
        this.content = content;
        this.preferences = preferences;
        this.artworkService = artworkService;
    }

    void populate() {
        final TextView heading = activity.text(
                "Recently played", 22, Color.rgb(24, 29, 39));
        heading.setTypeface(null, android.graphics.Typeface.BOLD);
        content.addView(heading);
        final TextView help = activity.text(
                "Tap a game to play, or add a ROM from your device.",
                15, Color.DKGRAY);
        help.setPadding(0, activity.dp(4), 0, activity.dp(14));
        content.addView(help);

        final Button open = new Button(activity);
        open.setText("Open ROM");
        open.setOnClickListener(view -> activity.openRomPicker());
        final LinearLayout.LayoutParams openParams =
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT);
        openParams.bottomMargin = activity.dp(16);
        content.addView(open, openParams);

        final String[] entries = LibraryActivity.nativeLibraryEntries(
                activity.getFilesDir().getAbsolutePath());
        if (entries == null || entries.length == 0) {
            final TextView empty = activity.text(
                    "No games yet\n\nOpen a Game Boy or Game Boy Color ROM to add it here.",
                    17, Color.GRAY);
            empty.setGravity(android.view.Gravity.CENTER);
            empty.setPadding(activity.dp(16), activity.dp(52),
                    activity.dp(16), activity.dp(52));
            content.addView(empty);
            return;
        }
        for (String encoded : entries) addGameCard(encoded);
    }

    private void addGameCard(String encoded) {
        final String[] fields = encoded.split(String.valueOf(FIELD_SEPARATOR), -1);
        if (fields.length != 9) return;

        final LinearLayout card = new LinearLayout(activity);
        card.setOrientation(LinearLayout.HORIZONTAL);
        card.setGravity(android.view.Gravity.CENTER_VERTICAL);
        card.setPadding(activity.dp(12), activity.dp(12),
                activity.dp(12), activity.dp(12));
        card.setBackgroundColor(Color.WHITE);
        card.setElevation(activity.dp(2));
        card.setClickable(true);
        card.setFocusable(true);
        card.setContentDescription("Play " + fields[3]);
        card.setOnClickListener(view -> activity.launchRom(fields[2], ""));
        final LinearLayout.LayoutParams cardParams =
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT);
        cardParams.bottomMargin = activity.dp(12);

        final ImageView cover = new ImageView(activity);
        cover.setScaleType(ImageView.ScaleType.CENTER_CROP);
        cover.setBackgroundColor(Color.rgb(225, 228, 235));
        cover.setContentDescription(fields[3] + " cover artwork");
        card.addView(cover, new LinearLayout.LayoutParams(
                activity.dp(82), activity.dp(108)));

        final LinearLayout details = new LinearLayout(activity);
        details.setOrientation(LinearLayout.VERTICAL);
        details.setPadding(activity.dp(16), 0, 0, 0);
        final TextView title = activity.text(fields[3], 19,
                Color.rgb(24, 29, 39));
        title.setTypeface(null, android.graphics.Typeface.BOLD);
        details.addView(title);
        final TextView platform = activity.text(fields[4], 15,
                Color.rgb(69, 91, 171));
        platform.setPadding(0, activity.dp(7), 0, activity.dp(3));
        details.addView(platform);
        final TextView language = activity.text("Language: " + fields[5],
                14, Color.DKGRAY);
        details.addView(language);
        details.addView(activity.text("Last played: " +
                formattedLastPlayed(fields[8]), 13, Color.GRAY));

        final Button more = new Button(activity);
        more.setText("⋮");
        more.setTextSize(24);
        more.setAllCaps(false);
        more.setMinWidth(activity.dp(48));
        more.setMinHeight(activity.dp(48));
        more.setContentDescription("More options for " + fields[3]);
        more.setOnClickListener(view -> new AlertDialog.Builder(activity)
                .setTitle("Remove recent game?")
                .setMessage("The ROM file and saved game will not be deleted.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Remove", (dialog, which) -> {
                    if (LibraryActivity.nativeRemoveLibraryEntry(
                            activity.getFilesDir().getAbsolutePath(), fields[0])) {
                        activity.showDashboard(false);
                    } else {
                        Toast.makeText(activity,
                                "Could not remove recent game",
                                Toast.LENGTH_SHORT).show();
                    }
                }).show());
        card.addView(details, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        card.addView(more, new LinearLayout.LayoutParams(
                activity.dp(48), activity.dp(48)));
        content.addView(card, cardParams);

        final boolean downloadArtwork =
                preferences.getBoolean("cover_artwork", true);
        artworkService.resolve(fields, downloadArtwork,
                new ArtworkService.Callback() {
                    @Override
                    public void onMetadata(String resolvedTitle,
                                           String resolvedLanguage) {
                        title.setText(resolvedTitle);
                        language.setText(resolvedLanguage);
                    }

                    @Override
                    public void onArtwork(Bitmap bitmap) {
                        cover.setImageBitmap(bitmap);
                    }
                });
    }

    private static String formattedLastPlayed(String timestamp) {
        try {
            return DateFormat.getDateTimeInstance(DateFormat.MEDIUM,
                    DateFormat.SHORT).format(
                    new Date(Long.parseLong(timestamp) * 1000L));
        } catch (Exception ignored) {
            return "Unknown";
        }
    }
}
