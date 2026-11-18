package dev.relift.app;

import android.app.Presentation;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * The displays besides the one the game's activity is on: a Presentation with
 * a SurfaceView on each, whose surface and touches go to the runtime
 * (host/android_display.c), for guests that show a screen on each display.
 * The runtime draws into each surface with a Vulkan swapchain of its own. A
 * presentation the system dismisses (its display was unplugged) is
 * forgotten, so the display can be opened again when it comes back.
 */
final class Displays {
    static final int MAX = 2;

    private final MainActivity act;
    private static final long RESHOW_MS = 300;

    private final Pres[] open = new Pres[MAX];
    private final boolean[] wanted = new boolean[MAX];
    private final Handler later = new Handler(Looper.getMainLooper());

    Displays(MainActivity act) {
        this.act = act;
    }

    /** The displays that can show a presentation, other than the game's own, in id order. */
    List<Display> others() {
        DisplayManager dm = act.getSystemService(DisplayManager.class);
        Display own = act.getDisplay();
        int ownId = own != null ? own.getDisplayId() : Display.DEFAULT_DISPLAY;
        Display[] all = dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        Arrays.sort(all, (a, b) -> Integer.compare(a.getDisplayId(), b.getDisplayId()));
        List<Display> out = new ArrayList<>();
        for (Display d : all) {
            if (d.getDisplayId() != ownId && d.isValid() && out.size() < MAX) {
                out.add(d);
            }
        }
        return out;
    }

    int count() {
        return others().size();
    }

    /** Shows the presentation of the index-th other display (on the UI thread); its surface arrives through the native callback. */
    void open(int index) {
        if (index < 0 || index >= MAX) {
            return;
        }
        wanted[index] = true;
        act.runOnUiThread(this::sync);
    }

    void close(int index) {
        if (index < 0 || index >= MAX) {
            return;
        }
        wanted[index] = false;
        act.runOnUiThread(this::sync);
    }

    /**
     * Brings the presentations to what the runtime asked for (UI thread): one
     * on each wanted other display, none on the game's own display, and a new
     * one where the system dismissed it or the numbering of the other displays
     * moved (the game's task changed display).
     */
    void sync() {
        List<Display> d = others();
        for (int i = 0; i < MAX; i++) {
            Pres p = open[i];
            if (p != null && (!wanted[i] || i >= d.size() || p.displayId() != d.get(i).getDisplayId())) {
                open[i] = null;
                p.dismiss();
            }
            if (!wanted[i] || i >= d.size() || open[i] != null) {
                continue;
            }
            final int index = i;
            Pres q = new Pres(act, d.get(i), i);
            open[i] = q;
            q.setOnDismissListener(x -> {
                if (open[index] == q) {
                    open[index] = null;
                    later.postDelayed(this::sync, RESHOW_MS);
                }
            });
            try {
                q.show();
            } catch (WindowManager.InvalidDisplayException e) {
                open[i] = null;
            }
        }
    }

    private static final class Pres extends Presentation implements SurfaceHolder.Callback, View.OnTouchListener {
        private final int index;

        Pres(MainActivity act, Display display, int index) {
            super(act, display);
            this.index = index;
        }

        @Override
        protected void onCreate(Bundle state) {
            super.onCreate(state);
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
                    | WindowManager.LayoutParams.FLAG_FULLSCREEN);
            SurfaceView sv = new SurfaceView(getContext());
            sv.getHolder().addCallback(this);
            sv.setOnTouchListener(this);
            setContentView(sv);
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                c.hide(WindowInsets.Type.systemBars());
            }
        }

        int displayId() {
            return getDisplay().getDisplayId();
        }

        @Override
        public void surfaceCreated(SurfaceHolder holder) {
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
            MainActivity.nativeDisplaySurface(index, holder.getSurface(), w, h);
        }

        /** Returns only once the runtime's presenter has let go of the surface. */
        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
            MainActivity.nativeDisplaySurface(index, null, 0, 0);
        }

        @Override
        public boolean onTouch(View v, MotionEvent e) {
            int a = e.getActionMasked();
            if (a == MotionEvent.ACTION_DOWN || a == MotionEvent.ACTION_POINTER_DOWN) {
                int i = e.getActionIndex();
                MainActivity.nativeDisplayTouch(index, e.getPointerId(i), 0, e.getX(i), e.getY(i));
            } else if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_POINTER_UP) {
                int i = e.getActionIndex();
                MainActivity.nativeDisplayTouch(index, e.getPointerId(i), 1, e.getX(i), e.getY(i));
            } else if (a == MotionEvent.ACTION_MOVE) {
                for (int i = 0; i < e.getPointerCount(); i++) {
                    MainActivity.nativeDisplayTouch(index, e.getPointerId(i), 2, e.getX(i), e.getY(i));
                }
            } else if (a == MotionEvent.ACTION_CANCEL) {
                for (int i = 0; i < e.getPointerCount(); i++) {
                    MainActivity.nativeDisplayTouch(index, e.getPointerId(i), 3, e.getX(i), e.getY(i));
                }
            }
            return true;
        }
    }
}
