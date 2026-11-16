package dev.relift.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;

/**
 * The game data copy as a foreground service in the ":data" process, so it
 * keeps going when the player leaves the app; its notification shows the
 * progress and opens the import's page.
 */
public class DataService extends Service {
    static final String CHANNEL = "game_data";
    static final int ID = 0x5244;
    private static long lastShown;

    private PowerManager.WakeLock wake;

    static void start(Context c) {
        c.startForegroundService(new Intent(c, DataService.class));
    }

    static void channel(Context c) {
        NotificationManager nm = c.getSystemService(NotificationManager.class);
        if (nm.getNotificationChannel(CHANNEL) == null) {
            NotificationChannel ch = new NotificationChannel(CHANNEL, "Game data", NotificationManager.IMPORTANCE_LOW);
            ch.setDescription("Copying the game data into the game");
            nm.createNotificationChannel(ch);
        }
    }

    static Notification.Builder builder(Context c) {
        channel(c);
        Intent open = new Intent(c, DataActivity.class);
        open.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        return new Notification.Builder(c, CHANNEL)
                .setContentIntent(PendingIntent.getActivity(c, 0, open,
                        PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT))
                .setShowWhen(false);
    }

    static Notification copying(Context c, DataImport j) {
        int max = 1000;
        int at = j.bytesTotal == 0 ? 0 : (int) (j.bytesDone * max / j.bytesTotal);
        return builder(c).setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentTitle("Copying game data")
                .setContentText(GameData.count(j.filesDone) + " of " + GameData.count(j.filesTotal) + " files \u00b7 "
                        + GameData.size(j.bytesDone) + " of " + GameData.size(j.bytesTotal))
                .setProgress(max, at, false)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .build();
    }

    /** Updates the notification, at most once a second. */
    static void progress(Context c, DataImport j) {
        long t = System.nanoTime();
        if (t - lastShown < 1_000_000_000L) {
            return;
        }
        lastShown = t;
        c.getSystemService(NotificationManager.class).notify(ID, copying(c, j));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        final DataImport j = DataImport.get(this);
        Notification n = copying(this, j);
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        } else {
            startForeground(ID, n);
        }
        if (wake == null) {
            wake = getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "relift:gamedata");
            wake.setReferenceCounted(false);
            wake.acquire(12 * 3600 * 1000L);
        }
        new Thread(() -> {
            j.runCopy();
            j.main.post(() -> ended(j));
        }, "game data copy").start();
        return START_NOT_STICKY;
    }

    private void ended(DataImport j) {
        if (wake != null) {
            wake.release();
            wake = null;
        }
        stopForeground(STOP_FOREGROUND_REMOVE);
        String title;
        switch (j.outcome) {
            case DONE: title = "Game data is ready"; break;
            case PARTIAL_DONE: title = "Some game data is missing"; break;
            case STOPPED: title = "The copy was interrupted"; break;
            default: title = "The copy stopped"; break;
        }
        if (j.listener == null) {
            getSystemService(NotificationManager.class).notify(ID, builder(this)
                    .setSmallIcon(android.R.drawable.stat_sys_download_done)
                    .setContentTitle(title)
                    .setContentText(GameData.count(j.filesDone) + " of " + GameData.count(j.filesTotal) + " files")
                    .setAutoCancel(true)
                    .build());
        }
        stopSelf();
    }

    @Override
    public void onTimeout(int startId, int type) {
        DataImport.get(this).stop();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
