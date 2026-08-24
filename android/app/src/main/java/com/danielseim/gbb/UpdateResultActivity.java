package com.danielseim.gbb;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.widget.Toast;

/** Lightweight PackageInstaller callback that survives replacement of SDL. */
public final class UpdateResultActivity extends Activity {
    private static final String TAG = "GBB updater";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        handle(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handle(intent);
    }

    private void handle(Intent intent) {
        if (intent == null ||
                !GbbActivity.ACTION_INSTALL_RESULT.equals(intent.getAction())) {
            finish();
            return;
        }
        final int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS,
                PackageInstaller.STATUS_FAILURE);
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            final Intent confirmation;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                confirmation = intent.getParcelableExtra(Intent.EXTRA_INTENT,
                                                         Intent.class);
            } else {
                //noinspection deprecation
                confirmation = intent.getParcelableExtra(Intent.EXTRA_INTENT);
            }
            if (confirmation != null) startActivity(confirmation);
            return;
        }
        if (status == PackageInstaller.STATUS_SUCCESS) {
            final ComponentName launcher = new ComponentName(
                    this, LibraryActivity.class);
            startActivity(Intent.makeRestartActivityTask(launcher));
            finish();
            return;
        }
        final String message = intent.getStringExtra(
                PackageInstaller.EXTRA_STATUS_MESSAGE);
        Log.e(TAG, "Package installation failed: " + message);
        Toast.makeText(this, "Go Bigger Boy update failed.",
                Toast.LENGTH_LONG).show();
        startActivity(Intent.makeRestartActivityTask(new ComponentName(
                this, LibraryActivity.class)));
        finish();
    }
}
