package dev.relift.app;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.zip.Inflater;
import java.util.zip.InflaterInputStream;

/**
 * The game data import without Android: the game's list of files (the data
 * manifest relift builds into the app), finding the game data in a folder or
 * zip the player picked, what is left to copy and the space it needs, the
 * copy of one file with its check, the check of the data in the game, and a
 * zip reader over random access. The Android side lists folders and opens
 * files; everything that decides is here, so it runs in the host's tests.
 *
 * The data in the game is the app's external files folder: the manifest name
 * {@code romfs/<path>} is the file {@code <folder>/romfs/<path>}, and the
 * same for {@code dlc/}. A file is copied under a temporary name, hashed as
 * it is written, and renamed into place only when its size and SHA-256 match
 * the list, so a file there with the listed size is a finished one.
 */
final class GameData {
    /** The manifest's asset name, as relift writes it (relift/src/datamanifest.rs). */
    static final String MANIFEST = "data-manifest.txt";
    /** The suffix of a file that is still being copied. */
    static final String PART = ".relift-part";
    /** Room kept free beyond the data itself. */
    static final long MARGIN = 64L << 20;

    private GameData() {
    }

    /** One file of the game's list. */
    static final class Entry {
        final String name;
        final long size;
        final String sha256;

        Entry(String name, long size, String sha256) {
            this.name = name;
            this.size = size;
            this.sha256 = sha256;
        }

        /** The name the player sees: the path inside the game data folder. */
        String shown() {
            return name.startsWith("romfs/") ? name.substring(6) : name;
        }
    }

    /** The game's list of files. */
    static final class Manifest {
        final List<Entry> entries = new ArrayList<>();
        final Map<String, Entry> byName = new HashMap<>();
        /** Every folder an entry is in, as a manifest path ("romfs", "romfs/a", "dlc/x"). */
        final Map<String, Set<String>> kids = new HashMap<>();
        long bytes;

        static Manifest parse(String text) throws IOException {
            Manifest m = new Manifest();
            int no = 0;
            for (String l : text.split("\n")) {
                no++;
                if (l.endsWith("\r")) {
                    l = l.substring(0, l.length() - 1);
                }
                if (l.isEmpty() || l.startsWith("#")) {
                    continue;
                }
                int a = l.indexOf(' ');
                int b = a < 0 ? -1 : l.indexOf(' ', a + 1);
                if (b < 0 || b - a - 1 != 64) {
                    throw new IOException("data manifest line " + no + ": not `size sha256 path`");
                }
                long size;
                try {
                    size = Long.parseLong(l.substring(0, a));
                } catch (NumberFormatException e) {
                    throw new IOException("data manifest line " + no + ": bad size");
                }
                String name = l.substring(b + 1);
                if (!name.startsWith("romfs/") && !name.startsWith("dlc/")) {
                    throw new IOException("data manifest line " + no + ": not under romfs/ or dlc/");
                }
                m.add(new Entry(name, size, l.substring(a + 1, b).toLowerCase(Locale.US)));
            }
            return m;
        }

        void add(Entry e) {
            entries.add(e);
            byName.put(e.name, e);
            bytes += e.size;
            String p = e.name;
            for (int i = p.lastIndexOf('/'); i > 0; i = p.lastIndexOf('/')) {
                String dir = p.substring(0, i);
                kids.computeIfAbsent(dir, d -> new HashSet<>()).add(p.substring(i + 1));
                p = dir;
            }
        }

        boolean hasDlc() {
            return kids.containsKey("dlc");
        }

        Set<String> under(String dir) {
            Set<String> s = kids.get(dir);
            return s == null ? Collections.<String>emptySet() : s;
        }
    }

    /** A file or folder of what the player picked: a document of a folder tree or a zip entry. */
    static final class Node {
        final String name;
        final boolean dir;
        final long size;
        final Object ref;

        Node(String name, boolean dir, long size, Object ref) {
            this.name = name;
            this.dir = dir;
            this.size = size;
            this.ref = ref;
        }
    }

    /** Lists a folder of what the player picked. */
    interface Lister {
        List<Node> list(Node dir) throws IOException;
    }

    /** Where the game data is in what the player picked. */
    static final class Found {
        static final int HERE = 0;
        static final int INSIDE = 1;
        static final int ABOVE = 2;
        static final int NONE = 3;

        final int kind;
        /** The folder that holds the data (HERE, INSIDE), or the picked one (ABOVE, NONE). */
        final Node root;
        /** The root holds romfs/ (and dlc/) instead of being the romfs folder itself. */
        final boolean wrapped;

        Found(int kind, Node root, boolean wrapped) {
            this.kind = kind;
            this.root = root;
            this.wrapped = wrapped;
        }
    }

    private static final int NO = 0;
    private static final int WEAK = 1;
    private static final int STRONG = 2;

    /** How well a folder with these children holds the data, and whether it holds it wrapped. */
    private static int[] looks(Manifest m, List<Node> kids) {
        Set<String> top = m.under("romfs");
        int n = 0;
        for (Node k : kids) {
            if (k.dir && (k.name.equals("romfs") || k.name.equals("dlc") && m.hasDlc())) {
                return new int[] { STRONG, 1 };
            }
            if (top.contains(k.name)) {
                n++;
            }
        }
        if (n == 0) {
            return new int[] { NO, 0 };
        }
        return new int[] { n * 2 >= top.size() ? STRONG : WEAK, 0 };
    }

    /**
     * Finds the game data in the folder {@code picked}: there, in exactly one
     * folder inside it, or (for a folder) one level up, when the picked
     * folder is one of the data's own top folders. A zip's one top folder is
     * taken as found there.
     */
    static Found find(Manifest m, Lister l, Node picked, boolean zip) throws IOException {
        List<Node> kids = l.list(picked);
        int[] here = looks(m, kids);
        if (here[0] == STRONG) {
            return new Found(Found.HERE, picked, here[1] == 1);
        }
        Node match = null;
        boolean wrapped = false;
        int matches = 0;
        for (Node k : kids) {
            if (!k.dir) {
                continue;
            }
            int[] in = looks(m, l.list(k));
            if (in[0] == STRONG) {
                matches++;
                match = k;
                wrapped = in[1] == 1;
            }
        }
        if (matches == 1) {
            return new Found(zip ? Found.HERE : Found.INSIDE, match, wrapped);
        }
        if (!zip && matches == 0 && m.under("romfs").contains(picked.name)) {
            Set<String> want = m.under("romfs/" + picked.name);
            int n = 0;
            for (Node k : kids) {
                if (want.contains(k.name)) {
                    n++;
                }
            }
            if (n > 0 && n * 2 >= want.size()) {
                return new Found(Found.ABOVE, picked, false);
            }
        }
        if (here[0] == WEAK) {
            return new Found(Found.HERE, picked, false);
        }
        return new Found(Found.NONE, picked, false);
    }

    /** The files of the game's list under {@code root}, by manifest name; only the list's folders are entered. */
    static Map<String, Node> scan(Manifest m, Lister l, Node root, boolean wrapped) throws IOException {
        return scan(m, l, root, wrapped, 1);
    }

    /**
     * As scan, listing up to {@code threads} folders at once (the Lister must
     * allow it): each listing of a picked folder is a round trip through
     * Android's document provider.
     */
    static Map<String, Node> scan(Manifest m, Lister l, Node root, boolean wrapped, int threads) throws IOException {
        final Map<String, Node> out = new HashMap<>();
        final java.util.ArrayDeque<Object[]> todo = new java.util.ArrayDeque<>();
        if (!wrapped) {
            todo.add(new Object[] { root, "romfs" });
        } else {
            for (Node k : l.list(root)) {
                if (k.dir && (k.name.equals("romfs") || k.name.equals("dlc"))) {
                    todo.add(new Object[] { k, k.name });
                }
            }
        }
        final int[] busy = { 0 };
        final IOException[] failed = new IOException[1];
        Runnable work = () -> {
            while (true) {
                Object[] job;
                synchronized (todo) {
                    while (todo.isEmpty() && busy[0] > 0 && failed[0] == null) {
                        try {
                            todo.wait();
                        } catch (InterruptedException e) {
                            Thread.currentThread().interrupt();
                            return;
                        }
                    }
                    if (todo.isEmpty() || failed[0] != null) {
                        todo.notifyAll();
                        return;
                    }
                    job = todo.poll();
                    busy[0]++;
                }
                List<Object[]> more = new ArrayList<>();
                Map<String, Node> found = new HashMap<>();
                IOException err = null;
                try {
                    String path = (String) job[1];
                    for (Node k : l.list((Node) job[0])) {
                        String p = path + "/" + k.name;
                        if (k.dir) {
                            if (m.kids.containsKey(p)) {
                                more.add(new Object[] { k, p });
                            }
                        } else if (m.byName.containsKey(p)) {
                            found.put(p, k);
                        }
                    }
                } catch (IOException e) {
                    err = e;
                }
                synchronized (todo) {
                    busy[0]--;
                    out.putAll(found);
                    todo.addAll(more);
                    if (err != null && failed[0] == null) {
                        failed[0] = err;
                    }
                    todo.notifyAll();
                }
            }
        };
        Thread[] pool = new Thread[Math.max(0, threads - 1)];
        for (int i = 0; i < pool.length; i++) {
            pool[i] = new Thread(work, "game data scan " + i);
            pool[i].start();
        }
        work.run();
        for (Thread t : pool) {
            try {
                t.join();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new IOException("interrupted");
            }
        }
        if (failed[0] != null) {
            throw failed[0];
        }
        return out;
    }

    /** What an import of a source would do. */
    static final class Plan {
        /** In the source with the listed size and not in the game yet, in list order. */
        final List<Entry> copy = new ArrayList<>();
        /** Not in the game and not in the source. */
        final List<Entry> missing = new ArrayList<>();
        /** Not in the game and in the source with another size. */
        final List<Entry> wrong = new ArrayList<>();
        /** Files of the list the source has at all. */
        int found;
        /** Files already in the game. */
        int present;
        long copyBytes;
        long presentBytes;
        /** Bytes of the game's files that the copy replaces (they are of the wrong size). */
        long replaced;
        int files;
        long bytes;

        /** The source has none of the game's files. */
        boolean none() {
            return found == 0 && present < files;
        }

        boolean partial() {
            return !missing.isEmpty() || !wrong.isEmpty();
        }

        /** The first file that is missing or of the wrong size, in list order. */
        Entry firstBad() {
            Entry a = missing.isEmpty() ? null : missing.get(0);
            Entry b = wrong.isEmpty() ? null : wrong.get(0);
            if (a == null) {
                return b;
            }
            if (b == null) {
                return a;
            }
            return a.name.compareTo(b.name) <= 0 ? a : b;
        }

        /** Bytes short of what the copy needs with {@code free} bytes free, or 0. */
        long shortBy(long free) {
            long need = copyBytes + MARGIN - replaced;
            return need > free ? need - free : 0;
        }
    }

    /** The size of the game's file {@code name} under {@code root}, or -1. */
    static long sizeIn(File root, String name) {
        File f = new File(root, name);
        return f.isFile() ? f.length() : -1;
    }

    /**
     * Compares the game's list with the source's files and with what the game
     * already has under {@code root}; the files named in {@code redo} (found
     * damaged by a check) count as not there.
     */
    static Plan plan(Manifest m, Map<String, Node> src, File root, Set<String> redo) {
        Plan p = new Plan();
        p.files = m.entries.size();
        p.bytes = m.bytes;
        for (Entry e : m.entries) {
            Node n = src.get(e.name);
            if (n != null) {
                p.found++;
            }
            long have = sizeIn(root, e.name);
            if (have == e.size && !redo.contains(e.name)) {
                p.present++;
                p.presentBytes += e.size;
                continue;
            }
            if (n == null) {
                p.missing.add(e);
            } else if (n.size != e.size) {
                p.wrong.add(e);
            } else {
                p.copy.add(e);
                p.copyBytes += e.size;
                if (have > 0) {
                    p.replaced += have;
                }
            }
        }
        return p;
    }

    /** The game's files that are not finished, from the list alone: what is left to import. */
    static Plan left(Manifest m, File root, Set<String> redo) {
        return plan(m, Collections.<String, Node>emptyMap(), root, redo);
    }

    /** Told about the bytes of a copy or check as they go; false stops it. */
    interface Progress {
        boolean add(long bytes);
    }

    /** Told which file a check is at: its index in the list and its entry. */
    interface Each {
        void at(int index, Entry e);
    }

    /** The copy or check was stopped. */
    static final class Stopped extends IOException {
        private static final long serialVersionUID = 1L;

        Stopped() {
            super("stopped");
        }
    }

    /** A copied file did not match the list; nothing of it was kept. */
    static final class Mismatch extends IOException {
        private static final long serialVersionUID = 1L;

        final transient Entry entry;

        Mismatch(Entry e) {
            super(e.name + ": does not match the game's list");
            this.entry = e;
        }
    }

    /** The source could not be read. */
    static final class ReadFailed extends IOException {
        private static final long serialVersionUID = 1L;

        ReadFailed(IOException cause) {
            super(cause.getMessage(), cause);
        }
    }

    /** The game's storage could not be written. */
    static final class WriteFailed extends IOException {
        private static final long serialVersionUID = 1L;

        WriteFailed(String what) {
            super(what);
        }

        WriteFailed(IOException cause) {
            super(cause.getMessage(), cause);
        }
    }

    static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    static String hex(byte[] d) {
        StringBuilder b = new StringBuilder(d.length * 2);
        for (byte x : d) {
            b.append(Character.forDigit((x >> 4) & 15, 16)).append(Character.forDigit(x & 15, 16));
        }
        return b.toString();
    }

    /**
     * Copies {@code e} from {@code in} into the game under {@code root}: written
     * under a temporary name, hashed as it goes, renamed into place when it
     * matches the list, deleted when it does not or the copy stops.
     */
    static void copy(Entry e, InputStream in, File root, byte[] buf, Progress p) throws IOException {
        copy(e, in, root, buf, p, new Timing());
    }

    /** Where a copy's time goes, in nanoseconds per phase, summed over files. */
    static final class Timing {
        long open;
        long mkdir;
        long create;
        long read;
        long hash;
        long write;
        long close;
        long rename;
        long progress;
        long files;
        long bytes;

        synchronized void add(Timing t) {
            open += t.open;
            mkdir += t.mkdir;
            create += t.create;
            read += t.read;
            hash += t.hash;
            write += t.write;
            close += t.close;
            rename += t.rename;
            progress += t.progress;
            files += t.files;
            bytes += t.bytes;
        }

        @Override
        public synchronized String toString() {
            return String.format(Locale.US, "%d files, %.1f MiB; ms: open %d, mkdir %d, create %d, read %d, hash %d, "
                    + "write %d, close %d, rename %d, progress %d", files, bytes / 1048576.0, open / 1000000,
                    mkdir / 1000000, create / 1000000, read / 1000000, hash / 1000000, write / 1000000,
                    close / 1000000, rename / 1000000, progress / 1000000);
        }
    }

    /** As copy, adding the time of each phase to {@code t}. */
    static void copy(Entry e, InputStream in, File root, byte[] buf, Progress p, Timing t) throws IOException {
        long t0 = System.nanoTime();
        File out = new File(root, e.name);
        File part = new File(out.getPath() + PART);
        File dir = out.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs() && !dir.isDirectory()) {
            throw new WriteFailed("cannot make " + dir);
        }
        long t1 = System.nanoTime();
        t.mkdir += t1 - t0;
        MessageDigest md = sha256();
        long n = 0;
        boolean kept = false;
        try {
            OutputStream o = open(part);
            long t2 = System.nanoTime();
            t.create += t2 - t1;
            try {
                while (true) {
                    int got;
                    long a = System.nanoTime();
                    try {
                        got = in.read(buf);
                    } catch (IOException x) {
                        throw new ReadFailed(x);
                    }
                    long b = System.nanoTime();
                    t.read += b - a;
                    if (got < 0) {
                        break;
                    }
                    md.update(buf, 0, got);
                    long c = System.nanoTime();
                    t.hash += c - b;
                    n += got;
                    if (n > e.size) {
                        throw new Mismatch(e);
                    }
                    try {
                        o.write(buf, 0, got);
                    } catch (IOException x) {
                        throw new WriteFailed(x);
                    }
                    long d = System.nanoTime();
                    t.write += d - c;
                    boolean go = p.add(got);
                    t.progress += System.nanoTime() - d;
                    if (!go) {
                        throw new Stopped();
                    }
                }
            } finally {
                long a = System.nanoTime();
                try {
                    o.close();
                } catch (IOException x) {
                    throw new WriteFailed(x);
                }
                t.close += System.nanoTime() - a;
            }
            if (n != e.size || !hex(md.digest()).equals(e.sha256)) {
                throw new Mismatch(e);
            }
            long a = System.nanoTime();
            if (out.exists() && !out.delete() || !part.renameTo(out)) {
                throw new WriteFailed("cannot rename " + part);
            }
            t.rename += System.nanoTime() - a;
            t.files++;
            t.bytes += n;
            kept = true;
        } finally {
            if (!kept) {
                part.delete();
            }
        }
    }

    private static OutputStream open(File f) throws WriteFailed {
        try {
            return new FileOutputStream(f);
        } catch (IOException x) {
            throw new WriteFailed(x);
        }
    }

    /** Deletes what an interrupted copy left: temporary files under the game's data folders. */
    static void clean(File root) {
        for (String d : new String[] { "romfs", "dlc" }) {
            cleanUnder(new File(root, d));
        }
    }

    private static void cleanUnder(File dir) {
        File[] kids = dir.listFiles();
        if (kids == null) {
            return;
        }
        for (File k : kids) {
            if (k.isDirectory()) {
                cleanUnder(k);
            } else if (k.getName().endsWith(PART)) {
                k.delete();
            }
        }
    }

    /** Deletes the game data: the romfs and dlc folders under {@code root}. */
    static void delete(File root) {
        for (String d : new String[] { "romfs", "dlc" }) {
            deleteTree(new File(root, d));
        }
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) {
            for (File k : kids) {
                deleteTree(k);
            }
        }
        f.delete();
    }

    /** What a check of the game's data found. */
    static final class Report {
        int files;
        long bytes;
        int missing;
        int wrong;
        int damaged;
        String first = "";
        /** The manifest names of the files whose contents differ. */
        final List<String> damagedNames = new ArrayList<>();

        int bad() {
            return missing + wrong + damaged;
        }

        void note(Entry e) {
            if (first.isEmpty()) {
                first = e.shown();
            }
        }
    }

    /**
     * Checks the game's data under {@code root} against the list: every file's
     * size, and with {@code hash} its SHA-256 (each file is read once).
     */
    static Report check(Manifest m, File root, boolean hash, Progress p) throws IOException {
        return check(m, root, hash, p, (i, e) -> {
        });
    }

    static Report check(Manifest m, File root, boolean hash, Progress p, Each each) throws IOException {
        Report r = new Report();
        r.files = m.entries.size();
        r.bytes = m.bytes;
        byte[] buf = new byte[1 << 20];
        int index = 0;
        for (Entry e : m.entries) {
            each.at(index++, e);
            long have = sizeIn(root, e.name);
            if (have < 0) {
                r.missing++;
                r.note(e);
                if (!p.add(e.size)) {
                    throw new Stopped();
                }
                continue;
            }
            if (have != e.size) {
                r.wrong++;
                r.note(e);
                if (!p.add(e.size)) {
                    throw new Stopped();
                }
                continue;
            }
            if (!hash) {
                if (!p.add(e.size)) {
                    throw new Stopped();
                }
                continue;
            }
            MessageDigest md = sha256();
            try (InputStream in = new FileInputStream(new File(root, e.name))) {
                int got;
                while ((got = in.read(buf)) > 0) {
                    md.update(buf, 0, got);
                    if (!p.add(got)) {
                        throw new Stopped();
                    }
                }
            } catch (Stopped s) {
                throw s;
            } catch (IOException x) {
                r.damaged++;
                r.damagedNames.add(e.name);
                r.note(e);
                continue;
            }
            if (!hex(md.digest()).equals(e.sha256)) {
                r.damaged++;
                r.damagedNames.add(e.name);
                r.note(e);
            }
        }
        return r;
    }

    /** Bytes in the decimal units Android shows: "3.9 GB", "212 MB", "2 kB". */
    static String size(long b) {
        if (b >= 1_000_000_000L) {
            return String.format(Locale.US, "%.1f GB", b / 1e9);
        }
        if (b >= 100_000_000L) {
            return String.format(Locale.US, "%d MB", Math.round(b / 1e6));
        }
        if (b >= 1_000_000L) {
            return String.format(Locale.US, "%.1f MB", b / 1e6);
        }
        if (b >= 1000L) {
            return String.format(Locale.US, "%d kB", Math.round(b / 1e3));
        }
        return b + " B";
    }

    /** A count with thousands separators: "41,875". */
    static String count(long n) {
        return String.format(Locale.US, "%,d", n);
    }

    /** Everything a stream holds. */
    static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        byte[] b = new byte[1 << 16];
        int n;
        while ((n = in.read(b)) > 0) {
            o.write(b, 0, n);
        }
        return o.toByteArray();
    }

    /** Reads a small text file, or null. */
    static String readText(File f) {
        try (InputStream in = new FileInputStream(f)) {
            return new String(readAll(in), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return null;
        }
    }

    /** Writes a small text file whole: a temporary file renamed over it. */
    static boolean writeText(File f, String s) {
        File t = new File(f.getPath() + ".new");
        try (OutputStream o = new FileOutputStream(t)) {
            o.write(s.getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            return false;
        }
        return t.renameTo(f);
    }

    /** Lines of {@code key=value}. */
    static Map<String, String> fields(String text) {
        Map<String, String> m = new LinkedHashMap<>();
        if (text == null) {
            return m;
        }
        for (String l : text.split("\n")) {
            int i = l.indexOf('=');
            if (i > 0) {
                m.put(l.substring(0, i), l.substring(i + 1));
            }
        }
        return m;
    }

    static String unfields(Map<String, String> m) {
        StringBuilder b = new StringBuilder();
        for (Map.Entry<String, String> e : m.entrySet()) {
            b.append(e.getKey()).append('=').append(e.getValue().replace('\n', ' ')).append('\n');
        }
        return b.toString();
    }

    /**
     * The last thing known about the data in the game: from the start-up
     * check, a full check, or an import.
     */
    static final class Status {
        static final String START = "start";
        static final String CHECK = "check";
        static final String IMPORT = "import";
        static final String DELETED = "deleted";

        String source = "";
        boolean ok;
        int bad;
        int missing;
        int wrong;
        int damaged;
        String first = "";

        static Status read(File f) {
            Map<String, String> m = fields(readText(f));
            if (m.isEmpty()) {
                return null;
            }
            Status s = new Status();
            s.source = m.getOrDefault("source", "");
            s.ok = "1".equals(m.get("ok"));
            s.bad = num(m.get("bad"));
            s.missing = num(m.get("missing"));
            s.wrong = num(m.get("wrong"));
            s.damaged = num(m.get("damaged"));
            s.first = m.getOrDefault("first", "");
            return s;
        }

        boolean write(File f) {
            Map<String, String> m = new LinkedHashMap<>();
            m.put("source", source);
            m.put("ok", ok ? "1" : "0");
            m.put("bad", Integer.toString(bad));
            m.put("missing", Integer.toString(missing));
            m.put("wrong", Integer.toString(wrong));
            m.put("damaged", Integer.toString(damaged));
            m.put("first", first);
            return writeText(f, unfields(m));
        }

        static Status of(Report r, String source) {
            Status s = new Status();
            s.source = source;
            s.ok = r.bad() == 0;
            s.bad = r.bad();
            s.missing = r.missing;
            s.wrong = r.wrong;
            s.damaged = r.damaged;
            s.first = r.first;
            return s;
        }

        /** What is wrong, in a sentence: "12 files are missing and 3 are the wrong size." */
        String problem() {
            List<String> parts = new ArrayList<>();
            boolean known = missing + wrong + damaged > 0;
            if (!known) {
                parts.add(count(bad) + (bad == 1 ? " file is" : " files are") + " missing or the wrong size");
            }
            if (missing > 0) {
                parts.add(count(missing) + (missing == 1 ? " file is" : " files are") + " missing");
            }
            if (wrong > 0) {
                parts.add((parts.isEmpty() ? count(wrong) + (wrong == 1 ? " file is" : " files are")
                        : count(wrong) + (wrong == 1 ? " is" : " are")) + " the wrong size");
            }
            if (damaged > 0) {
                parts.add((parts.isEmpty() ? count(damaged) + (damaged == 1 ? " file is" : " files are")
                        : count(damaged) + (damaged == 1 ? " is" : " are")) + " damaged");
            }
            StringBuilder b = new StringBuilder();
            for (int i = 0; i < parts.size(); i++) {
                if (i > 0) {
                    b.append(i == parts.size() - 1 ? " and " : ", ");
                }
                b.append(parts.get(i));
            }
            return b.append('.').toString();
        }

        private static int num(String s) {
            try {
                return s == null ? 0 : Integer.parseInt(s.trim());
            } catch (NumberFormatException e) {
                return 0;
            }
        }
    }

    /** An import that has started and not finished: what the player picked. */
    static final class Pending {
        static final String FOLDER = "folder";
        static final String ZIP = "zip";

        String kind = FOLDER;
        String uri = "";
        /** The folder inside the picked one that holds the data, or empty. */
        String inside = "";
        boolean partial;

        static Pending read(File f) {
            Map<String, String> m = fields(readText(f));
            if (!m.containsKey("uri")) {
                return null;
            }
            Pending p = new Pending();
            p.kind = m.getOrDefault("kind", FOLDER);
            p.uri = m.get("uri");
            p.inside = m.getOrDefault("inside", "");
            p.partial = "1".equals(m.get("partial"));
            return p;
        }

        boolean write(File f) {
            Map<String, String> m = new LinkedHashMap<>();
            m.put("kind", kind);
            m.put("uri", uri);
            m.put("inside", inside);
            m.put("partial", partial ? "1" : "0");
            return writeText(f, unfields(m));
        }

        boolean zip() {
            return ZIP.equals(kind);
        }
    }

    /** Positioned reads of a file the player picked. */
    interface Bytes {
        long size() throws IOException;

        int read(long pos, byte[] b, int off, int len) throws IOException;
    }

    /** The zip is not one this reader can unpack: damaged, cut short, or encrypted. */
    static final class ZipBad extends IOException {
        private static final long serialVersionUID = 1L;

        ZipBad(String why) {
            super(why);
        }
    }

    /**
     * A zip read through its central directory (zip64 too), entries stored or
     * deflated, opened by position without reading the rest of the file.
     */
    static final class Zip implements Lister {
        static final class Item {
            String name;
            int method;
            int flags;
            long csize;
            long size;
            long local;
        }

        /** A folder of the zip: the entries under it by name. */
        static final class Dir {
            final Map<String, Node> kids = new TreeMap<>();
        }

        final Bytes in;
        final List<Item> items = new ArrayList<>();
        final Node root = new Node("", true, 0, new Dir());

        Zip(Bytes in) throws IOException {
            this.in = in;
            readDirectory();
            for (Item it : items) {
                place(it);
            }
        }

        private void full(long pos, byte[] b, int n) throws IOException {
            int off = 0;
            while (off < n) {
                int got = in.read(pos + off, b, off, n - off);
                if (got <= 0) {
                    throw new ZipBad("cut short");
                }
                off += got;
            }
        }

        private static int u16(byte[] b, int at) {
            return (b[at] & 0xff) | (b[at + 1] & 0xff) << 8;
        }

        private static long u32(byte[] b, int at) {
            return (u16(b, at) | (long) u16(b, at + 2) << 16) & 0xffffffffL;
        }

        private static long u64(byte[] b, int at) {
            return u32(b, at) | u32(b, at + 4) << 32;
        }

        private void readDirectory() throws IOException {
            long size = in.size();
            int tail = (int) Math.min(size, 65557);
            if (tail < 22) {
                throw new ZipBad("too short for a zip");
            }
            byte[] t = new byte[tail];
            full(size - tail, t, tail);
            int eocd = -1;
            for (int i = tail - 22; i >= 0; i--) {
                if (u32(t, i) == 0x06054b50L) {
                    eocd = i;
                    break;
                }
            }
            if (eocd < 0) {
                throw new ZipBad("no end of central directory");
            }
            long count = u16(t, eocd + 10);
            long cdSize = u32(t, eocd + 12);
            long cdOff = u32(t, eocd + 16);
            if (eocd >= 20 && u32(t, eocd - 20) == 0x07064b50L) {
                long z64 = u64(t, eocd - 20 + 8);
                byte[] r = new byte[56];
                full(z64, r, 56);
                if (u32(r, 0) != 0x06064b50L) {
                    throw new ZipBad("bad zip64 end of central directory");
                }
                count = u64(r, 32);
                cdSize = u64(r, 40);
                cdOff = u64(r, 48);
            }
            if (cdOff < 0 || cdSize < 0 || cdOff + cdSize > size || cdSize > Integer.MAX_VALUE) {
                throw new ZipBad("central directory out of the file");
            }
            byte[] cd = new byte[(int) cdSize];
            full(cdOff, cd, cd.length);
            int at = 0;
            for (long i = 0; i < count; i++) {
                if (at + 46 > cd.length || u32(cd, at) != 0x02014b50L) {
                    throw new ZipBad("bad central directory entry");
                }
                Item it = new Item();
                it.flags = u16(cd, at + 8);
                it.method = u16(cd, at + 10);
                it.csize = u32(cd, at + 20);
                it.size = u32(cd, at + 24);
                int nameLen = u16(cd, at + 28);
                int extraLen = u16(cd, at + 30);
                int commentLen = u16(cd, at + 32);
                it.local = u32(cd, at + 42);
                if (at + 46 + nameLen + extraLen > cd.length) {
                    throw new ZipBad("bad central directory entry");
                }
                it.name = new String(cd, at + 46, nameLen, StandardCharsets.UTF_8);
                int x = at + 46 + nameLen;
                int xe = x + extraLen;
                while (x + 4 <= xe) {
                    int id = u16(cd, x);
                    int len = u16(cd, x + 2);
                    int d = x + 4;
                    if (id == 1) {
                        if (it.size == 0xffffffffL && d + 8 <= xe) {
                            it.size = u64(cd, d);
                            d += 8;
                        }
                        if (it.csize == 0xffffffffL && d + 8 <= xe) {
                            it.csize = u64(cd, d);
                            d += 8;
                        }
                        if (it.local == 0xffffffffL && d + 8 <= xe) {
                            it.local = u64(cd, d);
                        }
                    }
                    x += 4 + len;
                }
                items.add(it);
                at += 46 + nameLen + extraLen + commentLen;
            }
        }

        private void place(Item it) {
            String[] parts = it.name.replace('\\', '/').split("/");
            boolean dir = it.name.endsWith("/");
            Node at = root;
            for (int i = 0; i < parts.length; i++) {
                String p = parts[i];
                if (p.isEmpty() || p.equals(".") || p.equals("..")) {
                    continue;
                }
                Map<String, Node> kids = ((Dir) at.ref).kids;
                boolean last = i == parts.length - 1;
                if (last && !dir) {
                    kids.put(p, new Node(p, false, it.size, it));
                    return;
                }
                Node next = kids.get(p);
                if (next == null || !next.dir) {
                    next = new Node(p, true, 0, new Dir());
                    kids.put(p, next);
                }
                at = next;
            }
        }

        @Override
        public List<Node> list(Node dir) {
            return new ArrayList<>(((Dir) dir.ref).kids.values());
        }

        /** The contents of a file entry. */
        InputStream open(Item it) throws IOException {
            if ((it.flags & 1) != 0) {
                throw new ZipBad(it.name + ": encrypted");
            }
            if (it.method != 0 && it.method != 8) {
                throw new ZipBad(it.name + ": compression method " + it.method);
            }
            byte[] h = new byte[30];
            full(it.local, h, 30);
            if (u32(h, 0) != 0x04034b50L) {
                throw new ZipBad(it.name + ": bad local header");
            }
            long data = it.local + 30 + u16(h, 26) + u16(h, 28);
            InputStream raw = new Range(in, data, it.csize);
            if (it.method == 0) {
                return raw;
            }
            final Inflater inf = new Inflater(true);
            return new InflaterInputStream(raw, inf, 1 << 16) {
                private boolean closed;

                @Override
                public void close() throws IOException {
                    if (!closed) {
                        closed = true;
                        inf.end();
                    }
                    super.close();
                }
            };
        }

        /** The entry offset order: reading in it goes through the file once. */
        static void sortByOffset(List<Item> l) {
            Collections.sort(l, (a, b) -> Long.compare(a.local, b.local));
        }
    }

    /** A byte range of a {@link Bytes}, read in order. */
    static final class Range extends InputStream {
        private final Bytes in;
        private long pos;
        private long left;

        Range(Bytes in, long pos, long len) {
            this.in = in;
            this.pos = pos;
            this.left = len;
        }

        @Override
        public int read() throws IOException {
            byte[] b = new byte[1];
            return read(b, 0, 1) == 1 ? b[0] & 0xff : -1;
        }

        @Override
        public int read(byte[] b, int off, int len) throws IOException {
            if (left <= 0) {
                return -1;
            }
            int n = in.read(pos, b, off, (int) Math.min(len, left));
            if (n <= 0) {
                throw new ZipBad("cut short");
            }
            pos += n;
            left -= n;
            return n;
        }
    }
}
