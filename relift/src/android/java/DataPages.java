package dev.relift.app;

import android.app.Activity;
import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.StatFs;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.File;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * The game data pages: the Game data row and page of the settings, the
 * check of the data in the game, and the import's own pages that
 * DataActivity shows (Game data needed, Looking, Ready to copy, Not enough
 * space, Copying, Interrupted, Ready). Drawn with Ui's widgets, as the
 * approved game data mockups show them.
 */
final class DataPages {
    static final String STATUS = "data-status.txt";
    static final String REDO = "data-redo.txt";
    static final String PENDING = "data-import.txt";

    private static GameData.Manifest manifest;
    private static boolean manifestRead;
    private static Check check;
    private static GameData.Plan left;
    private static boolean leftBusy;

    final Ui ui;
    /** The views a page in progress updates in place: rebuilding the page would drop a tap on its button. */
    private TextView liveBig;
    private TextView liveLine;
    private TextView liveNow;
    private Bar liveBar;
    private final List<TextView> liveValues = new ArrayList<>();

    DataPages(Ui ui) {
        this.ui = ui;
    }

    /** The game's list of files from the app's assets, or null when the app has none. */
    static synchronized GameData.Manifest manifest(Context a) {
        if (!manifestRead) {
            manifestRead = true;
            try (InputStream in = a.getAssets().open(GameData.MANIFEST)) {
                manifest = GameData.Manifest.parse(new String(GameData.readAll(in), StandardCharsets.UTF_8));
            } catch (Exception e) {
                manifest = null;
            }
        }
        return manifest;
    }

    /** The folder the game data goes in: the app's external files folder. */
    static File root(Activity a) {
        File d = a.getExternalFilesDir(null);
        return d != null ? d : new File(a.getFilesDir(), "external");
    }

    static File statusFile(Activity a) {
        return new File(a.getFilesDir(), STATUS);
    }

    static GameData.Status status(Activity a) {
        return GameData.Status.read(statusFile(a));
    }

    /** The files a check found damaged: the next import copies them again. */
    static Set<String> redo(Context a) {
        Set<String> s = new HashSet<>();
        String t = GameData.readText(new File(a.getFilesDir(), REDO));
        if (t != null) {
            for (String l : t.split("\n")) {
                if (!l.isEmpty()) {
                    s.add(l);
                }
            }
        }
        return s;
    }

    static long free(File root) {
        try {
            root.mkdirs();
            return new StatFs(root.getPath()).getAvailableBytes();
        } catch (IllegalArgumentException e) {
            return 0;
        }
    }

    /** Forgets what is known about the data in the game, after an import or a delete. */
    static synchronized void changed() {
        left = null;
    }

    /** What is left to import, worked out once off the main thread; null until then. */
    GameData.Plan left() {
        synchronized (DataPages.class) {
            if (left != null || leftBusy) {
                return left;
            }
            leftBusy = true;
        }
        final GameData.Manifest m = manifest(ui.act);
        final File root = root(ui.act);
        final Set<String> redo = redo(ui.act);
        new Thread(() -> {
            GameData.Plan p = m == null ? null : GameData.left(m, root, redo);
            synchronized (DataPages.class) {
                left = p;
                leftBusy = false;
            }
            ui.main.post(ui::render);
        }).start();
        return null;
    }

    boolean has() {
        return manifest(ui.act) != null;
    }

    GameData.Manifest m() {
        return manifest(ui.act);
    }

    String total() {
        GameData.Manifest m = m();
        return GameData.count(m.entries.size()) + " files, " + GameData.size(m.bytes);
    }

    /** The Game data row of Settings and of the launcher's settings. */
    View row() {
        GameData.Status s = status(ui.act);
        String sub;
        if (s == null) {
            sub = total();
        } else if (s.ok) {
            sub = total() + ", checked";
        } else if (deleted(s)) {
            sub = "Not on this phone";
        } else {
            sub = "Some game data is missing";
        }
        return ui.row("Game data", Ui.list(sub), Ui.Kind.NAV, false, null, () -> ui.push(Ui.Page.DATA));
    }

    static boolean deleted(GameData.Status s) {
        return GameData.Status.DELETED.equals(s.source);
    }

    boolean back(Ui.Page p) {
        switch (p) {
            case NEEDED:
            case LOOKING:
            case COPYING:
            case STOPPED:
            case DONE:
                return false;
            default:
                return true;
        }
    }

    String title(Ui.Page p) {
        switch (p) {
            case DATA: return "Game data";
            case DATA_CHECK: return "Check game data";
            case NEEDED: return "Game data needed";
            case CHOOSE: return "Import game data";
            case READY: return "Ready to copy";
            case SPACE: return "Not enough space";
            case COPYING: return "Copying game data";
            default: return "Game data";
        }
    }

    void build(Ui.Page p, Ui.Col a, Ui.Col b, boolean land) {
        switch (p) {
            case DATA: data(a, b); break;
            case DATA_CHECK: checking(a, b); break;
            case NEEDED: choose(a, b, true); break;
            case CHOOSE: choose(a, b, false); break;
            case LOOKING: looking(a, b); break;
            case READY: ready(a, b); break;
            case SPACE: space(a, b); break;
            case COPYING: copying(a, b); break;
            case STOPPED: stopped(a, b); break;
            default: done(a, b, land); break;
        }
    }

    DataActivity importer() {
        return ui.act instanceof DataActivity ? (DataActivity) ui.act : null;
    }

    // ------------------------------------------------------------------ the Game data page

    void data(Ui.Col a, Ui.Col b) {
        GameData.Status s = status(ui.act);
        GameData.Manifest m = m();
        if (s == null) {
            a.add(ui.activeCard("GAME DATA", "Not checked yet", Ui.list(total(),
                    "Checked when the game starts")), 26);
        } else if (s.ok) {
            String when = GameData.Status.CHECK.equals(s.source) ? "Every file checked"
                    : GameData.Status.IMPORT.equals(s.source) ? "Checked when it was copied"
                    : "Checked when the game started";
            a.add(ui.activeCard("GAME DATA", "Complete", Ui.list(total(), when)), 26);
        } else if (deleted(s)) {
            a.add(ui.banner(Ui.RED, "Game data needed", "The game data was deleted. The game can't start until you "
                    + "import it again.", new Ui.Btn("Import", Ui.Style.PRIMARY, this::importAgain)), 26);
        } else {
            a.add(ui.banner(Ui.RED, "Some game data is missing", s.problem().replace(".", "")
                    + ". The game may not start or may stop at some places.",
                    new Ui.Btn("Fix it", Ui.Style.PRIMARY, this::importAgain)), 26);
        }
        b.add(ui.row("Check game data", Ui.list("Compares every file with the game's list. Takes a few minutes."),
                Ui.Kind.NAV, false, null, this::startCheck), 26);
        b.add(ui.row("Import again", Ui.list("From a folder or a zip file"), Ui.Kind.NAV, false, null,
                this::importAgain), 26);
        b.add(ui.row("Delete game data", Ui.list("Frees " + GameData.size(m.bytes)
                + ". The game can't start until you import it again."), Ui.Kind.NAV, false, null,
                this::confirmDelete), 26);
    }

    /** In the game, closing it comes first; elsewhere the import opens at once. */
    void importAgain() {
        if (ui.launcher) {
            ui.host.openData(DataActivity.ACTION_IMPORT);
            return;
        }
        ui.message("Leave the game to import?", "The game has to close first, because it is using the files that "
                + "will be replaced. Progress you haven't saved in the game is lost.",
                new Ui.Btn("Cancel", Ui.Style.SECONDARY, null),
                new Ui.Btn("Close and import", Ui.Style.PRIMARY, () -> ui.host.openData(DataActivity.ACTION_IMPORT)));
    }

    void confirmDelete() {
        ui.message("Delete game data?", "This removes the " + GameData.size(m().bytes) + " of game data from this "
                + "phone. The game can't start until you import it again. Your saves and settings are kept.",
                new Ui.Btn("Cancel", Ui.Style.SECONDARY, null),
                new Ui.Btn("Delete", Ui.Style.DANGER, () -> ui.host.openData(DataActivity.ACTION_DELETE)));
    }

    /** Deletes the game data here and records it; runs off the main thread, then done on the main thread. */
    static void delete(final Activity a, final Runnable done) {
        new Thread(() -> {
            GameData.delete(root(a));
            new File(a.getFilesDir(), REDO).delete();
            GameData.Status s = new GameData.Status();
            s.source = GameData.Status.DELETED;
            s.write(statusFile(a));
            changed();
            a.runOnUiThread(done);
        }).start();
    }

    // ------------------------------------------------------------------ Check game data

    /** A check of every file in the game, running in this process. */
    static final class Check {
        volatile boolean stop;
        volatile boolean running = true;
        volatile int file;
        volatile long bytes;
        volatile String now = "";
        Ui ui;
    }

    void startCheck() {
        ui.push(Ui.Page.DATA_CHECK);
        synchronized (DataPages.class) {
            if (check != null && check.running) {
                check.ui = ui;
                return;
            }
            check = new Check();
            check.ui = ui;
        }
        final Check c = check;
        final GameData.Manifest m = m();
        final File root = root(ui.act);
        final Activity act = ui.act;
        new Thread(() -> {
            final long[] last = { 0 };
            GameData.Report r = null;
            try {
                r = GameData.check(m, root, true, n -> {
                    c.bytes += n;
                    long t = System.nanoTime();
                    if (t - last[0] > 150_000_000L) {
                        last[0] = t;
                        Ui u = c.ui;
                        u.main.post(u::refresh);
                    }
                    return !c.stop;
                }, (i, e) -> {
                    c.file = i;
                    c.now = e.shown();
                });
            } catch (Exception e) {
                r = null;
            }
            c.running = false;
            if (r != null) {
                GameData.Status.of(r, GameData.Status.CHECK).write(statusFile(act));
                StringBuilder redo = new StringBuilder();
                for (String n : r.damagedNames) {
                    redo.append(n).append('\n');
                }
                GameData.writeText(new File(act.getFilesDir(), REDO), redo.toString());
                changed();
            }
            final Ui u = c.ui;
            u.main.post(() -> {
                if (!u.stack.isEmpty() && u.stack.get(u.stack.size() - 1) == Ui.Page.DATA_CHECK) {
                    u.stack.remove(u.stack.size() - 1);
                }
                u.render();
            });
        }).start();
    }

    /** The big count, the bytes line, the bar's fraction and the file now, of the check in progress. */
    private Object[] checkState() {
        Check c = check;
        GameData.Manifest m = m();
        int f = c == null ? 0 : c.file;
        long done = c == null ? 0 : c.bytes;
        return new Object[] { GameData.count(f) + " of " + GameData.count(m.entries.size()) + " files",
            GameData.size(done) + " of " + GameData.size(m.bytes), m.bytes == 0 ? 0f : (float) done / m.bytes,
            c == null || c.now.isEmpty() ? null : "Now: " + c.now, null };
    }

    /** As checkState, for the copy, with the values of its card. */
    private Object[] copyState() {
        DataImport j = importer().job;
        int files = j.filesDone;
        long bytes = j.bytesDone;
        return new Object[] { GameData.count(files) + " of " + GameData.count(j.filesTotal) + " files",
            GameData.size(bytes) + " of " + GameData.size(j.bytesTotal),
            j.bytesTotal == 0 ? 0f : (float) bytes / j.bytesTotal, j.now.isEmpty() ? null : "Now: " + j.now,
            new String[] { GameData.count(files), GameData.count(Math.max(0, j.filesTotal - files)),
                GameData.size(Math.max(0, j.bytesTotal - bytes)) } };
    }

    /** Updates the page in progress in place; false when p is not one or its views are gone. */
    boolean refresh(Ui.Page p) {
        if (liveBig == null || p != Ui.Page.COPYING && p != Ui.Page.DATA_CHECK
                || p == Ui.Page.COPYING && importer() == null) {
            return false;
        }
        Object[] s = p == Ui.Page.COPYING ? copyState() : checkState();
        liveBig.setText((String) s[0]);
        liveLine.setText((String) s[1]);
        liveBar.set((Float) s[2]);
        liveNow.setText(s[3] == null ? "" : (String) s[3]);
        liveNow.setVisibility(s[3] == null ? View.GONE : View.VISIBLE);
        String[] v = (String[]) s[4];
        for (int i = 0; v != null && i < v.length && i < liveValues.size(); i++) {
            liveValues.get(i).setText(v[i]);
        }
        return true;
    }

    void checking(Ui.Col a, Ui.Col b) {
        Check c = check;
        Object[] s = checkState();
        a.add(progress("Checking game data", Ui.GREEN, (String) s[0], (String) s[1], (Float) s[2], (String) s[3]),
                26);
        b.note("Nothing is copied. Each file is read and compared with the game's list. You can leave this page; "
                + "the check carries on.");
        b.add(big("Stop", Ui.Style.SECONDARY, null, null, () -> {
            if (c != null) {
                c.stop = true;
            }
            ui.back();
        }), 26);
    }

    // ------------------------------------------------------------------ the import's pages

    void choose(Ui.Col a, Ui.Col b, boolean needed) {
        a.add(ui.hero(), 26);
        if (needed) {
            a.add(ui.banner(Ui.ACC, "Game data needed", "This game can't start until its data is on this phone. Copy "
                    + "the game data folder that relift exported on your computer to this phone, anywhere you like. "
                    + "Then choose it here."), 26);
        }
        DataActivity d = importer();
        b.add(big("Choose folder", Ui.Style.PRIMARY, "\uD83D\uDCC1", "The folder you copied to this phone",
                () -> d.pickFolder(null)), 26);
        b.add(big("Choose zip file", Ui.Style.SECONDARY, "\u25A4", "Or a zip file of that folder",
                () -> d.pickZip(null)), 26);
        GameData.Plan l = left();
        long need = l == null ? m().bytes : m().bytes - l.presentBytes;
        b.note("Needs " + GameData.size(need) + " of free space on this phone. The files are copied into this game "
                + "and checked as they go. Your own copy is not changed.");
        b.note("Tip: put it in its own folder, for example Download > Game data. Android won't let an app use the "
                + "top of your storage or the whole Download folder.", 28);
    }

    void looking(Ui.Col a, Ui.Col b) {
        DataActivity d = importer();
        a.add(ui.hero(), 26);
        b.add(progress("Looking at your " + (d.job.zip ? "zip file" : "folder"), Ui.ACC, "Counting files\u2026",
                "Checking them against this game's list", 0, null), 26);
        b.note("Nothing is copied yet. This takes a moment for a large folder.");
        b.add(big("Cancel", Ui.Style.SECONDARY, null, null, d::cancelLook), 26);
    }

    void ready(Ui.Col a, Ui.Col b) {
        DataActivity d = importer();
        GameData.Plan p = d.job.plan;
        long free = free(root(ui.act));
        a.add(ui.hero(), 26);
        if (p.partial()) {
            a.add(item("Found part of the game data", Ui.AMBER, "\u2022", GameData.count(p.found) + " of "
                    + GameData.count(p.files) + " files are there; " + GameData.count(p.missing.size() + p.wrong.size())
                    + " are missing or the wrong size."), 18);
        } else if (p.present > 0) {
            a.add(item("Found the game data", Ui.GREEN, "\u2713", "The " + GameData.count(p.copy.size())
                    + " files still needed are there, " + GameData.size(p.copyBytes) + " in total."), 18);
        } else {
            a.add(item("Found the game data", Ui.GREEN, "\u2713", "All " + GameData.count(p.files)
                    + " files of this game are there, " + GameData.size(p.bytes) + " in total."), 18);
        }
        a.add(item("Enough free space", Ui.GREEN, "\u2713", "Needs " + GameData.size(p.copyBytes)
                + ", this phone has " + GameData.size(free) + " free."), 18);
        b.note("The files are copied into this game and each one is checked. Keep the phone plugged in; it takes a "
                + "while. You can leave the app: the copy keeps going and picks up where it stopped if it is "
                + "interrupted.");
        b.add(big("Start copying", Ui.Style.PRIMARY, null, GameData.count(p.copy.size()) + " files \u00b7 "
                + GameData.size(p.copyBytes), d::startCopy), 26);
        b.add(big("Choose something else", Ui.Style.GHOST, null, null, d::chooseAgain), 26);
    }

    void space(Ui.Col a, Ui.Col b) {
        DataActivity d = importer();
        GameData.Plan p = d.job.plan;
        long free = free(root(ui.act));
        long need = p.copyBytes;
        long shortBy = Math.max(p.shortBy(free), 1);
        boolean during = d.job.copiedThisRun > 0;
        a.add(ui.hero(), 26);
        a.add(ui.banner(Ui.RED, "Not enough free space", "The game data needs " + GameData.size(need)
                + ". This phone has " + GameData.size(free) + " free, so it is short by " + GameData.size(shortBy)
                + (during ? ". What was copied so far is kept." : ". Nothing was copied.")), 26);
        b.add(values(new String[][] { { "Needs", GameData.size(need), null },
            { "Free on this phone", GameData.size(free), null }, { "Short by", GameData.size(shortBy), "red" } }), 26);
        b.note("Free up room by deleting apps, photos or downloads you don't need. Then choose the game data again.");
        b.add(buttons(new Ui.Btn("Try again", Ui.Style.PRIMARY, d::spaceAgain),
                new Ui.Btn("Choose something else", Ui.Style.SECONDARY, d::chooseAgain)), 26);
    }

    void copying(Ui.Col a, Ui.Col b) {
        DataImport j = importer().job;
        Object[] s = copyState();
        String[] v = (String[]) s[4];
        a.add(ui.hero(), 26);
        a.add(progress("Copying and checking", Ui.GREEN, (String) s[0], (String) s[1], (Float) s[2], (String) s[3]),
                26);
        b.add(values(new String[][] { { "Files checked", v[0], "green" }, { "Files left", v[1], null },
            { "Still needed on this phone", v[2], null } }), 26);
        b.note("Keep this screen open or leave the app; the copy continues. If it stops, you can continue later from "
                + "where it was.");
        b.add(big("Stop for now", Ui.Style.SECONDARY, null, null, j::stop), 26);
    }

    void stopped(Ui.Col a, Ui.Col b) {
        DataActivity d = importer();
        GameData.Plan l = left();
        GameData.Manifest m = m();
        String done = l == null ? "\u2026" : GameData.count(l.present);
        a.add(ui.hero(), 26);
        a.add(ui.banner(Ui.AMBER, "The copy was interrupted", done + " of " + GameData.count(m.entries.size())
                + " files were copied and checked before it stopped. Those stay. You can continue with the rest."), 26);
        b.add(values(new String[][] { { "Already copied and checked", done, "green" },
            { "Left to copy", l == null ? "\u2026" : GameData.count(m.entries.size() - l.present), null },
            { "Needs", l == null ? "\u2026" : GameData.size(m.bytes - l.presentBytes), null },
            { "Free on this phone", GameData.size(free(root(ui.act))), null } }), 26);
        b.add(big("Continue", Ui.Style.PRIMARY, null, "Choose the same folder or zip again", d::resume), 26);
        b.add(big("Start over", Ui.Style.SECONDARY, null, "Deletes what was copied so far", d::startOver), 26);
    }

    void done(Ui.Col a, Ui.Col b, boolean land) {
        DataActivity d = importer();
        GameData.Manifest m = m();
        a.add(ui.hero(), 26);
        a.add(success("All " + GameData.count(m.entries.size()) + " files copied and checked ("
                + GameData.size(m.bytes) + ").", land ? null : "You can delete your own copy now to free space."), 26);
        if (land) {
            b.note("You can delete your own copy now to free space. The game does not need it any more.");
        }
        b.add(big("Start game", Ui.Style.PRIMARY, null, null, d::startGame), 26);
    }

    // ------------------------------------------------------------------ widgets of these pages

    /** A big button: a label (with an icon) and a line under it. */
    View big(String label, Ui.Style st, String icon, String sub, final Runnable click) {
        int fill;
        int fg;
        int subColor;
        int stroke = 0;
        switch (st) {
            case PRIMARY: fill = Ui.ACC; fg = Ui.ON_ACC; subColor = Ui.ON_ACC_SUB; break;
            case DANGER: fill = Ui.RED; fg = Ui.ON_RED; subColor = fg; break;
            case GHOST: fill = 0; fg = Ui.ACC; stroke = Ui.ACC; subColor = fg; break;
            default: fill = Ui.CARD2; fg = Ui.TEXT; subColor = fg; break;
        }
        LinearLayout v = ui.vbox();
        v.setGravity(Gravity.CENTER);
        v.setMinimumHeight(ui.px(sub == null ? 120 : 150));
        v.setPadding(ui.px(30), ui.px(18), ui.px(30), ui.px(18));
        v.setBackground(ui.round(fill, 28, stroke, 4));
        TextView t = ui.text((icon == null ? "" : icon + "  ") + label, 40, fg, true);
        t.setGravity(Gravity.CENTER);
        v.addView(t, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        if (sub != null) {
            TextView s = ui.text(sub, 28, subColor, false);
            s.setGravity(Gravity.CENTER);
            s.setPadding(0, ui.px(14), 0, 0);
            v.addView(s, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT));
        }
        v.setContentDescription(label);
        v.setOnClickListener(x -> {
            if (click != null) {
                click.run();
            }
        });
        return v;
    }

    /** Buttons of equal width on one line. */
    View buttons(Ui.Btn... bs) {
        LinearLayout row = new LinearLayout(ui.act);
        row.setOrientation(LinearLayout.HORIZONTAL);
        for (int i = 0; i < bs.length; i++) {
            TextView t = ui.pillButton(bs[i].label, bs[i].style, 34, bs[i].run);
            t.setMinHeight(ui.px(100));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
            if (i > 0) {
                lp.leftMargin = ui.px(20);
            }
            row.addView(t, lp);
        }
        return row;
    }

    /** The progress card: a heading, the big count, a bar, the bytes and the file in progress. */
    View progress(String head, int headColor, String big, String line, float frac, String now) {
        LinearLayout c = ui.vbox();
        c.setBackground(ui.round(Ui.CARD, 28, 0, 0));
        c.setPadding(ui.px(44), ui.px(32), ui.px(44), ui.px(36));
        c.addView(ui.text(head.toUpperCase(java.util.Locale.US), 28, headColor, true));
        TextView b = ui.text(big, 52, Ui.TEXT, true);
        b.setPadding(0, ui.px(34), 0, 0);
        c.addView(b);
        LinearLayout.LayoutParams bl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ui.px(26));
        bl.topMargin = ui.px(34);
        Bar bar = new Bar(frac);
        c.addView(bar, bl);
        TextView l = ui.text(line, 34, Ui.BODY, false);
        l.setPadding(0, ui.px(30), 0, 0);
        c.addView(l);
        TextView n = ui.text(now == null ? "" : now, 28, Ui.SUB, false);
        n.setPadding(0, ui.px(22), 0, 0);
        n.setSingleLine(true);
        n.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
        n.setVisibility(now == null ? View.GONE : View.VISIBLE);
        c.addView(n);
        liveBig = b;
        liveLine = l;
        liveNow = n;
        liveBar = bar;
        return c;
    }

    /** A card of label and value lines; a value's color is "green", "red" or null (white). */
    View values(String[][] rows) {
        LinearLayout c = ui.vbox();
        c.setBackground(ui.round(Ui.CARD, 28, 0, 0));
        c.setPadding(ui.px(44), ui.px(26), ui.px(44), ui.px(26));
        liveValues.clear();
        for (String[] r : rows) {
            LinearLayout line = new LinearLayout(ui.act);
            line.setOrientation(LinearLayout.HORIZONTAL);
            line.setGravity(Gravity.CENTER_VERTICAL);
            line.setMinimumHeight(ui.px(66));
            line.addView(ui.text(r[0], 34, Ui.BODY, false),
                    new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
            int col = "green".equals(r[2]) ? Ui.GREEN : "red".equals(r[2]) ? Ui.RED : Ui.TEXT;
            TextView v = ui.text(r[1], 34, col, true);
            v.setPadding(ui.px(20), 0, 0, 0);
            line.addView(v);
            liveValues.add(v);
            c.addView(line);
        }
        return c;
    }

    /** A line of a check with its mark: a check mark, a cross or a dot in a tinted circle. */
    View item(String text, int color, String mark, String sub) {
        LinearLayout card = new LinearLayout(ui.act);
        card.setOrientation(LinearLayout.HORIZONTAL);
        card.setBackground(ui.round(Ui.CARD, 28, 0, 0));
        card.setPadding(ui.px(38), ui.px(32), ui.px(40), ui.px(30));
        TextView m = ui.text(mark, 30, color, true);
        m.setGravity(Gravity.CENTER);
        GradientDrawable g = new GradientDrawable();
        g.setShape(GradientDrawable.OVAL);
        g.setColor((color & 0x00ffffff) | 0x2e000000);
        m.setBackground(g);
        card.addView(m, new LinearLayout.LayoutParams(ui.px(48), ui.px(48)));
        LinearLayout t = ui.vbox();
        t.setPadding(ui.px(24), 0, 0, 0);
        t.addView(ui.text(text, 36, Ui.TEXT, false));
        if (sub != null) {
            TextView s = ui.text(sub, 28, Ui.SUB, false);
            s.setPadding(0, ui.px(10), 0, 0);
            s.setLineSpacing(ui.px(8), 1f);
            t.addView(s);
        }
        card.addView(t, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        return card;
    }

    /** The green card of a finished import. */
    View success(String line, String hint) {
        LinearLayout c = ui.vbox();
        c.setGravity(Gravity.CENTER_HORIZONTAL);
        c.setBackground(ui.round(Ui.CARD, 28, Ui.GREEN, 4));
        c.setPadding(ui.px(40), ui.px(56), ui.px(40), ui.px(44));
        TextView m = ui.text("\u2713", 80, Ui.GREEN, true);
        m.setGravity(Gravity.CENTER);
        GradientDrawable g = new GradientDrawable();
        g.setShape(GradientDrawable.OVAL);
        g.setColor((Ui.GREEN & 0x00ffffff) | 0x2e000000);
        m.setBackground(g);
        c.addView(m, new LinearLayout.LayoutParams(ui.px(128), ui.px(128)));
        TextView t = ui.text("Game data is ready", 48, Ui.TEXT, true);
        t.setGravity(Gravity.CENTER);
        t.setPadding(0, ui.px(44), 0, 0);
        c.addView(t);
        TextView l = ui.text(line, 32, Ui.BODY, false);
        l.setGravity(Gravity.CENTER);
        l.setPadding(0, ui.px(26), 0, 0);
        c.addView(l);
        if (hint != null) {
            TextView h = ui.text(hint, 30, Ui.SUB, false);
            h.setGravity(Gravity.CENTER);
            h.setPadding(0, ui.px(16), 0, 0);
            c.addView(h);
        }
        return c;
    }

    final class Bar extends View {
        float f;
        final android.graphics.Paint p = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);

        Bar(float f) {
            super(ui.act);
            this.f = Math.max(0f, Math.min(1f, f));
        }

        void set(float v) {
            f = Math.max(0f, Math.min(1f, v));
            invalidate();
        }

        @Override
        protected void onDraw(android.graphics.Canvas c) {
            float h = getHeight();
            p.setColor(Ui.TRACK);
            c.drawRoundRect(0, 0, getWidth(), h, h / 2, h / 2, p);
            p.setColor(Ui.ACC);
            c.drawRoundRect(0, 0, Math.max(h, getWidth() * f), h, h / 2, h / 2, p);
        }
    }
}
