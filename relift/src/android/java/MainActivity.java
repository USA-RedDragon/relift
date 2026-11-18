package dev.relift.app;

import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.os.Process;
import android.util.Log;
import android.view.Surface;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.ViewGroup;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;
import java.io.File;
import java.io.FileInputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import org.libsdl.app.SDLActivity;

/**
 * SDL's activity plus launch arguments and environment variables from the
 * intent, the haptic tick of the on-screen controller, the game's settings
 * pages over the pause sheet, the GPU driver the game starts with, and the
 * game data import when the start-up check finds the data missing.
 */
public class MainActivity extends SDLActivity implements Runnable, OnBackInvokedCallback, Ui.Host {
    static final int NEXT_PAUSE = 0;
    static final int NEXT_CONTROLLER = 1;
    static final int NEXT_RESUME = 2;

    static native void nativeSettingsDone(int next);

    static native String nativeGpuDriverLoaded();

    static native void nativeDisplaySurface(int index, Surface surface, int width, int height);

    static native void nativeDisplayTouch(int index, int id, int action, float x, float y);

    private Drivers drivers;
    private Ui ui;
    private Displays displays;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        drivers = new Drivers(this);
        displays = new Displays(this);
        ui = new Ui(this, drivers, this, false);
        addContentView(ui.root, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        if (Build.VERSION.SDK_INT >= 33) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, this);
        }
        String page = getIntent().getStringExtra("relift_open");
        if ("gpu_driver".equals(page)) {
            ui.open(Ui.Page.DRIVER);
        }
    }

    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
    }

    @Override
    public void onBackInvoked() {
        if (ui != null && ui.back()) {
            return;
        }
        onNativeKeyDown(KeyEvent.KEYCODE_BACK);
        onNativeKeyUp(KeyEvent.KEYCODE_BACK);
    }

    @Override
    protected String[] getArguments() {
        String e = getIntent().getStringExtra("relift_env");
        if (e != null) {
            for (String kv : e.trim().split("\\s+")) {
                int i = kv.indexOf('=');
                if (i > 0) {
                    nativeSetenv(kv.substring(0, i), kv.substring(i + 1));
                }
            }
        }
        getExternalFilesDir(null);
        Drivers d = drivers != null ? drivers : new Drivers(this);
        installGpuDriver(d, getIntent().getStringExtra("relift_gpu_driver_install"));
        ArrayList<String> args = new ArrayList<>();
        String driver = getIntent().getStringExtra("relift_gpu_driver");
        if (driver == null || driver.trim().isEmpty()) {
            driver = d.choice();
        }
        args.add("--relift-set");
        args.add("video.gpu_driver=" + driver.trim());
        String a = getIntent().getStringExtra("relift_args");
        if (a != null && !a.trim().isEmpty()) {
            args.addAll(Arrays.asList(a.trim().split("\\s+")));
        }
        return args.toArray(new String[0]);
    }

    /** The test hook relift_gpu_driver_install: adds a package file without asking, and without a test load. */
    private static void installGpuDriver(Drivers d, String zip) {
        if (zip == null || zip.isEmpty()) {
            return;
        }
        File ext = d.ctx.getExternalFilesDir(null);
        File src = new File(zip);
        if (!src.isAbsolute() && ext != null) {
            src = new File(ext, zip);
        }
        try (FileInputStream in = new FileInputStream(src)) {
            Drivers.Pkg p = d.commit(d.stage(in));
            Log.i(Drivers.TAG, "GPU driver package " + (p == null ? zip : p.dir) + " installed");
        } catch (Exception e) {
            Log.e(Drivers.TAG, "GPU driver package " + zip + " not installed: " + e);
        }
    }

    @Override
    protected void onActivityResult(int request, int code, Intent data) {
        super.onActivityResult(request, code, data);
        if (request == Ui.REQ_PICK && ui != null) {
            ui.picked(code, data);
        }
    }

    /** Called by the runtime (any thread) when the player taps Settings on the pause sheet. */
    public void reliftOpenSettings() {
        runOnUiThread(() -> ui.open(Ui.Page.SETTINGS));
    }

    /**
     * Called by the runtime on the thread that loads the Vulkan driver when the
     * chosen package did not load: the choice goes back to the phone's own
     * driver, and the game waits until the player has read the message.
     */
    public void reliftGpuDriverFailed(String name) {
        Drivers.Pkg p = drivers.find(name);
        final String shown = p != null ? p.name : name;
        drivers.setChoice(Drivers.SYSTEM);
        final CountDownLatch go = new CountDownLatch(1);
        runOnUiThread(() -> ui.loadFailed(shown, go::countDown));
        try {
            go.await();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /**
     * Called by the runtime on its main thread with the start-up check of the
     * game data: "ok", or "bad", the count of bad files and the first one,
     * tab-separated. On "bad" the game closes and the import opens; the
     * runtime does not start the game.
     */
    public void reliftDataChecked(String report) {
        String[] f = report.split("\t", -1);
        GameData.Status s = new GameData.Status();
        s.source = GameData.Status.START;
        s.ok = "ok".equals(f[0]);
        if (!s.ok) {
            s.bad = f.length > 1 ? Integer.parseInt(f[1]) : 1;
            s.first = f.length > 2 ? f[2].replaceFirst("^romfs/", "") : "";
        }
        GameData.Status old = DataPages.status(this);
        if (s.ok || old == null || !DataPages.deleted(old)) {
            s.write(DataPages.statusFile(this));
        }
        if (!s.ok) {
            Log.i(Drivers.TAG, "game data: " + report.replace('\t', ' ') + "; opening the import");
            leaveFor(null);
        }
    }

    /** Opens the game data import with action and ends the game's process. */
    private void leaveFor(String action) {
        DataActivity.open(this, action);
        finishAndRemoveTask();
        Process.killProcess(Process.myPid());
    }

    @Override
    public void openData(String action) {
        leaveFor(action);
    }

    @Override
    public void closed(int next) {
        nativeSettingsDone(next);
    }

    @Override
    public void restart() {
        Restarter.restart(this);
    }

    @Override
    public String loaded() {
        try {
            return nativeGpuDriverLoaded();
        } catch (UnsatisfiedLinkError e) {
            return null;
        }
    }

    /** Called by the runtime (any thread): how many displays besides this one can show a presentation. */
    public int reliftDisplayCount(int unused) {
        return displays == null ? 0 : displays.count();
    }

    /** Called by the runtime (any thread): shows the presentation of the index-th other display. */
    public int reliftDisplayOpen(int index) {
        displays.open(index);
        return 0;
    }

    /** Called by the runtime (any thread): removes the presentation of the index-th other display. */
    public int reliftDisplayClose(int index) {
        displays.close(index);
        return 0;
    }

    public void reliftHaptic() {
        runOnUiThread(this);
    }

    @Override
    public void run() {
        getWindow().getDecorView().performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
    }
}
