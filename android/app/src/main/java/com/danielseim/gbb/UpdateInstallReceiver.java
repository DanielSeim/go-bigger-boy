package com.danielseim.gbb;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.os.Build;
import android.util.Log;
import android.widget.Toast;

/** Completes PackageInstaller's user-confirmation flow for an in-app update. */
public final class UpdateInstallReceiver extends BroadcastReceiver {
    public static final String ACTION_INSTALL_RESULT =
            "com.danielseim.gbb.INSTALL_UPDATE_RESULT";
    private static final String TAG = "GBB updater";

    @Override
    public void onReceive(Context context, Intent intent) {
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
            if (confirmation != null) {
                confirmation.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                context.startActivity(confirmation);
            }
            return;
        }
        if (status != PackageInstaller.STATUS_SUCCESS) {
            final String message = intent.getStringExtra(
                    PackageInstaller.EXTRA_STATUS_MESSAGE);
            Log.e(TAG, "Package installation failed: " + message);
            Toast.makeText(context, "Go Bigger Boy update failed.",
                    Toast.LENGTH_LONG).show();
        }
    }
}
