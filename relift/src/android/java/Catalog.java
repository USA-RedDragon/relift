package dev.relift.app;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.NetworkCapabilities;
import android.util.Log;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.UnknownHostException;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import javax.net.ssl.HttpsURLConnection;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * The list of drivers this app version offers (the asset gpu_drivers.json,
 * part of the APK, never fetched) and the download of one entry: https only,
 * checked against the entry's SHA-256 before anything is installed.
 */
final class Catalog {
    /** One entry of the list. */
    static final class Entry {
        String name;
        String version;
        String author;
        long size;
        List<String> worksWith = new ArrayList<>();
        String license;
        String url;
        String sha256;

        String line1() {
            return "Version " + version + " \u00b7 by " + author + " \u00b7 " + megabytes(size);
        }

        String worksLine() {
            return "Works with: " + join(worksWith);
        }

        boolean recommendedFor(String gpu) {
            return gpu != null && worksWith.contains(gpu);
        }
    }

    /** How a download ended. */
    static final class Failed extends Exception {
        static final int OFFLINE = 1;
        static final int STOPPED = 2;
        static final int MISMATCH = 3;
        final int kind;

        Failed(int kind, String why) {
            super(why);
            this.kind = kind;
        }
    }

    /** Progress of a download, from any thread. */
    interface Progress {
        void at(long done, long total);
    }

    /** Tells a running download to stop. */
    static final class Cancel {
        volatile boolean set;
    }

    static List<Entry> load(Context ctx) {
        ArrayList<Entry> out = new ArrayList<>();
        try (InputStream in = ctx.getAssets().open("gpu_drivers.json")) {
            JSONArray a = new JSONObject(new String(Drivers.readAll(in), "UTF-8")).getJSONArray("drivers");
            for (int i = 0; i < a.length(); i++) {
                JSONObject o = a.getJSONObject(i);
                Entry e = new Entry();
                e.name = o.getString("name");
                e.version = o.getString("version");
                e.author = o.getString("author");
                e.size = o.getLong("size");
                JSONArray w = o.getJSONArray("works_with");
                for (int j = 0; j < w.length(); j++) {
                    e.worksWith.add(w.getString(j));
                }
                e.license = o.getString("license");
                e.url = o.getString("url");
                e.sha256 = o.getString("sha256").toLowerCase(Locale.US);
                if (e.url.startsWith("https://") && e.sha256.length() == 64) {
                    out.add(e);
                }
            }
        } catch (java.io.FileNotFoundException e) {
            return out;
        } catch (Exception e) {
            Log.w(Drivers.TAG, "gpu_drivers.json: " + e);
        }
        return out;
    }

    /**
     * Downloads e into dest and checks it. Throws Failed with OFFLINE when the
     * phone has no connection, STOPPED when the download breaks off or is
     * cancelled, MISMATCH when the file is not the listed one; dest is
     * deleted in every failure.
     */
    static void download(Context ctx, Entry e, File dest, Progress p, Cancel c) throws Failed {
        dest.delete();
        if (!online(ctx)) {
            throw new Failed(Failed.OFFLINE, "no network");
        }
        HttpURLConnection conn = null;
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            URL url = new URL(e.url);
            long total = e.size;
            for (int hop = 0; ; hop++) {
                conn = (HttpURLConnection) url.openConnection();
                if (!(conn instanceof HttpsURLConnection)) {
                    throw new IOException("not https: " + url);
                }
                conn.setInstanceFollowRedirects(false);
                conn.setConnectTimeout(15000);
                conn.setReadTimeout(20000);
                int code = conn.getResponseCode();
                if (code >= 300 && code < 400 && hop < 5) {
                    String loc = conn.getHeaderField("Location");
                    conn.disconnect();
                    url = new URL(url, loc);
                    continue;
                }
                if (code != 200) {
                    throw new IOException("HTTP " + code);
                }
                break;
            }
            long done = 0;
            byte[] buf = new byte[1 << 16];
            try (InputStream in = conn.getInputStream(); OutputStream out = new FileOutputStream(dest)) {
                for (int r; (r = in.read(buf)) > 0; ) {
                    if (c.set) {
                        throw new IOException("cancelled");
                    }
                    out.write(buf, 0, r);
                    md.update(buf, 0, r);
                    done += r;
                    p.at(done, total);
                }
            }
            if (c.set) {
                throw new IOException("cancelled");
            }
            String got = Drivers.hex(md.digest());
            if (!got.equals(e.sha256)) {
                dest.delete();
                Log.w(Drivers.TAG, "GPU driver download " + e.url + ": SHA-256 " + got + ", list says " + e.sha256);
                throw new Failed(Failed.MISMATCH, "checksum");
            }
        } catch (Failed f) {
            dest.delete();
            throw f;
        } catch (UnknownHostException x) {
            dest.delete();
            Log.w(Drivers.TAG, "GPU driver download: " + x);
            throw new Failed(Failed.OFFLINE, x.toString());
        } catch (Exception x) {
            dest.delete();
            Log.w(Drivers.TAG, "GPU driver download: " + x);
            throw new Failed(Failed.STOPPED, x.toString());
        } finally {
            if (conn != null) {
                conn.disconnect();
            }
        }
    }

    static boolean online(Context ctx) {
        ConnectivityManager cm = ctx.getSystemService(ConnectivityManager.class);
        if (cm == null) {
            return true;
        }
        NetworkCapabilities nc = cm.getNetworkCapabilities(cm.getActiveNetwork());
        return nc != null && nc.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET);
    }

    static String megabytes(long n) {
        return String.format(Locale.US, "%.1f MB", n / 1e6);
    }

    static String join(List<String> l) {
        StringBuilder b = new StringBuilder();
        for (String s : l) {
            b.append(b.length() > 0 ? ", " : "").append(s);
        }
        return b.toString();
    }
}
