package dev.relift.app;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * The game's shader math choice (the runtime's gpu.math setting): exact, the
 * console's own results, or native, this phone's GPU math, which is faster
 * and may change the picture slightly. Each app keeps its own choice, as it
 * keeps its own GPU driver; the game reads it when it starts.
 */
final class Maths {
    static final String EXACT = "exact";
    static final String NATIVE = "native";

    final SharedPreferences prefs;

    Maths(Context ctx) {
        this.prefs = ctx.getSharedPreferences("settings", Context.MODE_PRIVATE);
    }

    /** The choice: EXACT (the default) or NATIVE. */
    String choice() {
        return NATIVE.equals(prefs.getString("gpu.math", EXACT)) ? NATIVE : EXACT;
    }

    void setChoice(String mode) {
        prefs.edit().putString("gpu.math", NATIVE.equals(mode) ? NATIVE : EXACT).commit();
    }

    /** The name the settings pages show for a mode. */
    static String name(String mode) {
        return NATIVE.equals(mode) ? "Fast" : "Exact";
    }

    /** What a mode means for the player. */
    static String sub(String mode) {
        return NATIVE.equals(mode) ? "Faster on most phones. The picture may differ slightly from the console's"
                : "The same picture as the console";
    }
}
