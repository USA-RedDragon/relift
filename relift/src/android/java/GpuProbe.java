package dev.relift.app;

import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Process;
import android.os.ResultReceiver;
import android.util.Log;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The test load of a GPU driver, in a process of its own (":gpucheck"), so a
 * driver that crashes cannot take the game with it. It loads the program's
 * library, loads the driver, makes a Vulkan instance, reads the GPU, answers
 * and ends its process.
 */
public class GpuProbe extends Service {
    static native String nativeProbe(String dir, String library);

    /** What a probe reports to the caller. */
    interface Done {
        void done(String result);
    }

    private static final long TIMEOUT_MS = 30000;

    @Override
    public int onStartCommand(Intent intent, int flags, int id) {
        final String dir = intent == null ? null : intent.getStringExtra("dir");
        final String lib = intent == null ? null : intent.getStringExtra("library");
        final ResultReceiver back = intent == null ? null : intent.getParcelableExtra("back");
        new Thread(() -> {
            String r;
            try {
                String name = Ui.metaString(this, "relift.library");
                System.loadLibrary(name.isEmpty() ? "main" : name);
                r = nativeProbe(dir, lib);
            } catch (Throwable t) {
                r = "error\n" + t;
            }
            Log.i(Drivers.TAG, "GPU driver check " + (dir == null ? "system" : dir) + ": " + r.replace('\n', '|'));
            if (back != null) {
                Bundle b = new Bundle();
                b.putString("result", r);
                back.send(0, b);
            }
            Process.killProcess(Process.myPid());
        }).start();
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    /**
     * Test-loads the driver in folder dir (its library file name lib; dir
     * null: the phone's own driver) and calls done on the main thread with
     * "ok" or "error" and then the probe's lines. A probe that crashes or
     * hangs reports an error after a timeout.
     */
    static void run(Context ctx, String dir, String lib, Done done) {
        final Handler main = new Handler(Looper.getMainLooper());
        final AtomicBoolean once = new AtomicBoolean();
        ResultReceiver back = new ResultReceiver(main) {
            @Override
            protected void onReceiveResult(int code, Bundle data) {
                if (!once.getAndSet(true)) {
                    done.done(data == null ? "error\nno answer" : data.getString("result", "error\nno answer"));
                }
            }
        };
        main.postDelayed(() -> {
            if (!once.getAndSet(true)) {
                Log.w(Drivers.TAG, "GPU driver check of " + dir + " ended without an answer");
                done.done("error\nthe check ended without an answer");
            }
        }, TIMEOUT_MS);
        Intent i = new Intent(ctx, GpuProbe.class);
        i.putExtra("dir", dir);
        i.putExtra("library", lib);
        i.putExtra("back", plain(back));
        ctx.startService(i);
    }

    private static ResultReceiver plain(ResultReceiver r) {
        android.os.Parcel p = android.os.Parcel.obtain();
        r.writeToParcel(p, 0);
        p.setDataPosition(0);
        ResultReceiver out = ResultReceiver.CREATOR.createFromParcel(p);
        p.recycle();
        return out;
    }
}
