package dev.relift.app;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import java.io.File;

/**
 * Restart now: runs in a process of its own (":restart"), waits until the
 * game's process has ended, starts the game again with its launch intent and
 * ends.
 */
public class Restarter extends Activity {
    private static final long WAIT_MS = 5000;

    /** Starts the game again from a new process; the caller then ends its own process. */
    static void restart(Activity game) {
        Intent i = new Intent(game, Restarter.class);
        i.putExtra("pid", Process.myPid());
        i.putExtra("launch", new Intent(game.getIntent()));
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        game.startActivity(i);
        game.finishAndRemoveTask();
        Process.killProcess(Process.myPid());
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        final int pid = getIntent().getIntExtra("pid", -1);
        final Intent launch = getIntent().getParcelableExtra("launch");
        final long until = System.currentTimeMillis() + WAIT_MS;
        final Handler h = new Handler(Looper.getMainLooper());
        h.post(new Runnable() {
            @Override
            public void run() {
                if (pid > 0 && new File("/proc/" + pid).exists() && System.currentTimeMillis() < until) {
                    h.postDelayed(this, 50);
                    return;
                }
                if (launch != null) {
                    launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
                    startActivity(launch);
                }
                finish();
                overridePendingTransition(0, 0);
                h.postDelayed(() -> Process.killProcess(Process.myPid()), 500);
            }
        });
    }
}
