package dev.relift.app;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * Tests of the game data import's logic (GameData), run on the host with
 * plain javac and java by scripts/ci/checks.sh: finding the data one level
 * in and up, partial data, a copy stopped and resumed, a zip with a top
 * folder (zip64 too), the space check and a file that does not match.
 */
public final class GameDataTest {
    private static final java.util.Set<String> NONE = java.util.Collections.emptySet();
    private static int failed;
    private static int passed;
    private static File scratch;

    public static void main(String[] args) throws Exception {
        scratch = new File(System.getProperty("java.io.tmpdir"), "relift-gamedata-test-" + ProcessHandle.current().pid());
        rm(scratch);
        scratch.mkdirs();
        try {
            run("finds the data in the picked folder", GameDataTest::findsHere);
            run("offers the one folder inside", GameDataTest::offersInside);
            run("finds a folder that holds romfs and dlc", GameDataTest::findsWrapped);
            run("offers the folder above a top folder of the data", GameDataTest::offersAbove);
            run("two folders inside that both hold data are not guessed", GameDataTest::ambiguousIsNone);
            run("a folder without the data names the first file", GameDataTest::wrongFolder);
            run("partial data is counted and copies what is there", GameDataTest::partialData);
            run("a stopped copy keeps finished files and resumes", GameDataTest::stopAndResume);
            run("a zip with one top folder unpacks into the game", GameDataTest::zipTopFolder);
            run("a zip64 with many entries is read", GameDataTest::zip64);
            run("a damaged zip is refused", GameDataTest::badZip);
            run("the space check counts what is left and the margin", GameDataTest::space);
            run("a file that differs is not kept", GameDataTest::mismatch);
            run("the check names missing, short and damaged files", GameDataTest::check);
            run("sizes and counts read like Android", GameDataTest::formats);
            run("status and pending imports round-trip", GameDataTest::records);
        } finally {
            rm(scratch);
        }
        System.out.println(passed + " passed, " + failed + " failed");
        if (failed > 0) {
            System.exit(1);
        }
    }

    interface Test {
        void run() throws Exception;
    }

    private static void run(String name, Test t) {
        try {
            t.run();
            passed++;
            System.out.println("ok   " + name);
        } catch (Throwable e) {
            failed++;
            System.out.println("FAIL " + name + ": " + e);
            e.printStackTrace(System.out);
        }
    }

    private static void check(boolean c, String what) {
        if (!c) {
            throw new AssertionError(what);
        }
    }

    private static void eq(Object a, Object b, String what) {
        if (a == null ? b != null : !a.equals(b)) {
            throw new AssertionError(what + ": " + a + " != " + b);
        }
    }

    private static void rm(File f) {
        File[] k = f.listFiles();
        if (k != null) {
            for (File x : k) {
                rm(x);
            }
        }
        f.delete();
    }

    private static File dir(String name) {
        File d = new File(scratch, name);
        rm(d);
        d.mkdirs();
        return d;
    }

    private static void put(File root, String rel, byte[] b) throws IOException {
        File f = new File(root, rel);
        f.getParentFile().mkdirs();
        try (FileOutputStream o = new FileOutputStream(f)) {
            o.write(b);
        }
    }

    private static byte[] bytes(String s) {
        return s.getBytes(StandardCharsets.UTF_8);
    }

    private static String sha(byte[] b) {
        return GameData.hex(GameData.sha256().digest(b));
    }

    /** The game's files: romfs/<path> to contents. */
    private static final String[][] GAME = {
        { "as/a.bin", "alpha" },
        { "as/sub/b.bin", "bravo bravo" },
        { "battle/c.bin", "charlie" },
        { "common/d.bin", "delta delta delta" },
        { "font/e.bin", "" },
        { "top.bin", "top level file" },
    };

    private static GameData.Manifest manifest(boolean dlc) throws IOException {
        StringBuilder t = new StringBuilder("# relift game data manifest: size sha256 path\n");
        List<String> lines = new ArrayList<>();
        for (String[] f : GAME) {
            byte[] b = bytes(f[1]);
            lines.add(b.length + " " + sha(b) + " romfs/" + f[0]);
        }
        if (dlc) {
            byte[] b = bytes("extra");
            lines.add(b.length + " " + sha(b) + " dlc/0100/x.bin");
        }
        lines.sort((a, b) -> a.substring(a.lastIndexOf(' ') + 1).compareTo(b.substring(b.lastIndexOf(' ') + 1)));
        for (String l : lines) {
            t.append(l).append('\n');
        }
        return GameData.Manifest.parse(t.toString());
    }

    /** Writes the game's romfs files under {@code root}. */
    private static void game(File root) throws IOException {
        for (String[] f : GAME) {
            put(root, f[0], bytes(f[1]));
        }
    }

    /** A folder tree on the host as the Android side lists one. */
    static final class Files implements GameData.Lister {
        int lists;

        static GameData.Node node(File f) {
            return new GameData.Node(f.getName(), f.isDirectory(), f.isFile() ? f.length() : 0, f);
        }

        @Override
        public List<GameData.Node> list(GameData.Node dir) {
            lists++;
            List<GameData.Node> out = new ArrayList<>();
            File[] k = ((File) dir.ref).listFiles();
            if (k != null) {
                Arrays.sort(k);
                for (File f : k) {
                    out.add(node(f));
                }
            }
            return out;
        }
    }

    private static InputStream source(GameData.Node n) throws IOException {
        return new FileInputStream((File) n.ref);
    }

    private static GameData.Progress forever() {
        return b -> true;
    }

    private static void findsHere() throws Exception {
        GameData.Manifest m = manifest(false);
        File romfs = dir("here/romfs");
        game(romfs);
        Files l = new Files();
        GameData.Found f = GameData.find(m, l, Files.node(romfs), false);
        eq(f.kind, GameData.Found.HERE, "kind");
        check(!f.wrapped, "the romfs folder itself is not wrapped");
        Map<String, GameData.Node> src = GameData.scan(m, l, f.root, f.wrapped);
        eq(src.size(), GAME.length, "every file found");
    }

    private static void offersInside() throws Exception {
        GameData.Manifest m = manifest(false);
        File download = dir("inside/Download");
        game(new File(download, "Game data"));
        put(download, "Pictures/p.jpg", bytes("x"));
        put(download, "notes.txt", bytes("x"));
        GameData.Found f = GameData.find(m, new Files(), Files.node(download), false);
        eq(f.kind, GameData.Found.INSIDE, "kind");
        eq(f.root.name, "Game data", "the folder inside");
    }

    private static void findsWrapped() throws Exception {
        GameData.Manifest m = manifest(true);
        File d = dir("wrapped/export");
        game(new File(d, "romfs"));
        put(d, "dlc/0100/x.bin", bytes("extra"));
        Files l = new Files();
        GameData.Found f = GameData.find(m, l, Files.node(d), false);
        eq(f.kind, GameData.Found.HERE, "kind");
        check(f.wrapped, "wrapped");
        Map<String, GameData.Node> src = GameData.scan(m, l, f.root, true);
        eq(src.size(), GAME.length + 1, "romfs and dlc files");
        eq(GameData.scan(m, l, f.root, true, 4).keySet(), src.keySet(), "the same files listed by 4 threads");
        check(src.containsKey("dlc/0100/x.bin"), "the dlc file");
        GameData.Found up = GameData.find(m, l, Files.node(dir("wrapped/parent")), false);
        eq(up.kind, GameData.Found.NONE, "an empty folder");
        File parent = new File(scratch, "wrapped");
        GameData.Found in = GameData.find(m, l, Files.node(parent), false);
        eq(in.kind, GameData.Found.INSIDE, "the folder that holds romfs, one level in");
        eq(in.root.name, "export", "named");
        check(in.wrapped, "and wrapped");
    }

    private static void offersAbove() throws Exception {
        GameData.Manifest m = manifest(false);
        File romfs = dir("above/romfs");
        game(romfs);
        GameData.Found f = GameData.find(m, new Files(), Files.node(new File(romfs, "as")), false);
        eq(f.kind, GameData.Found.ABOVE, "kind");
        eq(f.root.name, "as", "the picked folder");
        GameData.Found z = GameData.find(m, new Files(), Files.node(new File(romfs, "as")), true);
        check(z.kind != GameData.Found.ABOVE, "a zip has no folder above");
    }

    private static void ambiguousIsNone() throws Exception {
        GameData.Manifest m = manifest(false);
        File d = dir("two");
        game(new File(d, "copy 1"));
        game(new File(d, "copy 2"));
        eq(GameData.find(m, new Files(), Files.node(d), false).kind, GameData.Found.NONE, "kind");
    }

    private static void wrongFolder() throws Exception {
        GameData.Manifest m = manifest(false);
        File d = dir("wrong/Photos");
        put(d, "holiday/1.jpg", bytes("jpg"));
        Files l = new Files();
        GameData.Found f = GameData.find(m, l, Files.node(d), false);
        eq(f.kind, GameData.Found.NONE, "kind");
        GameData.Plan p = GameData.plan(m, GameData.scan(m, l, f.root, false), dir("wrong/game"), NONE);
        check(p.none(), "none of the files");
        eq(p.firstBad().shown(), "as/a.bin", "the first file named without romfs/");
    }

    private static void partialData() throws Exception {
        GameData.Manifest m = manifest(false);
        File src = dir("partial/src");
        game(src);
        new File(src, "battle/c.bin").delete();
        new File(src, "font/e.bin").delete();
        put(src, "common/d.bin", bytes("short"));
        Files l = new Files();
        GameData.Found f = GameData.find(m, l, Files.node(src), false);
        eq(f.kind, GameData.Found.HERE, "most top folders are there");
        GameData.Plan p = GameData.plan(m, GameData.scan(m, l, f.root, false), dir("partial/game"), NONE);
        check(p.partial(), "partial");
        check(!p.none(), "not none");
        eq(p.missing.size(), 2, "missing");
        eq(p.wrong.size(), 1, "wrong size");
        eq(p.copy.size(), 3, "copied anyway when asked");
        eq(p.firstBad().name, "romfs/battle/c.bin", "the first bad file in list order");
        File game = new File(scratch, "partial/game");
        Map<String, GameData.Node> nodes = GameData.scan(m, l, f.root, false);
        for (GameData.Entry e : p.copy) {
            try (InputStream in = source(nodes.get(e.name))) {
                GameData.copy(e, in, game, new byte[7], forever());
            }
        }
        GameData.Report r = GameData.check(m, game, true, forever());
        eq(r.missing, 3, "the check sees what is still missing");
        GameData.Plan left = GameData.left(m, game, NONE);
        eq(left.present, 3, "three files are in the game");
    }

    private static void stopAndResume() throws Exception {
        GameData.Manifest m = manifest(false);
        File src = dir("resume/src");
        game(src);
        File game = dir("resume/game");
        Files l = new Files();
        Map<String, GameData.Node> nodes = GameData.scan(m, l, Files.node(src), false);
        GameData.Plan p = GameData.plan(m, nodes, game, NONE);
        eq(p.copy.size(), GAME.length, "everything to copy");
        final long[] budget = { 12 };
        int done = 0;
        boolean stopped = false;
        for (GameData.Entry e : p.copy) {
            try (InputStream in = source(nodes.get(e.name))) {
                GameData.copy(e, in, game, new byte[4], b -> (budget[0] -= b) > 0);
                done++;
            } catch (GameData.Stopped s) {
                stopped = true;
                break;
            }
        }
        check(stopped, "the copy stopped");
        eq(done, 1, "one file finished before the stop");
        put(game, "romfs/as/sub/b.bin" + GameData.PART, bytes("half"));
        GameData.clean(game);
        check(!new File(game, "romfs/as/sub/b.bin" + GameData.PART).exists(), "the half file is dropped");
        GameData.Plan again = GameData.plan(m, nodes, game, NONE);
        eq(again.present, 1, "the finished file stays");
        eq(again.copy.size(), GAME.length - 1, "only the rest is copied");
        eq(again.copyBytes, m.bytes - 5, "and only its bytes are needed");
        for (GameData.Entry e : again.copy) {
            try (InputStream in = source(nodes.get(e.name))) {
                GameData.copy(e, in, game, new byte[3], forever());
            }
        }
        eq(GameData.check(m, game, true, forever()).bad(), 0, "complete after the resume");
        eq(GameData.left(m, game, NONE).present, GAME.length, "nothing left");
    }

    /** A host file as positioned reads, as the Android side reads a picked file's descriptor. */
    static final class Disk implements GameData.Bytes {
        final RandomAccessFile f;

        Disk(File file) throws IOException {
            f = new RandomAccessFile(file, "r");
        }

        @Override
        public long size() throws IOException {
            return f.length();
        }

        @Override
        public int read(long pos, byte[] b, int off, int len) throws IOException {
            f.seek(pos);
            return f.read(b, off, len);
        }
    }

    private static void zipTopFolder() throws Exception {
        GameData.Manifest m = manifest(false);
        File z = new File(dir("zip"), "game-data.zip");
        try (ZipOutputStream o = new ZipOutputStream(new FileOutputStream(z))) {
            o.putNextEntry(new ZipEntry("Game data/"));
            o.closeEntry();
            boolean stored = false;
            for (String[] f : GAME) {
                byte[] b = bytes(f[1]);
                ZipEntry e = new ZipEntry("Game data/" + f[0]);
                if (stored) {
                    e.setMethod(ZipEntry.STORED);
                    e.setSize(b.length);
                    CRC32 c = new CRC32();
                    c.update(b);
                    e.setCrc(c.getValue());
                }
                stored = !stored;
                o.putNextEntry(e);
                o.write(b);
                o.closeEntry();
            }
        }
        GameData.Zip zip = new GameData.Zip(new Disk(z));
        GameData.Found f = GameData.find(m, zip, zip.root, true);
        eq(f.kind, GameData.Found.HERE, "a zip's one top folder is used without asking");
        eq(f.root.name, "Game data", "the top folder");
        Map<String, GameData.Node> nodes = GameData.scan(m, zip, f.root, f.wrapped);
        GameData.Plan p = GameData.plan(m, nodes, dir("zip/game"), NONE);
        eq(p.copy.size(), GAME.length, "every file to copy");
        List<GameData.Zip.Item> order = new ArrayList<>();
        Map<GameData.Zip.Item, GameData.Entry> by = new java.util.HashMap<>();
        for (GameData.Entry e : p.copy) {
            GameData.Zip.Item it = (GameData.Zip.Item) nodes.get(e.name).ref;
            order.add(it);
            by.put(it, e);
        }
        GameData.Zip.sortByOffset(order);
        File game = new File(scratch, "zip/game");
        for (GameData.Zip.Item it : order) {
            try (InputStream in = zip.open(it)) {
                GameData.copy(by.get(it), in, game, new byte[1 << 16], forever());
            }
        }
        eq(GameData.check(m, game, true, forever()).bad(), 0, "the unpacked data matches");
    }

    private static void zip64() throws Exception {
        File z = new File(dir("zip64"), "many.zip");
        int n = 70000;
        try (ZipOutputStream o = new ZipOutputStream(new FileOutputStream(z))) {
            o.setLevel(0);
            for (int i = 0; i < n; i++) {
                o.putNextEntry(new ZipEntry("d" + (i % 100) + "/f" + i));
                o.write(bytes("n" + i));
                o.closeEntry();
            }
        }
        GameData.Zip zip = new GameData.Zip(new Disk(z));
        eq(zip.items.size(), n, "every entry");
        GameData.Zip.Item last = zip.items.get(n - 1);
        eq(last.name, "d99/f69999", "the last name");
        try (InputStream in = zip.open(last)) {
            eq(new String(in.readAllBytes(), StandardCharsets.UTF_8), "n69999", "its contents");
        }
        eq(zip.list(zip.root).size(), 100, "the folders");
    }

    private static void badZip() throws Exception {
        File z = new File(dir("bad"), "cut.zip");
        File good = new File(dir("bad/good"), "g.zip");
        try (ZipOutputStream o = new ZipOutputStream(new FileOutputStream(good))) {
            o.putNextEntry(new ZipEntry("a"));
            o.write(bytes("aaaa"));
            o.closeEntry();
        }
        byte[] all = java.nio.file.Files.readAllBytes(good.toPath());
        try (FileOutputStream o = new FileOutputStream(z)) {
            o.write(all, 0, all.length - 30);
        }
        boolean refused = false;
        try {
            new GameData.Zip(new Disk(z));
        } catch (GameData.ZipBad e) {
            refused = true;
        }
        check(refused, "a cut zip is refused");
        refused = false;
        put(z.getParentFile(), "text.zip", bytes("not a zip at all, just some words"));
        try {
            new GameData.Zip(new Disk(new File(z.getParentFile(), "text.zip")));
        } catch (GameData.ZipBad e) {
            refused = true;
        }
        check(refused, "a text file is refused");
    }

    private static void space() throws Exception {
        GameData.Manifest m = manifest(false);
        File src = dir("space/src");
        game(src);
        File game = dir("space/game");
        put(game, "romfs/as/a.bin", bytes("alpha"));
        put(game, "romfs/common/d.bin", bytes("old"));
        GameData.Plan p = GameData.plan(m, GameData.scan(m, new Files(), Files.node(src), false), game, NONE);
        long need = m.bytes - 5;
        eq(p.copyBytes, need, "the bytes left");
        eq(p.replaced, 3L, "the wrong file in the game is replaced");
        eq(p.shortBy(need + GameData.MARGIN), 0L, "enough with the margin");
        eq(p.shortBy(need), GameData.MARGIN - 3, "short by the margin less what is replaced");
        eq(p.shortBy(0), need + GameData.MARGIN - 3, "short by everything");
    }

    private static void mismatch() throws Exception {
        GameData.Manifest m = manifest(false);
        File game = dir("mismatch/game");
        GameData.Entry e = m.byName.get("romfs/as/a.bin");
        boolean thrown = false;
        try {
            GameData.copy(e, new ByteArrayInputStream(bytes("ALPHA")), game, new byte[16], forever());
        } catch (GameData.Mismatch x) {
            thrown = true;
        }
        check(thrown, "same size, other bytes");
        check(!new File(game, "romfs/as/a.bin").exists(), "not kept");
        check(!new File(game, "romfs/as/a.bin" + GameData.PART).exists(), "no half file");
        thrown = false;
        try {
            GameData.copy(e, new ByteArrayInputStream(bytes("alpha and more")), game, new byte[4], forever());
        } catch (GameData.Mismatch x) {
            thrown = true;
        }
        check(thrown, "longer than listed");
        thrown = false;
        InputStream broken = new InputStream() {
            @Override
            public int read() throws IOException {
                throw new IOException("gone");
            }
        };
        try {
            GameData.copy(e, broken, game, new byte[4], forever());
        } catch (GameData.ReadFailed x) {
            thrown = true;
        }
        check(thrown, "a read error is the source's");
        GameData.copy(e, new ByteArrayInputStream(bytes("alpha")), game, new byte[2], forever());
        check(new File(game, "romfs/as/a.bin").isFile(), "the right bytes are kept");
    }

    private static void check() throws Exception {
        GameData.Manifest m = manifest(false);
        File game = dir("check/game");
        game(new File(game, "romfs"));
        new File(game, "romfs/battle/c.bin").delete();
        put(game, "romfs/common/d.bin", bytes("short"));
        put(game, "romfs/top.bin", bytes("TOP level file"));
        GameData.Report sizes = GameData.check(m, game, false, forever());
        eq(sizes.missing, 1, "missing");
        eq(sizes.wrong, 1, "wrong size");
        eq(sizes.damaged, 0, "sizes only");
        eq(sizes.first, "battle/c.bin", "the first bad file");
        GameData.Report full = GameData.check(m, game, true, forever());
        eq(full.damaged, 1, "damaged with hashing");
        GameData.Status s = GameData.Status.of(full, GameData.Status.CHECK);
        eq(s.problem(), "1 file is missing, 1 is the wrong size and 1 is damaged.", "in words");
        boolean stopped = false;
        try {
            GameData.check(m, game, true, b -> false);
        } catch (GameData.Stopped x) {
            stopped = true;
        }
        check(stopped, "a check can stop");
    }

    private static void formats() {
        eq(GameData.size(3925607923L), "3.9 GB", "GB");
        eq(GameData.size(212_000_000L), "212 MB", "MB");
        eq(GameData.size(1_500_000L), "1.5 MB", "small MB");
        eq(GameData.size(2048), "2 kB", "kB");
        eq(GameData.count(41875), "41,875", "count");
        GameData.Status s = new GameData.Status();
        s.bad = 3;
        eq(s.problem(), "3 files are missing or the wrong size.", "only a total");
        s.missing = 12;
        s.wrong = 3;
        eq(s.problem(), "12 files are missing and 3 are the wrong size.", "the mockup's sentence");
    }

    private static void records() throws Exception {
        File d = dir("records");
        GameData.Pending p = new GameData.Pending();
        p.kind = GameData.Pending.ZIP;
        p.uri = "content://example/doc/1";
        p.inside = "Game data";
        p.partial = true;
        check(p.write(new File(d, "pending")), "written");
        GameData.Pending q = GameData.Pending.read(new File(d, "pending"));
        check(q != null && q.zip() && q.partial, "read back");
        eq(q.uri, p.uri, "uri");
        eq(q.inside, "Game data", "inside");
        check(GameData.Pending.read(new File(d, "none")) == null, "no file, no import");
        GameData.Status s = new GameData.Status();
        s.source = GameData.Status.START;
        s.ok = true;
        check(s.write(new File(d, "status")), "status written");
        GameData.Status t = GameData.Status.read(new File(d, "status"));
        check(t != null && t.ok && t.source.equals(GameData.Status.START), "status read back");
    }
}
