package dev.relift.app;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The settings pages of a game: Settings, Graphics, GPU driver, Manage GPU
 * drivers, Get drivers, Game data and their messages, drawn over the paused
 * game, or, from the Game settings shortcut, on their own (launcher mode, no
 * game running); and the game data import's own pages (DataActivity). Sizes
 * are the mockups' pixels at 2.625 pixels per dp.
 */
final class Ui {
    /*
     * The palette: dark or light as the phone is set (see theme). The pages
     * read these when they draw, so a page drawn after a change has the new
     * colors.
     */
    static int BG;
    static int CARD;
    static int CARD2;
    static int SUB;
    static int ACC;
    static int AMBER;
    static int RED;
    static int GREEN;
    static int LINE;
    static int BODY;
    static int DIALOG;
    /** Titles and other main text. */
    static int TEXT;
    /** Text on an ACC fill (a primary button, a Default pill). */
    static int ON_ACC;
    /** Text on a RED fill. */
    static int ON_RED;
    /** The faint line of a primary button's second line. */
    static int ON_ACC_SUB;
    /** The outline of a message and a menu. */
    static int BORDER;
    /** The empty part of a progress bar. */
    static int TRACK;
    /** The three dots of a menu button. */
    static int DOTS;
    /** Whether the light palette is in use. */
    static boolean light;
    /** The test hook relift_theme ("light" or "dark") in place of the phone's setting; null follows the phone. */
    static String forced;

    static {
        palette(false);
    }

    /** Sets the palette for the phone's light or dark setting, or for forced. */
    static void theme(Context c) {
        boolean night = (c.getResources().getConfiguration().uiMode
                & android.content.res.Configuration.UI_MODE_NIGHT_MASK) != android.content.res.Configuration.UI_MODE_NIGHT_NO;
        palette(forced != null ? "light".equals(forced) : !night);
    }

    static void palette(boolean l) {
        light = l;
        BG = l ? 0xfff4f5f7 : 0xff14161a;
        CARD = l ? 0xffffffff : 0xff1e2127;
        CARD2 = l ? 0xffe6e8ec : 0xff2b2e35;
        SUB = l ? 0xff5b6472 : 0xff9aa1ad;
        ACC = l ? 0xff1f5fcc : 0xff8ab4ff;
        AMBER = l ? 0xffb77800 : 0xffffc233;
        RED = l ? 0xffc62828 : 0xffff8a80;
        GREEN = l ? 0xff1f8a46 : 0xff7ddc9a;
        LINE = l ? 0xffd7dae0 : 0xff2f333b;
        BODY = l ? 0xff2b313b : 0xffd5d9e0;
        DIALOG = l ? 0xffffffff : 0xff23262c;
        TEXT = l ? 0xff15181d : 0xffffffff;
        ON_ACC = l ? 0xffffffff : 0xff0b1a33;
        ON_RED = l ? 0xffffffff : 0xff33100d;
        ON_ACC_SUB = l ? 0xffd6e2f7 : 0xff27406b;
        BORDER = l ? 0xffc9cdd4 : 0xFF555555;
        TRACK = l ? 0xffd7dae0 : 0xff3a3f48;
        DOTS = l ? 0xff5b6472 : 0xffc4c8d0;
    }

    /** The text color of a pill: its own dark color on the dark palette, white on the light one. */
    static int pillText(String hex) {
        return light ? 0xffffffff : Color.parseColor(hex);
    }
    static final int REQ_PICK = 0x5244;

    /** What the page needs from the activity it is in. */
    interface Host {
        /** The pages closed; next is where the game goes (MainActivity.NEXT_*). */
        void closed(int next);

        /** Restart now. */
        void restart();

        /** The driver the running game loaded: "system", a package folder, or null (none yet, or no game running). */
        String loaded();

        /**
         * Opens the game data import with an action of DataActivity
         * (ACTION_IMPORT or ACTION_DELETE); a running game closes first.
         */
        void openData(String action);
    }

    enum Page {
        HOME, SETTINGS, GRAPHICS, DRIVER, MANAGE, GET, DATA, DATA_CHECK,
        NEEDED, CHOOSE, LOOKING, READY, SPACE, COPYING, STOPPED, DONE
    }

    enum Style { PRIMARY, SECONDARY, DANGER, GHOST }

    /** A button of a message. */
    static final class Btn {
        final String label;
        final Style style;
        final Runnable run;

        Btn(String label, Style style, Runnable run) {
            this.label = label;
            this.style = style;
            this.run = run;
        }
    }

    final Activity act;
    final FrameLayout root;
    final Drivers drivers;
    final Host host;
    final boolean launcher;
    final boolean vulkan;
    final String game;
    final float k;
    final Handler main = new Handler(Looper.getMainLooper());
    final ArrayList<Page> stack = new ArrayList<>();
    final ArrayList<int[]> scrolls = new ArrayList<>();
    View popup;
    View dialog;
    boolean pinned;
    Runnable onClose;
    Catalog.Entry downloading;
    int percent;
    Catalog.Cancel cancel;
    boolean probing;
    final DataPages data;

    Ui(Activity act, Drivers drivers, Host host, boolean launcher) {
        this.act = act;
        this.drivers = drivers;
        this.host = host;
        this.launcher = launcher;
        this.vulkan = meta(act, "relift.gpu_driver");
        CharSequence l = act.getApplicationInfo().loadLabel(act.getPackageManager());
        this.game = l == null ? "this game" : l.toString();
        this.k = act.getResources().getDisplayMetrics().density / 2.625f;
        String t = act.getIntent() == null ? null : act.getIntent().getStringExtra("relift_theme");
        if (t != null && !t.isEmpty()) {
            forced = t;
        }
        theme(act);
        this.data = new DataPages(this);
        root = new FrameLayout(act);
        root.setClickable(true);
        root.setFocusable(true);
        root.setVisibility(View.GONE);
        root.setOnApplyWindowInsetsListener((v, in) -> {
            android.graphics.Insets b = in.getInsets(android.view.WindowInsets.Type.systemBars()
                    | android.view.WindowInsets.Type.displayCutout());
            v.setPadding(b.left, b.top, b.right, b.bottom);
            return in;
        });
        root.addOnLayoutChangeListener((v, l0, t0, r0, b0, l1, t1, r1, b1) -> {
            if (r0 - l0 != r1 - l1 || b0 - t0 != b1 - t1) {
                main.post(this::render);
            }
        });
    }

    static boolean meta(Activity a, String key) {
        try {
            ApplicationInfo ai = a.getPackageManager().getApplicationInfo(a.getPackageName(), PackageManager.GET_META_DATA);
            return ai.metaData != null && ai.metaData.getBoolean(key, false);
        } catch (Exception e) {
            return false;
        }
    }

    static String metaString(Context a, String key) {
        try {
            ApplicationInfo ai = a.getPackageManager().getApplicationInfo(a.getPackageName(), PackageManager.GET_META_DATA);
            return ai.metaData == null ? "" : ai.metaData.getString(key, "");
        } catch (Exception e) {
            return "";
        }
    }

    boolean showing() {
        return root.getVisibility() == View.VISIBLE;
    }

    /** Opens the pages at first. */
    void open(Page first) {
        stack.clear();
        stack.add(first);
        scrolls.clear();
        root.setVisibility(View.VISIBLE);
        if (first.ordinal() < Page.DATA.ordinal()) {
            checkSystem();
        }
        render();
    }

    void push(Page p) {
        closePopup();
        stack.add(p);
        render();
    }

    /** The back gesture: closes a message or menu, or goes back one page. */
    boolean back() {
        if (!showing()) {
            return false;
        }
        if (dialog != null) {
            if (!pinned) {
                closeDialog();
            }
            return true;
        }
        if (popup != null) {
            closePopup();
            return true;
        }
        if (stack.size() > 1) {
            stack.remove(stack.size() - 1);
            render();
            return true;
        }
        close(MainActivity.NEXT_PAUSE);
        return true;
    }

    void close(int next) {
        stack.clear();
        closePopup();
        closeDialog();
        root.removeAllViews();
        root.setVisibility(View.GONE);
        Runnable r = onClose;
        onClose = null;
        if (r != null) {
            r.run();
        } else {
            host.closed(next);
        }
    }

    /** Runs the phone's own driver check once per Android build, in the check process. */
    void checkSystem() {
        if (drivers.system() != null || probing) {
            return;
        }
        probing = true;
        GpuProbe.run(act, null, null, r -> {
            probing = false;
            drivers.saveSystem(r);
            render();
        });
    }

    Drivers.SystemDriver sys() {
        Drivers.SystemDriver s = drivers.system();
        return s != null ? s : new Drivers.SystemDriver();
    }

    String gpu() {
        Drivers.SystemDriver s = sys();
        return s.gpu.isEmpty() ? "the phone's GPU" : s.gpu;
    }

    int px(float mock) {
        return Math.round(mock * k);
    }


    /** Updates a page in progress in place, or renders the page again. */
    void refresh() {
        if (stack.isEmpty() || !data.refresh(stack.get(stack.size() - 1))) {
            render();
        }
    }

    void render() {
        if (!showing()) {
            return;
        }
        saveScrolls();
        theme(act);
        root.removeAllViews();
        int w = root.getWidth() > 0 ? root.getWidth() : act.getResources().getDisplayMetrics().widthPixels;
        int h = root.getHeight() > 0 ? root.getHeight() : act.getResources().getDisplayMetrics().heightPixels;
        boolean land = w > h;
        if (stack.isEmpty()) {
            root.setBackgroundColor(0x73000000);
        } else {
            root.setBackgroundColor(launcher ? BG : 0xb8000000);
            Page p = stack.get(stack.size() - 1);
            FrameLayout sheet = new FrameLayout(act);
            FrameLayout.LayoutParams lp;
            if (launcher) {
                lp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
                sheet.setBackgroundColor(BG);
            } else if (land) {
                lp = new FrameLayout.LayoutParams(w - w * 560 / 2400, ViewGroup.LayoutParams.MATCH_PARENT);
                lp.gravity = Gravity.END;
                sheet.setBackgroundColor(BG);
            } else {
                int top = h * 170 / 2400;
                lp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, h - top);
                lp.topMargin = top;
                GradientDrawable g = new GradientDrawable();
                g.setColor(BG);
                float r = px(48);
                g.setCornerRadii(new float[] { r, r, r, r, 0, 0, 0, 0 });
                sheet.setBackground(g);
            }
            sheet.setClickable(true);
            LinearLayout page = new LinearLayout(act);
            page.setOrientation(LinearLayout.VERTICAL);
            if (launcher && !land) {
                page.setPadding(0, px(70), 0, 0);
            }
            page.addView(header(title(p), p == Page.DRIVER && land, data.back(p)));
            build(page, p, land);
            sheet.addView(page, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT));
            root.addView(sheet, lp);
        }
        if (popup != null) {
            root.addView(popup);
        }
        if (dialog != null) {
            root.addView(dialog);
        }
        restoreScrolls();
    }

    String title(Page p) {
        switch (p) {
            case HOME: return game + ": settings";
            case SETTINGS: return "Settings";
            case GRAPHICS: return "Graphics";
            case DRIVER: return "GPU driver";
            case MANAGE: return "Manage GPU drivers";
            case GET: return "Get drivers";
            default: return data.title(p);
        }
    }

    void build(LinearLayout page, Page p, boolean land) {
        land = land && p != Page.SETTINGS && p != Page.GRAPHICS;
        Col a = new Col();
        Col b = land ? new Col() : a;
        switch (p) {
            case HOME: home(a, b); break;
            case SETTINGS: settings(a, b); break;
            case GRAPHICS: graphics(a); break;
            case DRIVER: driver(a, b, land); break;
            case MANAGE: manage(a, b, land); break;
            case GET: get(a, b, land); break;
            default: data.build(p, a, b, land); break;
        }
        if (!land) {
            page.addView(scroll(a.box, px(40), px(40)), new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
            return;
        }
        LinearLayout two = new LinearLayout(act);
        two.setOrientation(LinearLayout.HORIZONTAL);
        two.setPadding(px(40), 0, px(40), 0);
        LinearLayout.LayoutParams la = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 800);
        LinearLayout.LayoutParams lb = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 900);
        lb.leftMargin = px(60);
        two.addView(scroll(a.box, 0, 0), la);
        two.addView(scroll(b.box, 0, 0), lb);
        page.addView(two, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
    }

    ScrollView scroll(View content, int side, int unused) {
        ScrollView s = new ScrollView(act);
        s.setVerticalScrollBarEnabled(false);
        s.setFillViewport(true);
        s.setTag("scroll");
        content.setPadding(side, px(30), side, px(40));
        s.addView(content);
        return s;
    }

    void saveScrolls() {
        scrolls.clear();
        collect(root, scrolls, true);
    }

    void restoreScrolls() {
        final ArrayList<int[]> saved = new ArrayList<>(scrolls);
        root.post(() -> {
            ArrayList<int[]> now = new ArrayList<>();
            collect(root, now, false);
            int i = 0;
            for (View v : found) {
                if (i < saved.size()) {
                    final int y = saved.get(i)[0];
                    ((ScrollView) v).scrollTo(0, y);
                }
                i++;
            }
        });
    }

    final ArrayList<View> found = new ArrayList<>();

    void collect(View v, ArrayList<int[]> out, boolean read) {
        if (v == root) {
            found.clear();
        }
        if (v instanceof ScrollView && "scroll".equals(v.getTag())) {
            found.add(v);
            out.add(new int[] { v.getScrollY() });
        }
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) {
                collect(g.getChildAt(i), out, read);
            }
        }
    }


    void home(Col a, Col b) {
        a.add(hero(), 26);
        b.label("Graphics");
        driverRow(b);
        if (data.has()) {
            b.label("Data");
            b.add(data.row(), 26);
        }
        b.note("Applies the next time you start this game.");
    }

    /** The game's card: its icon, title, platform and version. */
    View hero() {
        LinearLayout card = new LinearLayout(act);
        card.setOrientation(LinearLayout.HORIZONTAL);
        card.setGravity(Gravity.CENTER_VERTICAL);
        card.setBackground(round(CARD, 28, 0, 0));
        card.setPadding(px(30), px(30), px(30), px(30));
        ImageView icon = new ImageView(act);
        icon.setImageDrawable(act.getApplicationInfo().loadIcon(act.getPackageManager()));
        icon.setClipToOutline(true);
        icon.setBackground(round(0xff7d838e, 24, 0, 0));
        card.addView(icon, new LinearLayout.LayoutParams(px(130), px(130)));
        LinearLayout t = vbox();
        t.setPadding(px(30), 0, 0, 0);
        t.addView(text(game, 44, TEXT, true));
        String platform = metaString(act, "relift.platform");
        String version = metaString(act, "relift.game_version");
        String line = platform + (version.isEmpty() ? "" : (platform.isEmpty() ? "Version " : " \u00b7 version ") + version);
        if (!line.isEmpty()) {
            TextView s = text(line, 30, SUB, false);
            s.setPadding(0, px(8), 0, 0);
            t.addView(s);
        }
        card.addView(t, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        return card;
    }

    void settings(Col a, Col b) {
        a.add(row("Graphics", list("GPU driver"), Kind.NAV, false, null, () -> push(Page.GRAPHICS)), 26);
        b.add(row("On-screen controller", list("Opacity, size, layout"), Kind.NAV, false, null,
                () -> close(MainActivity.NEXT_CONTROLLER)), 26);
        if (data.has()) {
            b.add(data.row(), 26);
        }
    }

    void graphics(Col a) {
        driverRow(a);
    }

    void driverRow(Col c) {
        Drivers.SystemDriver s = sys();
        if (!vulkan) {
            c.add(row("GPU driver", list("Not available in this game", "This game always uses the phone's own driver."),
                    Kind.PLAIN, false, null, null), 26);
            return;
        }
        if (drivers.system() != null && !s.usable) {
            c.add(row("GPU driver", list("Not available on this phone", "The phone's GPU driver could not be started."),
                    Kind.PLAIN, false, null, null), 26);
            return;
        }
        String ch = drivers.choice();
        Drivers.Pkg p = drivers.find(ch);
        List<String> subs = p == null ? list("Phone's own driver", s.line()) : list(p.name, p.detail());
        c.add(row("GPU driver", subs, Kind.NAV, false, null, () -> push(Page.DRIVER)), 26);
    }

    void driver(Col a, Col b, boolean land) {
        Drivers.SystemDriver s = sys();
        String ch = drivers.choice();
        if (drivers.find(ch) == null) {
            ch = Drivers.SYSTEM;
        }
        String loaded = launcher ? null : host.loaded();
        if (loaded != null && !loaded.equals(ch)) {
            a.add(banner("Restart to use " + drivers.choiceName(ch),
                    "The driver is loaded when the game starts. Progress you haven't saved in the game is lost.",
                    new Btn("Restart now", Style.PRIMARY, host::restart),
                    new Btn("Later", Style.SECONDARY, () -> close(MainActivity.NEXT_RESUME))), 26);
        }
        String shown = launcher ? ch : loaded;
        if (shown != null) {
            Drivers.Pkg lp = drivers.find(shown);
            String heading = launcher ? "NOW, FOR THIS GAME" : "IN USE NOW";
            if (lp != null) {
                a.add(activeCard(heading, lp.name, list(lp.detail())), 26);
            } else if (shown.equals(Drivers.SYSTEM)) {
                a.add(activeCard(heading, s.name, list(s.version.isEmpty() ? "" : "Version " + s.version,
                        "Comes with your phone")), 26);
            } else {
                a.add(activeCard(heading, shown, list()), 26);
            }
        }
        if (!land && !launcher) {
            b.label("Driver for this game");
        }
        b.add(radio("Phone's own driver", list(s.line()), Drivers.SYSTEM, ch, loaded), 26);
        for (Drivers.Pkg p : drivers.list()) {
            b.add(radio(p.name, list(p.detail()), p.dir, ch, loaded), 26);
        }
        Col btns = land ? a : b;
        btns.add(outline("+", "Add driver from file", this::pick), 26);
        btns.add(outline("\u2193", "Get drivers", () -> push(Page.GET)), 26);
        if (!land) {
            b.add(row("Manage drivers", list("Remove drivers from this game"), Kind.NAV, false, null,
                    () -> push(Page.MANAGE)), 26);
        }
        if (launcher) {
            b.note("Applies the next time you start this game.");
        }
    }

    View radio(String title, List<String> subs, final String dir, String chosen, String loaded) {
        ArrayList<String[]> pills = new ArrayList<>();
        boolean sel = dir.equals(chosen);
        if (dir.equals(loaded)) {
            pills.add(new String[] { "In use now", "#0b2a17", "green" });
        }
        if (sel && loaded != null && !dir.equals(loaded)) {
            pills.add(new String[] { "Starts with the next start", "#33260a", "amber" });
        }
        Row r = new Row();
        r.title = title;
        r.subs = subs;
        r.kind = Kind.RADIO;
        r.selected = sel;
        r.pills = pills;
        r.click = () -> {
            drivers.setChoice(dir);
            render();
        };
        return row(r);
    }

    void manage(Col a, Col b, boolean land) {
        a.note("These are the drivers in this game. The phone's own driver can't be removed.");
        Drivers.SystemDriver s = sys();
        b.add(row("Phone's own driver", list(s.line()), Kind.PLAIN, false, null, null), 26);
        for (final Drivers.Pkg p : drivers.list()) {
            Row r = new Row();
            r.title = p.name;
            r.subs = list(p.detail(), p.addedOn());
            r.kind = Kind.PLAIN;
            r.menu = anchor -> menu(anchor, new Btn("Remove driver", Style.DANGER, () -> confirmRemove(p)));
            b.add(row(r), 26);
        }
        Col btns = land ? a : b;
        btns.add(outline("+", "Add driver from file", this::pick), 26);
        btns.add(outline("\u2193", "Get drivers", () -> push(Page.GET)), 26);
    }

    void get(Col a, Col b, boolean land) {
        a.note("These come from the list that is part of this app version. The app checks every download before it "
                + "installs it.");
        List<Catalog.Entry> all = Catalog.load(act);
        String g = drivers.system() != null && sys().usable ? sys().gpu : null;
        ArrayList<Catalog.Entry> rec = new ArrayList<>();
        ArrayList<Catalog.Entry> other = new ArrayList<>();
        for (Catalog.Entry e : all) {
            (e.recommendedFor(g) ? rec : other).add(e);
        }
        if (!rec.isEmpty()) {
            b.label("Recommended for your GPU (" + g + ")");
            for (Catalog.Entry e : rec) {
                b.add(entry(e, true), 26);
            }
            if (!other.isEmpty()) {
                b.label("Other drivers");
            }
        }
        for (Catalog.Entry e : other) {
            b.add(entry(e, false), 26);
        }
    }

    View entry(final Catalog.Entry e, boolean rec) {
        Row r = new Row();
        r.title = e.name;
        r.subs = list(e.line1(), e.worksLine(), "License: " + e.license);
        r.kind = Kind.PLAIN;
        if (rec) {
            r.pills.add(new String[] { "Recommended", "#0b2a17", "green" });
        }
        if (drivers.withSha(e.sha256) != null) {
            r.subs.add("Already added");
        } else if (downloading == e || downloading != null && downloading.sha256.equals(e.sha256)) {
            r.subs.add("Downloading\u2026 " + percent + "%");
            r.progress = percent / 100f;
            r.button = new Btn("Cancel", Style.SECONDARY, () -> {
                if (cancel != null) {
                    cancel.set = true;
                }
            });
        } else {
            r.button = new Btn("Download", Style.PRIMARY, () -> download(e));
        }
        return row(r);
    }


    void pick() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        try {
            act.startActivityForResult(i, REQ_PICK);
        } catch (Exception e) {
            Log.w(Drivers.TAG, "no file picker: " + e);
        }
    }

    /** The picker's answer, from the activity's onActivityResult. */
    void picked(int code, Intent data) {
        if (code != Activity.RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        final Uri u = data.getData();
        add(() -> act.getContentResolver().openInputStream(u), null);
    }

    interface Source {
        InputStream open() throws Exception;
    }

    void add(final Source src, final Runnable after) {
        new Thread(() -> {
            Drivers.Staged st;
            try (InputStream in = src.open()) {
                st = drivers.stage(in);
            } catch (Drivers.AddError e) {
                main.post(() -> {
                    message(e.title, e.body, new Btn("OK", Style.SECONDARY, null));
                    if (after != null) {
                        after.run();
                    }
                });
                return;
            } catch (Exception e) {
                Drivers.AddError ae = Drivers.unreadable(e);
                main.post(() -> {
                    message(ae.title, ae.body, new Btn("OK", Style.SECONDARY, null));
                    if (after != null) {
                        after.run();
                    }
                });
                return;
            }
            final Drivers.Staged s = st;
            main.post(() -> GpuProbe.run(act, s.folder.getAbsolutePath(), s.pkg.library, r -> {
                if (after != null) {
                    after.run();
                }
                if (!r.startsWith("ok\n") || drivers.isSystem(r)) {
                    drivers.discard(s);
                    Log.w(Drivers.TAG, "GPU driver " + s.pkg.name + " did not start: " + r.replace('\n', ' '));
                    message("This driver doesn't work with this phone", "It could not start on this phone's GPU ("
                            + gpu() + "). It may be made for a different GPU.", new Btn("OK", Style.SECONDARY, null));
                    return;
                }
                Drivers.Pkg old = drivers.same(s.pkg);
                if (old != null) {
                    message(s.pkg.name + " is already added", "Add it again to replace the copy in this game.",
                            new Btn("Cancel", Style.SECONDARY, () -> drivers.discard(s)),
                            new Btn("Replace", Style.PRIMARY, () -> commit(s)));
                    pinned = true;
                    return;
                }
                commit(s);
            }));
        }).start();
    }

    void commit(Drivers.Staged s) {
        final Drivers.Pkg p;
        try {
            p = drivers.commit(s);
        } catch (Drivers.AddError e) {
            message(e.title, e.body, new Btn("OK", Style.SECONDARY, null));
            return;
        }
        render();
        if (p == null) {
            return;
        }
        String d = p.detail();
        stacked("Use this driver for " + game + "?", p.name + " was added." + (d.isEmpty() ? "" : " " + d + ".")
                + " It started fine on this phone's GPU (" + gpu() + ").",
                new Btn(game.length() <= 24 ? "Use for " + game : "Use for this game", Style.PRIMARY, () -> {
                    drivers.setChoice(p.dir);
                    render();
                }), new Btn("Not now", Style.GHOST, null));
    }

    void download(final Catalog.Entry e) {
        if (downloading != null) {
            return;
        }
        downloading = e;
        percent = 0;
        cancel = new Catalog.Cancel();
        final Catalog.Cancel c = cancel;
        final File f = new File(act.getCacheDir(), "driver-download.zip");
        render();
        new Thread(() -> {
            try {
                Catalog.download(act, e, f, (done, total) -> {
                    int pc = total > 0 ? (int) Math.min(100, done * 100 / total) : 0;
                    if (pc != percent) {
                        main.post(() -> {
                            percent = pc;
                            render();
                        });
                    }
                }, c);
            } catch (Catalog.Failed x) {
                main.post(() -> {
                    downloading = null;
                    render();
                    if (c.set) {
                        return;
                    }
                    if (x.kind == Catalog.Failed.OFFLINE) {
                        message("No internet connection", "Connect to Wi-Fi or mobile data and try again.",
                                new Btn("Cancel", Style.SECONDARY, null),
                                new Btn("Try again", Style.PRIMARY, () -> download(e)));
                    } else if (x.kind == Catalog.Failed.MISMATCH) {
                        message("The file didn't match", "The downloaded file is not the one this app expects, so it "
                                + "was not installed and has been deleted. Try again later.",
                                new Btn("OK", Style.SECONDARY, null));
                    } else {
                        message("Download failed", "The download stopped before it was complete. Nothing was "
                                + "installed.", new Btn("Cancel", Style.SECONDARY, null),
                                new Btn("Try again", Style.PRIMARY, () -> download(e)));
                    }
                });
                return;
            }
            main.post(() -> add(() -> new FileInputStream(f), () -> {
                f.delete();
                downloading = null;
                render();
            }));
        }).start();
    }

    void confirmRemove(final Drivers.Pkg p) {
        message("Remove " + p.name + "?", "It is removed from this game. If the game uses it, it goes back to the "
                + "phone's own driver the next time it starts. You can add the file again later.",
                new Btn("Cancel", Style.SECONDARY, null), new Btn("Remove", Style.DANGER, () -> {
                    drivers.remove(p);
                    render();
                }));
    }

    /** The one-time message when the chosen driver did not load at the start; done runs when the player goes on. */
    void loadFailed(String name, final Runnable done) {
        stack.clear();
        root.setVisibility(View.VISIBLE);
        Drivers.SystemDriver s = sys();
        onClose = done;
        message("Using the phone's own driver", name + " could not be loaded, so the game started with the phone's "
                + "own driver instead" + (s.name.isEmpty() ? "" : ": " + s.name) + ". Your choice is changed back.",
                new Btn("Driver settings", Style.SECONDARY, () -> {
                    stack.add(Page.DRIVER);
                    render();
                }), new Btn("OK", Style.PRIMARY, () -> close(MainActivity.NEXT_RESUME)));
        pinned = true;
        render();
    }


    void message(String title, String body, Btn... buttons) {
        dialog(title, body, false, buttons);
    }

    void stacked(String title, String body, Btn... buttons) {
        dialog(title, body, true, buttons);
    }

    void dialog(String title, String body, boolean stacked, Btn... buttons) {
        closePopup();
        pinned = false;
        int w = root.getWidth() > 0 ? root.getWidth() : act.getResources().getDisplayMetrics().widthPixels;
        int h = root.getHeight() > 0 ? root.getHeight() : act.getResources().getDisplayMetrics().heightPixels;
        boolean land = w > h;
        FrameLayout dim = new FrameLayout(act);
        dim.setBackgroundColor(stack.isEmpty() ? 0 : 0x99000000);
        dim.setClickable(true);
        LinearLayout card = vbox();
        card.setBackground(round(DIALOG, 40, BORDER, 3));
        card.setPadding(px(50), px(50), px(50), px(44));
        card.addView(text(title, 44, TEXT, true));
        TextView b = text(body, 33, BODY, false);
        b.setPadding(0, px(22), 0, 0);
        b.setLineSpacing(px(10), 1f);
        card.addView(b);
        LinearLayout row = new LinearLayout(act);
        row.setOrientation(stacked ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        row.setPadding(0, px(40), 0, 0);
        for (int i = 0; i < buttons.length; i++) {
            final Btn bt = buttons[i];
            View v = pillButton(bt.label, bt.style, 34, () -> {
                closeDialog();
                if (bt.run != null) {
                    bt.run.run();
                }
            });
            LinearLayout.LayoutParams lp = stacked
                    ? new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                            ViewGroup.LayoutParams.WRAP_CONTENT)
                    : new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
            if (i > 0) {
                if (stacked) {
                    lp.topMargin = px(16);
                } else {
                    lp.leftMargin = px(20);
                }
            }
            row.addView(v, lp);
        }
        card.addView(row);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(Math.min(px(land ? 1200 : 920), w - px(80)),
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER);
        dim.addView(card, lp);
        dialog = dim;
        render();
    }

    void closeDialog() {
        if (dialog != null) {
            root.removeView(dialog);
            dialog = null;
        }
        pinned = false;
    }

    interface Anchored {
        void open(View anchor);
    }

    void menu(View anchor, Btn... items) {
        closePopup();
        int[] at = new int[2];
        int[] r0 = new int[2];
        anchor.getLocationInWindow(at);
        root.getLocationInWindow(r0);
        FrameLayout catcher = new FrameLayout(act);
        catcher.setClickable(true);
        catcher.setOnClickListener(v -> closePopup());
        LinearLayout box = vbox();
        box.setBackground(round(CARD2, 24, BORDER, 3));
        for (int i = 0; i < items.length; i++) {
            final Btn it = items[i];
            if (i > 0) {
                View line = new View(act);
                line.setBackgroundColor(LINE);
                box.addView(line, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, px(2))));
            }
            TextView t = text(it.label, 36, it.style == Style.DANGER ? RED : TEXT, false);
            t.setPadding(px(40), px(40), px(60), px(40));
            t.setOnClickListener(v -> {
                closePopup();
                if (it.run != null) {
                    it.run.run();
                }
            });
            box.addView(t);
        }
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        int rootW = root.getWidth();
        lp.topMargin = at[1] - r0[1] + anchor.getHeight() / 2 + px(24);
        lp.rightMargin = Math.max(px(20), rootW - (at[0] - r0[0] + anchor.getWidth()) + px(16));
        lp.gravity = Gravity.TOP | Gravity.END;
        catcher.addView(box, lp);
        popup = catcher;
        root.addView(popup);
    }

    void closePopup() {
        if (popup != null) {
            root.removeView(popup);
            popup = null;
        }
    }


    final class Col {
        final LinearLayout box = vbox();

        void add(View v, int gap) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.bottomMargin = px(gap);
            box.addView(v, lp);
        }

        void label(String s) {
            TextView t = text(s.toUpperCase(Locale.US), 28, SUB, true);
            t.setPadding(px(14), px(6), 0, 0);
            add(t, 12);
        }

        void note(String s) {
            note(s, 30);
        }

        void note(String s, float size) {
            TextView t = text(s, size, SUB, false);
            t.setPadding(px(20), 0, px(20), 0);
            t.setLineSpacing(px(12), 1f);
            add(t, 18);
        }
    }

    enum Kind { NAV, RADIO, PLAIN }

    /** A row: a card with a title, lines under it and what sits at its end. */
    static final class Row {
        String title;
        List<String> subs = new ArrayList<>();
        Kind kind = Kind.PLAIN;
        boolean selected;
        boolean highlight;
        List<String[]> pills = new ArrayList<>();
        Anchored menu;
        Btn button;
        float progress = -1;
        Runnable click;
    }

    View row(String title, List<String> subs, Kind kind, boolean selected, List<String[]> pills, Runnable click) {
        Row r = new Row();
        r.title = title;
        r.subs = subs;
        r.kind = kind;
        r.selected = selected;
        if (pills != null) {
            r.pills = pills;
        }
        r.click = click;
        return row(r);
    }

    View row(final Row r) {
        LinearLayout card = vbox();
        card.setBackground(round(CARD, 28, r.selected || r.highlight ? ACC : 0, r.selected || r.highlight ? 4 : 0));
        LinearLayout line = new LinearLayout(act);
        line.setOrientation(LinearLayout.HORIZONTAL);
        line.setGravity(Gravity.CENTER_VERTICAL);
        if (r.kind == Kind.RADIO) {
            line.addView(new Radio(r.selected), new LinearLayout.LayoutParams(px(120), px(80)));
        }
        LinearLayout t = vbox();
        t.setPadding(r.kind == Kind.RADIO ? 0 : px(44), px(34), px(16), px(34));
        t.addView(text(r.title, 40, TEXT, false));
        for (int i = 0; i < r.subs.size(); i++) {
            String s = r.subs.get(i);
            if (s == null || s.isEmpty()) {
                continue;
            }
            TextView v = text(s, 30, SUB, false);
            v.setPadding(0, px(i == 0 ? 8 : 2), 0, 0);
            t.addView(v);
        }
        if (!r.pills.isEmpty()) {
            LinearLayout pr = new LinearLayout(act);
            pr.setOrientation(LinearLayout.HORIZONTAL);
            pr.setPadding(0, px(12), 0, 0);
            for (String[] p : r.pills) {
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, px(48));
                lp.rightMargin = px(14);
                pr.addView(pill(p[0], pillText(p[1]), p[2].equals("green") ? GREEN : p[2].equals("amber")
                        ? AMBER : ACC), lp);
            }
            t.addView(pr);
        }
        line.addView(t, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        if (r.kind == Kind.NAV) {
            TextView ch = text("\u203a", 56, ACC, false);
            ch.setPadding(px(10), 0, px(44), px(8));
            line.addView(ch);
        }
        if (r.menu != null) {
            final Dots d = new Dots();
            d.setOnClickListener(v -> r.menu.open(v));
            line.addView(d, new LinearLayout.LayoutParams(px(110), px(110)));
        }
        if (r.button != null) {
            View b = pillButton(r.button.label, r.button.style, 32, r.button.run);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(px(250), px(80));
            lp.rightMargin = px(40);
            line.addView(b, lp);
        }
        card.addView(line);
        if (r.progress >= 0) {
            card.addView(new Bar(r.progress), barParams());
        }
        if (r.click != null) {
            card.setOnClickListener(v -> r.click.run());
        }
        return card;
    }

    LinearLayout.LayoutParams barParams() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, px(14));
        lp.leftMargin = px(44);
        lp.rightMargin = px(44);
        lp.bottomMargin = px(42);
        return lp;
    }

    View banner(String title, String body, Btn... buttons) {
        return banner(ACC, title, body, buttons);
    }

    /** A banner in the color of its kind: ACC for information, AMBER for a warning, RED for an error. */
    View banner(int color, String title, String body, Btn... buttons) {
        LinearLayout f = new LinearLayout(act);
        f.setOrientation(LinearLayout.HORIZONTAL);
        f.setBackground(round(CARD, 28, color, 4));
        View bar = new View(act);
        bar.setBackground(round(color, 6, 0, 0));
        LinearLayout.LayoutParams bl = new LinearLayout.LayoutParams(px(12), ViewGroup.LayoutParams.MATCH_PARENT);
        bl.topMargin = px(34);
        bl.bottomMargin = px(34);
        f.addView(bar, bl);
        LinearLayout c = vbox();
        c.setPadding(px(38), px(40), px(40), px(30));
        c.addView(text(title, 38, TEXT, true));
        TextView b = text(body, 31, BODY, false);
        b.setPadding(0, px(6), 0, 0);
        b.setLineSpacing(px(8), 1f);
        c.addView(b);
        LinearLayout row = new LinearLayout(act);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, px(28), 0, 0);
        for (int i = 0; i < buttons.length; i++) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, px(80));
            if (i > 0) {
                lp.leftMargin = px(20);
            }
            TextView v = pillButton(buttons[i].label, buttons[i].style, 34, buttons[i].run);
            v.setPadding(px(35), 0, px(35), 0);
            row.addView(v, lp);
        }
        if (buttons.length > 0) {
            c.addView(row);
        }
        f.addView(c, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        return f;
    }

    View activeCard(String heading, String name, List<String> lines) {
        LinearLayout c = vbox();
        c.setBackground(round(CARD, 28, 0, 0));
        c.setPadding(px(44), px(34), px(44), px(34));
        c.addView(text(heading, 28, GREEN, true));
        TextView n = text(name, 44, TEXT, true);
        n.setPadding(0, px(14), 0, 0);
        c.addView(n);
        for (String l : lines) {
            if (l != null && !l.isEmpty()) {
                TextView v = text(l, 30, SUB, false);
                v.setPadding(0, px(4), 0, 0);
                c.addView(v);
            }
        }
        return c;
    }

    View outline(String icon, String label, Runnable click) {
        TextView t = text(icon + "  " + label, 38, ACC, true);
        t.setGravity(Gravity.CENTER);
        t.setBackground(round(0, 28, ACC, 4));
        t.setMinHeight(px(110));
        t.setOnClickListener(v -> click.run());
        return t;
    }

    TextView pillButton(String label, Style st, float size, final Runnable click) {
        int fill;
        int fg;
        int stroke = 0;
        switch (st) {
            case PRIMARY: fill = ACC; fg = ON_ACC; break;
            case DANGER: fill = RED; fg = ON_RED; break;
            case GHOST: fill = 0; fg = ACC; stroke = ACC; break;
            default: fill = CARD2; fg = TEXT; break;
        }
        TextView t = text(label, size, fg, true);
        t.setGravity(Gravity.CENTER);
        t.setMaxLines(2);
        t.setEllipsize(android.text.TextUtils.TruncateAt.END);
        t.setPadding(px(30), px(12), px(30), px(12));
        t.setMinHeight(px(90));
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setCornerRadius(px(60));
        if (stroke != 0) {
            g.setStroke(px(3), stroke);
        }
        t.setBackground(g);
        t.setOnClickListener(v -> {
            if (click != null) {
                click.run();
            }
        });
        return t;
    }

    View pill(String label, int fg, int bg) {
        TextView t = text(label, 26, fg, true);
        t.setGravity(Gravity.CENTER);
        t.setPadding(px(18), 0, px(18), 0);
        t.setBackground(round(bg, 24, 0, 0));
        return t;
    }

    View header(String title, boolean dots, boolean withBack) {
        LinearLayout v = vbox();
        FrameLayout bar = new FrameLayout(act);
        if (withBack) {
            TextView back = text("\u2039", 80, TEXT, false);
            back.setGravity(Gravity.CENTER);
            back.setOnClickListener(x -> back());
            back.setContentDescription("Back");
            FrameLayout.LayoutParams bl = new FrameLayout.LayoutParams(px(100), ViewGroup.LayoutParams.MATCH_PARENT);
            bar.addView(back, bl);
        }
        TextView t = text(title, 52, TEXT, true);
        t.setGravity(Gravity.CENTER_VERTICAL);
        t.setSingleLine(true);
        t.setEllipsize(android.text.TextUtils.TruncateAt.END);
        FrameLayout.LayoutParams tl = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT);
        tl.leftMargin = px(withBack ? 100 : 50);
        tl.rightMargin = px(dots ? 120 : 30);
        bar.addView(t, tl);
        if (dots) {
            Dots d = new Dots();
            d.setOnClickListener(x -> menu(x, new Btn("Manage drivers", Style.SECONDARY, () -> push(Page.MANAGE))));
            FrameLayout.LayoutParams dl = new FrameLayout.LayoutParams(px(120), ViewGroup.LayoutParams.MATCH_PARENT,
                    Gravity.END);
            bar.addView(d, dl);
        }
        v.addView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, px(130)));
        View line = new View(act);
        line.setBackgroundColor(LINE);
        v.addView(line, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, px(2))));
        return v;
    }

    final class Radio extends View {
        final boolean on;
        final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);

        Radio(boolean on) {
            super(act);
            this.on = on;
        }

        @Override
        protected void onDraw(Canvas c) {
            float cx = px(62);
            float cy = getHeight() / 2f;
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(px(4));
            p.setColor(on ? ACC : 0xff6b7280);
            c.drawCircle(cx, cy, px(22), p);
            if (on) {
                p.setStyle(Paint.Style.FILL);
                p.setColor(ACC);
                c.drawCircle(cx, cy, px(11), p);
            }
        }
    }

    final class Dots extends View {
        final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);

        Dots() {
            super(act);
            p.setColor(DOTS);
            setClickable(true);
            setContentDescription("More options");
        }

        @Override
        protected void onDraw(Canvas c) {
            float cx = getWidth() / 2f;
            float cy = getHeight() / 2f;
            for (int d = -1; d <= 1; d++) {
                c.drawCircle(cx, cy + d * px(14), Math.max(2f, px(4.5f)), p);
            }
        }
    }

    final class Bar extends View {
        final float f;
        final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);

        Bar(float f) {
            super(act);
            this.f = f;
        }

        @Override
        protected void onDraw(Canvas c) {
            float h = getHeight();
            p.setColor(TRACK);
            c.drawRoundRect(0, 0, getWidth(), h, h / 2, h / 2, p);
            p.setColor(ACC);
            c.drawRoundRect(0, 0, getWidth() * f, h, h / 2, h / 2, p);
        }
    }

    LinearLayout vbox() {
        LinearLayout l = new LinearLayout(act);
        l.setOrientation(LinearLayout.VERTICAL);
        return l;
    }

    TextView text(String s, float size, int color, boolean bold) {
        TextView t = new TextView(act);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_PX, size * k);
        t.setTextColor(color);
        t.setIncludeFontPadding(false);
        if (bold) {
            t.setTypeface(Typeface.DEFAULT_BOLD);
        }
        return t;
    }

    Drawable round(int fill, float r, int stroke, float sw) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setCornerRadius(px(r));
        if (stroke != 0 && sw > 0) {
            g.setStroke(Math.max(1, px(sw)), stroke);
        }
        return g;
    }

    static List<String> list(String... s) {
        ArrayList<String> l = new ArrayList<>();
        for (String x : s) {
            l.add(x);
        }
        return l;
    }
}
