package dev.relift.app;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.util.Log;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * The game data import of this process (DataActivity and DataService run in
 * the app's ":data" process): looking at what the player picked through
 * Android's document access, and the copy, run by the foreground service.
 * GameData decides; this reads folders and files and keeps the state the
 * pages show.
 */
final class DataImport {
    /** What the last step found; the activity shows it and sets it back to NONE. */
    enum Outcome {
        NONE, LOOKING, READY, SHORT, NOT_FOUND, INSIDE, ABOVE, PARTIAL, BAD_ZIP, LOST,
        COPYING, DONE, PARTIAL_DONE, STOPPED, MISMATCH, WRITE
    }

    /** Told on the main thread when the state changes. */
    interface Listener {
        void changed();
    }

    private static DataImport instance;

    final Context app;
    final File root;
    final File pending;
    final Handler main = new Handler(Looper.getMainLooper());
    Listener listener;

    Uri uri;
    boolean zip;
    String inside = "";
    boolean partialOk;
    GameData.Manifest manifest;
    GameData.Found found;
    GameData.Plan plan;
    Map<String, GameData.Node> nodes;
    Saf saf;
    GameData.Zip archive;
    ParcelFileDescriptor fd;

    volatile Outcome outcome = Outcome.NONE;
    volatile boolean running;
    volatile boolean stop;
    volatile int lookId;
    volatile int filesDone;
    volatile int filesTotal;
    volatile long bytesDone;
    volatile long bytesTotal;
    volatile String now = "";
    volatile int copiedThisRun;
    GameData.Entry bad;
    String aboveName = "";
    Uri aboveUri;
    private long lastTick;

    private DataImport(Context c) {
        app = c.getApplicationContext();
        File ext = app.getExternalFilesDir(null);
        root = ext != null ? ext : new File(app.getFilesDir(), "external");
        pending = new File(app.getFilesDir(), DataPages.PENDING);
    }

    static synchronized DataImport get(Context c) {
        if (instance == null) {
            instance = new DataImport(c);
        }
        return instance;
    }

    /** The import of this process, or null when none was made here. */
    static synchronized DataImport peek() {
        return instance;
    }

    void post() {
        main.post(() -> {
            if (listener != null) {
                listener.changed();
            }
        });
    }

    private void tick() {
        long t = System.nanoTime();
        if (t - lastTick > 100_000_000L) {
            lastTick = t;
            post();
            DataService.progress(app, this);
        }
    }

    // ------------------------------------------------------------------ reading what was picked

    /** A folder tree the player picked, listed one query per folder. */
    static final class Saf implements GameData.Lister {
        static final String[] COLUMNS = { DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE };
        final ContentResolver cr;
        final Uri tree;

        Saf(ContentResolver cr, Uri tree) {
            this.cr = cr;
            this.tree = tree;
        }

        GameData.Node root() throws IOException {
            String id = DocumentsContract.getTreeDocumentId(tree);
            String name = id;
            try (Cursor c = cr.query(DocumentsContract.buildDocumentUriUsingTree(tree, id), COLUMNS, null, null,
                    null)) {
                if (c != null && c.moveToFirst()) {
                    name = c.getString(1);
                }
            }
            return new GameData.Node(name, true, 0, id);
        }

        @Override
        public List<GameData.Node> list(GameData.Node dir) throws IOException {
            List<GameData.Node> out = new ArrayList<>();
            Uri kids = DocumentsContract.buildChildDocumentsUriUsingTree(tree, (String) dir.ref);
            try (Cursor c = cr.query(kids, COLUMNS, null, null, null)) {
                if (c == null) {
                    throw new FileNotFoundException(dir.name + ": cannot be listed");
                }
                while (c.moveToNext()) {
                    boolean isDir = DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2));
                    out.add(new GameData.Node(c.getString(1), isDir, c.isNull(3) ? 0 : c.getLong(3), c.getString(0)));
                }
            }
            return out;
        }

        InputStream open(GameData.Node n) throws IOException {
            InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, (String) n.ref));
            if (in == null) {
                throw new FileNotFoundException(n.name);
            }
            return in;
        }
    }

    /** Positioned reads of a picked file's descriptor; nothing is opened again by path. */
    static final class Fd implements GameData.Bytes {
        final FileChannel ch;
        final long size;

        Fd(ParcelFileDescriptor pfd) throws IOException {
            ch = new FileInputStream(pfd.getFileDescriptor()).getChannel();
            long s = pfd.getStatSize();
            size = s >= 0 ? s : ch.size();
        }

        @Override
        public long size() {
            return size;
        }

        @Override
        public int read(long pos, byte[] b, int off, int len) throws IOException {
            return ch.read(ByteBuffer.wrap(b, off, len), pos);
        }
    }

    void closeSource() {
        if (fd != null) {
            try {
                fd.close();
            } catch (IOException e) {
                Log.w(Drivers.TAG, "game data: closing the zip: " + e);
            }
        }
        fd = null;
        archive = null;
        saf = null;
        nodes = null;
    }

    /** Looks at the folder or zip {@code u} (in its folder {@code in}, if not empty) off the main thread. */
    void look(Uri u, boolean isZip, String in, boolean partial) {
        closeSource();
        uri = u;
        zip = isZip;
        inside = in == null ? "" : in;
        partialOk = partial;
        outcome = Outcome.LOOKING;
        final int id = ++lookId;
        post();
        new Thread(() -> {
            Outcome o;
            try {
                o = lookNow();
            } catch (GameData.ZipBad e) {
                Log.w(Drivers.TAG, "game data: the zip cannot be read: " + e.getMessage());
                o = Outcome.BAD_ZIP;
            } catch (SecurityException | IOException e) {
                Log.w(Drivers.TAG, "game data: the choice cannot be read: " + e);
                o = zip ? Outcome.BAD_ZIP : Outcome.LOST;
            }
            if (id == lookId) {
                outcome = o;
                post();
            }
        }).start();
    }

    void cancelLook() {
        lookId++;
        outcome = Outcome.NONE;
        closeSource();
    }

    private Outcome lookNow() throws IOException {
        manifest = DataPages.manifest(app);
        if (manifest == null) {
            throw new FileNotFoundException("the app has no " + GameData.MANIFEST);
        }
        GameData.Lister l;
        GameData.Node top;
        if (zip) {
            fd = app.getContentResolver().openFileDescriptor(uri, "r");
            if (fd == null) {
                throw new FileNotFoundException(uri.toString());
            }
            archive = new GameData.Zip(new Fd(fd));
            l = archive;
            top = archive.root;
        } else {
            saf = new Saf(app.getContentResolver(), uri);
            l = saf;
            top = saf.root();
        }
        if (!inside.isEmpty()) {
            for (GameData.Node k : l.list(top)) {
                if (k.dir && k.name.equals(inside)) {
                    top = k;
                    break;
                }
            }
        }
        long t0 = System.nanoTime();
        found = GameData.find(manifest, l, top, zip);
        if (found.kind == GameData.Found.INSIDE) {
            return Outcome.INSIDE;
        }
        if (found.kind == GameData.Found.ABOVE) {
            above(top);
            return Outcome.ABOVE;
        }
        long t1 = System.nanoTime();
        nodes = GameData.scan(manifest, l, found.root, found.wrapped, WORKERS);
        long t2 = System.nanoTime();
        GameData.clean(root);
        plan = GameData.plan(manifest, nodes, root, DataPages.redo(app));
        Log.i(Drivers.TAG, "game data: looked at " + (zip ? "a zip" : "a folder") + " in "
                + (System.nanoTime() - t0) / 1000000 + " ms (find " + (t1 - t0) / 1000000 + ", list "
                + (t2 - t1) / 1000000 + ", compare " + (System.nanoTime() - t2) / 1000000 + "): " + plan.found + " of " + plan.files + " files there, "
                + plan.present + " already in the game, " + plan.copy.size() + " to copy, " + plan.missing.size()
                + " missing, " + plan.wrong.size() + " of another size");
        if (plan.none()) {
            return Outcome.NOT_FOUND;
        }
        if (plan.partial() && !partialOk) {
            return Outcome.PARTIAL;
        }
        if (plan.copy.isEmpty()) {
            return finish();
        }
        return space();
    }

    /** READY when the copy fits, else SHORT. */
    Outcome space() {
        return plan.shortBy(DataPages.free(root)) > 0 ? Outcome.SHORT : Outcome.READY;
    }

    /** The folder above the picked one, when its provider names documents by path. */
    private void above(GameData.Node picked) {
        aboveName = "";
        aboveUri = null;
        String id = (String) picked.ref;
        int slash = id.lastIndexOf('/');
        int colon = id.indexOf(':');
        if (slash > colon && colon >= 0) {
            String parent = id.substring(0, slash);
            int s2 = parent.lastIndexOf('/');
            aboveName = parent.substring(Math.max(s2, colon) + 1);
            aboveUri = DocumentsContract.buildDocumentUri(uri.getAuthority(), parent);
        }
    }

    // ------------------------------------------------------------------ the copy

    /** Remembers the import so an interrupted one is offered again, and starts the service that copies. */
    void startCopy() {
        GameData.Pending p = new GameData.Pending();
        p.kind = zip ? GameData.Pending.ZIP : GameData.Pending.FOLDER;
        p.uri = uri.toString();
        p.inside = inside;
        p.partial = partialOk;
        p.write(pending);
        stop = false;
        running = true;
        outcome = Outcome.COPYING;
        filesDone = plan.present;
        filesTotal = plan.present + plan.copy.size();
        bytesDone = plan.presentBytes;
        bytesTotal = plan.presentBytes + plan.copyBytes;
        now = "";
        copiedThisRun = 0;
        post();
        DataService.start(app);
    }

    void stop() {
        stop = true;
    }

    /** The copy, on the service's thread. */
    void runCopy() {
        Outcome o;
        long t0 = System.nanoTime();
        try {
            o = copyNow();
        } catch (GameData.Stopped e) {
            o = Outcome.STOPPED;
        } catch (GameData.Mismatch e) {
            bad = e.entry;
            Log.w(Drivers.TAG, "game data: " + e.getMessage());
            o = Outcome.MISMATCH;
        } catch (GameData.WriteFailed e) {
            Log.w(Drivers.TAG, "game data: writing: " + e);
            GameData.Plan left = GameData.plan(manifest, nodes, root, DataPages.redo(app));
            plan = left;
            o = left.shortBy(DataPages.free(root)) > 0 ? Outcome.SHORT : Outcome.WRITE;
        } catch (GameData.ZipBad e) {
            Log.w(Drivers.TAG, "game data: the zip: " + e);
            o = Outcome.BAD_ZIP;
        } catch (SecurityException | IOException e) {
            Log.w(Drivers.TAG, "game data: reading the choice: " + e);
            o = Outcome.LOST;
        }
        Log.i(Drivers.TAG, "game data: copy ended (" + o + ") after " + (System.nanoTime() - t0) / 1000000 + " ms, "
                + copiedThisRun + " files copied");
        DataPages.changed();
        running = false;
        outcome = o;
        post();
    }

    /** Files copied at once: each file costs a round trip through the document provider, so they overlap. */
    static final int WORKERS = 16;

    /** Copies one file of the list from the source into the game. */
    private void copyOne(GameData.Entry e, byte[] buf, GameData.Timing t) throws IOException {
        now = e.shown();
        GameData.Node node = nodes.get(e.name);
        long a = System.nanoTime();
        InputStream in = archive != null ? archive.open((GameData.Zip.Item) node.ref) : saf.open(node);
        t.open += System.nanoTime() - a;
        try {
            GameData.copy(e, in, root, buf, got -> {
                added(got);
                return !stop;
            }, t);
        } finally {
            long b = System.nanoTime();
            in.close();
            t.close += System.nanoTime() - b;
        }
        synchronized (this) {
            filesDone++;
            copiedThisRun++;
        }
        tick();
    }

    private void added(long got) {
        synchronized (this) {
            bytesDone += got;
        }
        tick();
    }

    private Outcome copyNow() throws IOException {
        GameData.clean(root);
        plan = GameData.plan(manifest, nodes, root, DataPages.redo(app));
        filesDone = plan.present;
        filesTotal = plan.present + plan.copy.size();
        bytesDone = plan.presentBytes;
        bytesTotal = plan.presentBytes + plan.copyBytes;
        List<GameData.Entry> order = new ArrayList<>(plan.copy);
        if (archive != null) {
            List<GameData.Zip.Item> items = new ArrayList<>();
            Map<GameData.Zip.Item, GameData.Entry> by = new HashMap<>();
            for (GameData.Entry e : order) {
                GameData.Zip.Item it = (GameData.Zip.Item) nodes.get(e.name).ref;
                items.add(it);
                by.put(it, e);
            }
            GameData.Zip.sortByOffset(items);
            order.clear();
            for (GameData.Zip.Item it : items) {
                order.add(by.get(it));
            }
        }
        final GameData.Timing t = new GameData.Timing();
        final AtomicInteger next = new AtomicInteger();
        final IOException[] failed = new IOException[1];
        final RuntimeException[] crashed = new RuntimeException[1];
        long t0 = System.nanoTime();
        int n = Math.min(WORKERS, Math.max(1, order.size()));
        Thread[] pool = new Thread[n];
        for (int w = 0; w < n; w++) {
            pool[w] = new Thread(() -> {
                byte[] buf = new byte[1 << 20];
                GameData.Timing mine = new GameData.Timing();
                try {
                    for (int i = next.getAndIncrement(); i < order.size(); i = next.getAndIncrement()) {
                        if (stop) {
                            throw new GameData.Stopped();
                        }
                        copyOne(order.get(i), buf, mine);
                    }
                } catch (IOException e) {
                    synchronized (failed) {
                        if (failed[0] == null) {
                            failed[0] = e;
                        }
                    }
                    stop = true;
                } catch (RuntimeException e) {
                    synchronized (failed) {
                        if (crashed[0] == null) {
                            crashed[0] = e;
                        }
                    }
                    stop = true;
                } finally {
                    t.add(mine);
                }
            }, "game data copy " + w);
            pool[w].start();
        }
        for (Thread w : pool) {
            try {
                w.join();
            } catch (InterruptedException e) {
                stop = true;
                Thread.currentThread().interrupt();
            }
        }
        Log.i(Drivers.TAG, "game data: copy timing, " + n + " workers, " + (System.nanoTime() - t0) / 1000000
                + " ms in all (phases summed over workers): " + t);
        if (crashed[0] != null) {
            throw crashed[0];
        }
        if (failed[0] != null && !(failed[0] instanceof GameData.Stopped)) {
            throw failed[0];
        }
        if (failed[0] != null || stop) {
            throw new GameData.Stopped();
        }
        return finish();
    }

    /** After the last file: checks the sizes of everything, records it and ends the import. */
    private Outcome finish() throws IOException {
        GameData.Report r = GameData.check(manifest, root, false, b -> true);
        GameData.Status.of(r, GameData.Status.IMPORT).write(new File(app.getFilesDir(), DataPages.STATUS));
        if (r.bad() == 0) {
            new File(app.getFilesDir(), DataPages.REDO).delete();
        }
        pending.delete();
        closeSource();
        return r.bad() == 0 ? Outcome.DONE : Outcome.PARTIAL_DONE;
    }
}
