package dev.relift.app;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.util.Log;
import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import org.json.JSONObject;

/**
 * The GPU driver packages of this game: the ones added (each a folder under
 * the app's private gpu_drivers folder with its meta.json and library, as the
 * runtime reads them), the game's choice, and the checks a new package goes
 * through before it is added. Everything here belongs to this one game.
 */
final class Drivers {
    static final String SYSTEM = "system";
    static final String TAG = "relift";

    /** One added package, with what its meta.json says. */
    static final class Pkg {
        String dir;
        String name;
        String version;
        String author;
        String driverVersion;
        String library;
        int minApi;
        long added;
        String sha256;

        String detail() {
            StringBuilder b = new StringBuilder();
            if (!version.isEmpty()) {
                b.append("Version ").append(version);
            }
            if (!author.isEmpty()) {
                b.append(b.length() > 0 ? " \u00b7 by " : "By ").append(author);
            }
            return b.toString();
        }

        String addedOn() {
            return "Added " + new SimpleDateFormat("d MMM", Locale.US).format(new Date(added));
        }
    }

    /** A package checked and unpacked into a staging folder, waiting for its test load. */
    static final class Staged {
        File folder;
        Pkg pkg;
    }

    /** Why a file could not be added: the title and body of the message. */
    static final class AddError extends Exception {
        final String title;
        final String body;

        AddError(String title, String body) {
            super(title + ": " + body);
            this.title = title;
            this.body = body;
        }
    }

    /** The phone's own driver as its Vulkan driver reports it. */
    static final class SystemDriver {
        String gpu = "";
        String name = "";
        String version = "";
        boolean usable;
        String reason = "";

        String line() {
            return version.isEmpty() ? name : name + ", version " + version;
        }
    }

    final Context ctx;
    final File root;
    final SharedPreferences prefs;
    final List<Catalog.Entry> catalog;

    Drivers(Context ctx) {
        this.ctx = ctx;
        this.root = new File(ctx.getFilesDir(), "gpu_drivers");
        this.prefs = ctx.getSharedPreferences("gpu_drivers", Context.MODE_PRIVATE);
        this.catalog = Catalog.load(ctx);
    }

    /** A package that is a file of the list shows the list's name, version and author. */
    private void label(Pkg p) {
        for (Catalog.Entry e : catalog) {
            if (e.sha256.equalsIgnoreCase(p.sha256)) {
                p.name = e.name;
                p.version = e.version;
                p.author = e.author;
            }
        }
    }

    /** The added packages, the newest first. */
    List<Pkg> list() {
        ArrayList<Pkg> out = new ArrayList<>();
        File[] kids = root.listFiles();
        if (kids != null) {
            for (File k : kids) {
                Pkg p = k.getName().startsWith(".") ? null : read(k);
                if (p != null) {
                    out.add(p);
                }
            }
        }
        Collections.sort(out, (a, b) -> a.added != b.added ? Long.compare(b.added, a.added) : a.dir.compareTo(b.dir));
        return out;
    }

    Pkg find(String dir) {
        if (dir == null || dir.equals(SYSTEM) || dir.startsWith(".") || dir.contains("/")) {
            return null;
        }
        return read(new File(root, dir));
    }

    private Pkg read(File folder) {
        try {
            Pkg p = meta(folder);
            if (!new File(folder, p.library).isFile()) {
                return null;
            }
            p.dir = folder.getName();
            p.added = prefs.getLong("added." + p.dir, folder.lastModified());
            p.sha256 = prefs.getString("sha256." + p.dir, "");
            label(p);
            return p;
        } catch (Exception e) {
            return null;
        }
    }

    private static Pkg meta(File folder) throws Exception {
        JSONObject m = new JSONObject(new String(Files.readAllBytes(new File(folder, "meta.json").toPath()), "UTF-8"));
        Pkg p = new Pkg();
        p.name = m.optString("name", "").trim();
        p.version = m.optString("packageVersion", "").trim();
        p.author = m.optString("author", "").trim();
        p.driverVersion = m.optString("driverVersion", "").trim();
        p.minApi = m.optInt("minApi", 0);
        p.library = m.getString("libraryName");
        if (p.name.isEmpty()) {
            p.name = folder.getName();
        }
        if (p.library.isEmpty() || p.library.contains("/") || p.library.startsWith(".")) {
            throw new IOException("meta.json names no library of the package");
        }
        return p;
    }

    /** This game's choice: SYSTEM or the folder name of an added package. */
    String choice() {
        return prefs.getString("choice", SYSTEM);
    }

    void setChoice(String dir) {
        prefs.edit().putString("choice", dir == null ? SYSTEM : dir).commit();
    }

    /** The name a choice shows: the phone's own driver or the package's name. */
    String choiceName(String dir) {
        Pkg p = find(dir);
        return p == null ? "Phone's own driver" : p.name;
    }

    void remove(Pkg p) {
        delete(new File(root, p.dir));
        prefs.edit().remove("added." + p.dir).remove("sha256." + p.dir).commit();
        if (p.dir.equals(choice())) {
            setChoice(SYSTEM);
        }
    }

    /** The package already added with the same name and version, if any. */
    Pkg same(Pkg p) {
        for (Pkg q : list()) {
            if (q.name.equals(p.name) && q.version.equals(p.version)) {
                return q;
            }
        }
        return null;
    }

    /** An added package whose source file had this SHA-256. */
    Pkg withSha(String sha) {
        for (Pkg q : list()) {
            if (q.sha256.equalsIgnoreCase(sha)) {
                return q;
            }
        }
        return null;
    }

    /** The folder name for a package: its name and version in the characters the runtime accepts. */
    static String folderName(Pkg p) {
        String s = (p.name + (p.version.isEmpty() ? "" : "_" + p.version)).replaceAll("[^A-Za-z0-9._-]+", "_");
        s = s.replaceAll("^[._]+", "").replaceAll("_+$", "");
        if (s.length() > 80) {
            s = s.substring(0, 80);
        }
        if (s.isEmpty() || s.equals(SYSTEM)) {
            s = "driver" + (s.isEmpty() ? "" : "_" + s);
        }
        return s;
    }

    /**
     * Checks a picked or downloaded file and unpacks it into a staging folder:
     * a driver package, the Android version it needs, the free space. The
     * file is copied first, so the picked file is only read.
     */
    Staged stage(InputStream src) throws AddError {
        File tmp = new File(ctx.getCacheDir(), "driver-add.zip");
        File folder = new File(root, ".new");
        delete(folder);
        tmp.delete();
        try {
            String sha;
            try {
                sha = copy(src, tmp);
            } catch (IOException e) {
                throw unreadable(e);
            }
            byte[] head = new byte[4];
            int n;
            try (InputStream in = new FileInputStream(tmp)) {
                n = in.read(head);
            } catch (IOException e) {
                throw unreadable(e);
            }
            if (n < 4 || head[0] != 'P' || head[1] != 'K' || head[2] != 3 || head[3] != 4) {
                throw notPackage("the file is not a zip archive");
            }
            Staged s = new Staged();
            s.folder = folder;
            long need = 0;
            try (ZipFile z = new ZipFile(tmp)) {
                ZipEntry me = z.getEntry("meta.json");
                if (me == null) {
                    throw notPackage("no meta.json at the top of the archive");
                }
                JSONObject m;
                try (InputStream in = z.getInputStream(me)) {
                    m = new JSONObject(new String(readAll(in), "UTF-8"));
                } catch (org.json.JSONException e) {
                    throw notPackage("meta.json is not JSON: " + e.getMessage());
                }
                String lib = m.optString("libraryName", "");
                if (lib.isEmpty() || lib.contains("/") || lib.startsWith(".") || z.getEntry(lib) == null) {
                    throw notPackage("meta.json names no library of the package");
                }
                int min = m.optInt("minApi", 0);
                if (min > Build.VERSION.SDK_INT) {
                    throw new AddError("This driver needs Android " + androidName(min) + " or newer",
                            "This phone has Android " + Build.VERSION.RELEASE + ".");
                }
                java.util.Enumeration<? extends ZipEntry> all = z.entries();
                while (all.hasMoreElements()) {
                    ZipEntry e = all.nextElement();
                    if (!e.isDirectory()) {
                        need += Math.max(0, e.getSize());
                    }
                }
                need += 4L << 20;
                root.mkdirs();
                long free = root.getUsableSpace();
                if (need > free) {
                    long mb = (need - free + (1 << 20) - 1) >> 20;
                    throw new AddError("Not enough free space", "This driver needs " + mb + " MB more room on the phone.");
                }
                if (!folder.mkdirs()) {
                    throw unreadable(new IOException("cannot make " + folder));
                }
                byte[] buf = new byte[1 << 16];
                all = z.entries();
                while (all.hasMoreElements()) {
                    ZipEntry e = all.nextElement();
                    String en = e.getName();
                    if (e.isDirectory() || en.contains("/") || en.contains("\\") || en.startsWith(".")) {
                        continue;
                    }
                    File f = new File(folder, en);
                    try (InputStream in = z.getInputStream(e); OutputStream out = new FileOutputStream(f)) {
                        for (int r; (r = in.read(buf)) > 0; ) {
                            out.write(buf, 0, r);
                        }
                    }
                    f.setReadOnly();
                }
            } catch (AddError e) {
                throw e;
            } catch (IOException e) {
                throw unreadable(e);
            }
            try {
                s.pkg = meta(folder);
            } catch (Exception e) {
                throw notPackage(e.toString());
            }
            s.pkg.sha256 = sha;
            label(s.pkg);
            return s;
        } catch (AddError e) {
            delete(folder);
            Log.w(TAG, "GPU driver not added: " + e.getMessage());
            throw e;
        } finally {
            tmp.delete();
        }
    }

    /** Moves a staged package that passed its test load into place, replacing a package with the same name and version. */
    Pkg commit(Staged s) throws AddError {
        Pkg old = same(s.pkg);
        boolean chosen = old != null && old.dir.equals(choice());
        if (old != null) {
            remove(old);
        }
        String name = folderName(s.pkg);
        File dest = new File(root, name);
        delete(dest);
        if (!s.folder.renameTo(dest)) {
            discard(s);
            throw unreadable(new IOException("cannot move the package to " + dest));
        }
        prefs.edit().putLong("added." + name, System.currentTimeMillis()).putString("sha256." + name, s.pkg.sha256)
                .commit();
        if (chosen) {
            setChoice(name);
        }
        Log.i(TAG, "GPU driver package " + name + " added (" + s.pkg.library + ")");
        return find(name);
    }

    void discard(Staged s) {
        if (s != null) {
            delete(s.folder);
        }
    }

    static AddError notPackage(String why) {
        Log.w(TAG, "GPU driver file: " + why);
        return new AddError("Can't add this file",
                "This isn't a GPU driver package. Pick a .zip or .adpkg driver file, for example a Turnip release.");
    }

    static AddError unreadable(Exception e) {
        Log.w(TAG, "GPU driver file: " + e);
        return new AddError("Couldn't read this file",
                "It may be damaged or not fully downloaded. Try downloading it again.");
    }

    static String androidName(int api) {
        switch (api) {
            case 21: return "5";
            case 22: return "5.1";
            case 23: return "6";
            case 24: return "7";
            case 25: return "7.1";
            case 26: return "8";
            case 27: return "8.1";
            case 28: return "9";
            case 29: return "10";
            case 30: return "11";
            case 31: return "12";
            case 32: return "12L";
            default: return api < 21 ? "5" : Integer.toString(api - 20);
        }
    }

    /** The phone's own driver from the last check of this Android build, or null before one. */
    SystemDriver system() {
        String s = prefs.getString("system." + Build.FINGERPRINT, null);
        return s == null ? null : parseSystem(s);
    }

    void saveSystem(String probe) {
        prefs.edit().putString("system." + Build.FINGERPRINT, probe).commit();
    }

    static SystemDriver parseSystem(String probe) {
        SystemDriver d = new SystemDriver();
        String[] l = probe.split("\n", -1);
        if (l.length < 7 || !l[0].equals("ok")) {
            d.reason = l.length > 1 ? l[1] : probe;
            return d;
        }
        d.usable = true;
        d.gpu = gpuName(l[1]);
        d.name = d.gpu + " driver (system)";
        long v = Long.parseLong(l[3]) & 0xffffffffL;
        int vendor = (int) Long.parseLong(l[2]);
        if (vendor == 0x5143) {
            d.version = String.format(Locale.US, "%04d.%02d", (v >> 12) & 0x3ff, v & 0xfff);
        } else {
            d.version = String.format(Locale.US, "%d.%d.%d", v >> 22, (v >> 12) & 0x3ff, v & 0xfff);
        }
        return d;
    }

    /** "Adreno (TM) 740" as "Adreno 740". */
    static String gpuName(String device) {
        return device.replace("(TM)", "").replace("(R)", "").replaceAll("\\s+", " ").trim();
    }

    static String copy(InputStream src, File dest) throws IOException {
        MessageDigest md;
        try {
            md = MessageDigest.getInstance("SHA-256");
        } catch (Exception e) {
            throw new IOException(e);
        }
        byte[] buf = new byte[1 << 16];
        try (InputStream in = new BufferedInputStream(src); OutputStream out = new FileOutputStream(dest)) {
            for (int r; (r = in.read(buf)) > 0; ) {
                out.write(buf, 0, r);
                md.update(buf, 0, r);
            }
        }
        return hex(md.digest());
    }

    static String hex(byte[] b) {
        StringBuilder s = new StringBuilder();
        for (byte x : b) {
            s.append(String.format("%02x", x & 0xff));
        }
        return s.toString();
    }

    static byte[] readAll(InputStream in) throws IOException {
        java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        for (int r; (r = in.read(buf)) > 0; ) {
            o.write(buf, 0, r);
        }
        return o.toByteArray();
    }

    static void delete(File f) {
        File[] kids = f.listFiles();
        if (kids != null) {
            for (File k : kids) {
                delete(k);
            }
        }
        f.delete();
    }
}
