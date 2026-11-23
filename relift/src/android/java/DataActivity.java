package dev.relift.app;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.util.Log;
import android.window.OnBackInvokedDispatcher;

/**
 * The game data import, in the app's ":data" process: Game data needed, the
 * choice of a folder or zip through Android's pickers, the look at it, the
 * space check, the copy (DataService) and its end. Opened by the game when
 * its start-up check finds the data missing, by Import again, and by the
 * copy's notification.
 */
public class DataActivity extends Activity implements Ui.Host, DataImport.Listener {
    /** The intent extra that says what to open. */
    static final String EXTRA = "relift_data";
    /** Import again: the choice of a folder or zip, without the "needed" banner. */
    static final String ACTION_IMPORT = "import";
    /** Delete the game data, asked for on a Game data page. */
    static final String ACTION_DELETE = "delete";
    static final int REQ_FOLDER = 0x5245;
    static final int REQ_ZIP = 0x5246;
    static final int REQ_NOTIFY = 0x5247;

    Ui ui;
    DataImport job;
    private Ui.Page base = Ui.Page.NEEDED;
    private boolean resuming;

    /** Opens the import from another activity of the app. */
    static void open(Context c, String action) {
        Intent i = new Intent(c, DataActivity.class);
        if (action != null) {
            i.putExtra(EXTRA, action);
        }
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        c.startActivity(i);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        job = DataImport.get(this);
        ui = new Ui(this, new Drivers(this), this, true);
        setContentView(ui.root);
        if (Build.VERSION.SDK_INT >= 33) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT,
                    () -> ui.back());
        }
        route(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        route(intent);
    }

    private void route(Intent intent) {
        String action = intent == null ? null : intent.getStringExtra(EXTRA);
        base = ACTION_IMPORT.equals(action) ? Ui.Page.CHOOSE : Ui.Page.NEEDED;
        if (job.running) {
            show(Ui.Page.COPYING);
        } else if (job.outcome == DataImport.Outcome.DONE) {
            show(Ui.Page.DONE);
        } else if (job.outcome == DataImport.Outcome.PARTIAL_DONE) {
            show(Ui.Page.DATA);
        } else if (GameData.Pending.read(job.pending) != null) {
            show(Ui.Page.STOPPED);
        } else if (ACTION_DELETE.equals(action)) {
            openData(ACTION_DELETE);
        } else {
            show(base);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        job.listener = this;
        changed();
    }

    @Override
    protected void onPause() {
        super.onPause();
        job.listener = null;
    }

    @Override
    public void onBackPressed() {
        ui.back();
    }

    private Ui.Page top() {
        return ui.stack.isEmpty() ? null : ui.stack.get(ui.stack.size() - 1);
    }

    /** Shows page p: alone, or over the choice for the pages that go back to it. */
    void show(Ui.Page p) {
        if (top() == p && ui.showing()) {
            ui.refresh();
            return;
        }
        if (p == Ui.Page.READY || p == Ui.Page.SPACE) {
            ui.open(base);
            ui.push(p);
        } else {
            ui.open(p);
        }
    }

    @Override
    public void changed() {
        DataImport.Outcome o = job.outcome;
        boolean was = resuming;
        if (o != DataImport.Outcome.LOOKING) {
            resuming = false;
        }
        switch (o) {
            case LOOKING: show(Ui.Page.LOOKING); return;
            case READY: show(Ui.Page.READY); return;
            case SHORT: show(Ui.Page.SPACE); return;
            case COPYING: show(Ui.Page.COPYING); return;
            case DONE: show(Ui.Page.DONE); return;
            case PARTIAL_DONE: show(Ui.Page.DATA); return;
            case STOPPED: show(Ui.Page.STOPPED); return;
            case NONE: return;
            default: break;
        }
        job.outcome = DataImport.Outcome.NONE;
        final Uri again = job.uri;
        if (was && (o == DataImport.Outcome.LOST || o == DataImport.Outcome.BAD_ZIP)) {
            show(Ui.Page.STOPPED);
            if (job.zip) {
                pickZip(again);
            } else {
                pickFolder(again);
            }
            return;
        }
        boolean copy = GameData.Pending.read(job.pending) != null;
        show(copy ? Ui.Page.STOPPED : base);
        String what = job.zip ? "zip file" : "folder";
        switch (o) {
            case NOT_FOUND: {
                GameData.Entry e = job.plan == null ? null : job.plan.firstBad();
                ui.message("This isn't the game data", "None of the files this game needs are in this " + what
                        + ". The first one it looked for is missing: " + (e == null ? "" : e.shown())
                        + ". Choose the " + what + " that holds the game data you exported with relift.",
                        new Ui.Btn("Choose another " + (job.zip ? "zip" : "folder"), Ui.Style.PRIMARY,
                                this::chooseSame),
                        new Ui.Btn("Cancel", Ui.Style.SECONDARY, null));
                break;
            }
            case INSIDE: {
                final String name = job.found.root.name;
                ui.message("Try the folder inside", "The game data was not found here, but it looks like it is in a "
                        + "folder inside this one: " + name + ". Use that folder instead?",
                        new Ui.Btn("Use \u201c" + name + "\u201d", Ui.Style.PRIMARY,
                                () -> job.look(job.uri, false, name, job.partialOk)),
                        new Ui.Btn("Choose another", Ui.Style.SECONDARY, this::chooseSame));
                break;
            }
            case ABOVE: {
                final Uri up = job.aboveUri;
                String name = job.aboveName;
                ui.message("Try the folder around it", "This looks like a folder inside the game data, not the game "
                        + "data itself. Choose the folder that holds it" + (name.isEmpty() ? "" : ": " + name) + ".",
                        new Ui.Btn(name.isEmpty() ? "Choose folder" : "Choose \u201c" + name + "\u201d",
                                Ui.Style.PRIMARY, () -> pickFolder(up)),
                        new Ui.Btn("Choose another", Ui.Style.SECONDARY, this::chooseSame));
                break;
            }
            case PARTIAL: {
                GameData.Plan p = job.plan;
                GameData.Status s = new GameData.Status();
                s.missing = p.missing.size();
                s.wrong = p.wrong.size();
                String problem = s.problem();
                ui.message("Some game data is missing", "We found most of the game data but "
                        + problem.substring(0, problem.length() - 1) + ". The first missing file is "
                        + p.firstBad().shown() + ". The export may be incomplete. Export it again on your computer "
                        + "and copy it over.", new Ui.Btn("Choose another", Ui.Style.PRIMARY, this::chooseSame),
                        new Ui.Btn("Copy what is there", Ui.Style.SECONDARY, this::copyWhatIsThere));
                break;
            }
            case BAD_ZIP:
                ui.message("Couldn't read this zip", "The file may be damaged or not fully copied to this phone. Copy "
                        + "it again and try again.", new Ui.Btn("OK", Ui.Style.SECONDARY, null));
                break;
            case LOST:
                ui.message("Lost access to the folder", "Android no longer lets this app read the folder you picked. "
                        + "Choose it again; the files already copied stay.",
                        new Ui.Btn("Choose folder", Ui.Style.PRIMARY, () -> pickFolder(again)),
                        new Ui.Btn("Cancel", Ui.Style.SECONDARY, null));
                break;
            case MISMATCH:
                ui.message("A file didn't match", (job.bad == null ? "A file" : job.bad.shown())
                        + " was copied but did not match the game's list. Your copy of it may be damaged. Nothing "
                        + "wrong is kept.", new Ui.Btn("Try this file again", Ui.Style.PRIMARY, this::startCopy),
                        new Ui.Btn("Stop", Ui.Style.SECONDARY, null));
                break;
            case WRITE:
                ui.message("Can't write to this phone's storage", "The game data could not be saved. The storage may "
                        + "be full or have a problem. What was copied so far is kept.",
                        new Ui.Btn("Try again", Ui.Style.PRIMARY, this::startCopy),
                        new Ui.Btn("Stop", Ui.Style.SECONDARY, null));
                break;
            default:
                break;
        }
    }

    void pickFolder(Uri initial) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        initial(i, initial);
        pick(i, REQ_FOLDER);
    }

    void pickZip(Uri initial) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] { "application/zip", "application/x-zip-compressed",
            "application/octet-stream", "*/*" });
        initial(i, initial);
        pick(i, REQ_ZIP);
    }

    private static void initial(Intent i, Uri u) {
        if (u == null) {
            return;
        }
        Uri at = u;
        if (DocumentsContract.isTreeUri(u)) {
            at = DocumentsContract.buildDocumentUriUsingTree(u, DocumentsContract.getTreeDocumentId(u));
        }
        i.putExtra(DocumentsContract.EXTRA_INITIAL_URI, at);
    }

    private void pick(Intent i, int req) {
        try {
            startActivityForResult(i, req);
        } catch (Exception e) {
            Log.w(Drivers.TAG, "no document picker: " + e);
        }
    }

    @Override
    protected void onActivityResult(int request, int code, Intent data) {
        super.onActivityResult(request, code, data);
        if (code != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        if (request == REQ_FOLDER || request == REQ_ZIP) {
            GameData.Pending p = GameData.Pending.read(job.pending);
            boolean same = p != null && p.uri.equals(data.getData().toString());
            job.look(data.getData(), request == REQ_ZIP, same ? p.inside : "", p != null && p.partial);
        }
    }

    void chooseSame() {
        if (job.zip) {
            pickZip(null);
        } else {
            pickFolder(null);
        }
    }

    void chooseAgain() {
        job.cancelLook();
        show(base);
    }

    void cancelLook() {
        job.cancelLook();
        show(GameData.Pending.read(job.pending) != null ? Ui.Page.STOPPED : base);
    }

    void copyWhatIsThere() {
        job.partialOk = true;
        job.outcome = job.space();
        changed();
    }

    void spaceAgain() {
        job.outcome = job.space();
        changed();
    }

    void startCopy() {
        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] { Manifest.permission.POST_NOTIFICATIONS }, REQ_NOTIFY);
            return;
        }
        job.startCopy();
    }

    @Override
    public void onRequestPermissionsResult(int request, String[] perms, int[] results) {
        super.onRequestPermissionsResult(request, perms, results);
        if (request == REQ_NOTIFY) {
            job.startCopy();
        }
    }

    /** Continue: the same folder or zip again; Android's picker opens if the app can no longer read it. */
    void resume() {
        GameData.Pending p = GameData.Pending.read(job.pending);
        if (p == null) {
            show(base);
            return;
        }
        resuming = true;
        job.look(Uri.parse(p.uri), p.zip(), p.inside, p.partial);
    }

    void startOver() {
        job.cancelLook();
        new Thread(() -> {
            GameData.delete(job.root);
            job.pending.delete();
            DataPages.changed();
            runOnUiThread(() -> show(base));
        }).start();
    }

    void startGame() {
        job.outcome = DataImport.Outcome.NONE;
        Intent launch = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (launch != null) {
            launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startActivity(launch);
        }
        finishAndRemoveTask();
    }

    @Override
    public void closed(int next) {
        finish();
    }

    @Override
    public void restart() {
    }

    @Override
    public String loaded() {
        return null;
    }

    @Override
    public String loadedMath() {
        return null;
    }

    @Override
    public void openData(String action) {
        if (ACTION_DELETE.equals(action)) {
            DataPages.delete(this, () -> {
                base = Ui.Page.NEEDED;
                show(base);
            });
            return;
        }
        base = Ui.Page.CHOOSE;
        job.outcome = DataImport.Outcome.NONE;
        show(base);
    }
}
