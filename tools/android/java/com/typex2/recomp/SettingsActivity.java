package com.typex2.recomp;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * The app's home-screen entry: lists the games found in ES-DE's ROMs folder (ROMs/arcade/*.x2)
 * and the app's own files/game, and edits each game's x2.env (FPS counter, touch pad, pad
 * layout) before playing it. ES-DE launches org.libsdl.app.SDLActivity directly, so this screen
 * only appears when the app is opened from the launcher. Lines of x2.env it does not manage stay
 * as they are.
 */
public final class SettingsActivity extends Activity {
    /** A folder holding a supported game. */
    private static final class Found {
        final File dir;
        final String[] game; // Games.LIST row: id, name, base, size, entry

        Found(File dir, String[] game) {
            this.dir = dir;
            this.game = game;
        }
    }

    /** One choice of a setting: its x2.env value (null: no line) and how it reads. */
    private static final class Choice {
        final String value, label, detail;

        Choice(String value, String label, String detail) {
            this.value = value;
            this.label = label;
            this.detail = detail;
        }
    }

    private Found open; // the game whose settings show, or null for the list

    @Override
    protected void onResume() {
        super.onResume();
        if (open != null && open.dir.isDirectory()) showGame(open);
        else showList();
    }

    // Back from a game returns to the list (apps targeting API 36 get no onBackPressed)
    private final OnBackInvokedCallback backToList = this::showList;
    private boolean backRegistered;

    private void backGoesToList(boolean on) {
        if (on == backRegistered) return;
        OnBackInvokedDispatcher d = getOnBackInvokedDispatcher();
        if (on) d.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, backToList);
        else d.unregisterOnBackInvokedCallback(backToList);
        backRegistered = on;
    }

    /* Pads: A confirms like the d-pad centre, B goes back, wherever the system's fallbacks don't. */
    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        int k = e.getKeyCode();
        if (k == KeyEvent.KEYCODE_BUTTON_A || k == KeyEvent.KEYCODE_BUTTON_B) {
            if (e.getAction() == KeyEvent.ACTION_UP) {
                View f = getCurrentFocus();
                if (k == KeyEvent.KEYCODE_BUTTON_B) { if (open != null) showList(); else finish(); }
                else if (f != null) f.performClick();
            }
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    // ---- finding games ---------------------------------------------------------------------

    private static long hex(String s) {
        return Long.parseLong(s.substring(2), 16);
    }

    private static long rd32(byte[] b, int o) {
        return (b[o] & 0xffL) | (b[o + 1] & 0xffL) << 8 | (b[o + 2] & 0xffL) << 16 | (b[o + 3] & 0xffL) << 24;
    }

    /** The game in a folder, from its executable's PE header (as launcher.c does), or null. */
    private static String[] identify(File dir) {
        for (String exe : new String[] {"game.exe", "launcher.exe"}) {
            File f = new File(dir, exe);
            if (!f.isFile()) continue;
            byte[] h = new byte[1024];
            int n;
            try (FileInputStream in = new FileInputStream(f)) {
                n = in.read(h);
            } catch (IOException e) {
                return null;
            }
            if (n < 64 || h[0] != 'M' || h[1] != 'Z') return null;
            int pe = (int) rd32(h, 0x3c);
            if (pe < 0 || pe + 0x58 > n || h[pe] != 'P' || h[pe + 1] != 'E') return null;
            int opt = pe + 24;
            long entry = rd32(h, opt + 16), base = rd32(h, opt + 28), size = rd32(h, opt + 56);
            for (String[] g : Games.LIST)
                if (hex(g[2]) == base && hex(g[3]) == size && hex(g[4]) == base + entry) return g;
            return null;
        }
        return null;
    }

    /** ES-DE's ROMs folder: its ROMDirectory setting, else the default ROMs on shared storage. */
    private static File romDirectory() {
        File storage = Environment.getExternalStorageDirectory();
        File cfg = new File(storage, "ES-DE/settings/es_settings.xml");
        try {
            String xml = new String(Files.readAllBytes(cfg.toPath()), StandardCharsets.UTF_8);
            Matcher m = Pattern.compile("name=\"ROMDirectory\"\\s+value=\"([^\"]+)\"").matcher(xml);
            if (m.find()) return new File(m.group(1).replace("~", storage.getPath()));
        } catch (IOException ignored) {
        }
        return new File(storage, "ROMs");
    }

    private List<Found> scan() {
        List<File> dirs = new ArrayList<>();
        File[] arcade = new File(romDirectory(), "arcade").listFiles();
        if (arcade != null) {
            Arrays.sort(arcade);
            dirs.addAll(Arrays.asList(arcade));
        }
        File own = getExternalFilesDir(null);
        if (own != null) dirs.add(new File(own, "game"));
        List<Found> games = new ArrayList<>();
        for (File d : dirs) {
            if (!d.isDirectory()) continue;
            String[] g = identify(d);
            if (g != null) games.add(new Found(d, g));
        }
        return games;
    }

    // ---- x2.env ----------------------------------------------------------------------------

    private static List<String> readEnv(File dir) {
        try {
            return new ArrayList<>(Files.readAllLines(new File(dir, "x2.env").toPath(), StandardCharsets.UTF_8));
        } catch (IOException e) {
            return new ArrayList<>();
        }
    }

    private static String envValue(List<String> lines, String key) {
        for (String l : lines)
            if (l.startsWith(key + "=")) return l.substring(key.length() + 1).trim();
        return null;
    }

    /** Sets one key (null removes it) and keeps every other line. */
    private boolean writeEnv(File dir, String key, String value) {
        List<String> lines = readEnv(dir);
        boolean placed = false;
        for (int i = 0; i < lines.size(); i++) {
            if (!lines.get(i).startsWith(key + "=")) continue;
            if (value != null && !placed) {
                lines.set(i, key + "=" + value);
                placed = true;
            } else {
                lines.remove(i--);
            }
        }
        if (value != null && !placed) lines.add(key + "=" + value);
        File env = new File(dir, "x2.env"), tmp = new File(dir, "x2.env.tmp");
        try {
            StringBuilder s = new StringBuilder();
            for (String l : lines) s.append(l).append('\n');
            Files.write(tmp.toPath(), s.toString().getBytes(StandardCharsets.UTF_8));
            if (!tmp.renameTo(env)) throw new IOException("rename");
            return true;
        } catch (IOException e) {
            tmp.delete();
            Toast.makeText(this, "Could not save " + env.getPath(), Toast.LENGTH_LONG).show();
            return false;
        }
    }

    // ---- screens ---------------------------------------------------------------------------

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private LinearLayout page() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(24), dp(20), dp(24), dp(24));
        ScrollView scroll = new ScrollView(this);
        scroll.setFitsSystemWindows(true); // clear of the status and navigation bars (edge to edge since API 35)
        scroll.addView(col);
        setContentView(scroll);
        return col;
    }

    private TextView text(LinearLayout col, String s, int sp, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(0, dp(8), 0, dp(4));
        col.addView(t);
        return t;
    }

    private Button button(LinearLayout col, String s, View.OnClickListener click) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        b.setOnClickListener(click);
        int match = ViewGroup.LayoutParams.MATCH_PARENT, wrap = ViewGroup.LayoutParams.WRAP_CONTENT;
        col.addView(b, new LinearLayout.LayoutParams(match, wrap));
        return b;
    }

    private void showList() {
        open = null;
        backGoesToList(false);
        LinearLayout col = page();
        text(col, "Type X2", 26, true);
        View first = null;
        if (!Environment.isExternalStorageManager()) {
            text(col, "To find games in your ROMs folder, this app needs \"All files access\".", 16, false);
            first = button(col, "Allow all files access", v -> startActivity(new Intent(
                    Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName()))));
        }
        List<Found> games = scan();
        if (games.isEmpty()) {
            text(col, "No games found. Put each game in its own folder named <Game>.x2 in "
                    + new File(romDirectory(), "arcade").getPath()
                    + ", then launch it from ES-DE or from here.", 16, false);
        } else {
            text(col, "Games", 18, true);
            for (Found g : games) {
                Button b = button(col, g.game[1] + "\n" + g.dir.getPath(), v -> showGame(g));
                if (first == null) first = b;
            }
        }
        if (first != null) first.requestFocus();
    }

    /** One setting as radio buttons; picking one writes it at once. */
    private void choices(LinearLayout col, Found g, String title, String key, Choice[] options) {
        text(col, title, 18, true);
        String now = envValue(readEnv(g.dir), key);
        RadioGroup group = new RadioGroup(this);
        List<Choice> shown = new ArrayList<>(Arrays.asList(options));
        boolean known = false;
        for (Choice c : options) known |= c.value == null ? now == null : c.value.equals(now);
        if (!known) shown.add(new Choice(now, "Custom", now)); // a line written by hand stays selectable
        for (Choice c : shown) {
            RadioButton r = new RadioButton(this);
            r.setText(c.detail == null ? c.label : c.label + "\n" + c.detail);
            r.setPadding(dp(8), dp(8), 0, dp(8));
            r.setChecked(c.value == null ? now == null : c.value.equals(now));
            r.setOnClickListener(v -> writeEnv(g.dir, key, c.value));
            group.addView(r);
        }
        col.addView(group);
    }

    private static Choice[] padLayouts(String id) {
        Choice def;
        Choice stick;
        if (id.equals("ssf4ae")) {
            def = new Choice(null, "Pad (default)",
                    "LP MP HP on Y X R1, LK MK HK on B A R2; L1 / L2 all punches / kicks");
            stick = new Choice("l1=1,west=2,north=3,r1=123,l2=4,south=5,east=6,r2=456", "8-button stick",
                    "L1 Y X R1 = LP MP HP 3P, L2 B A R2 = LK MK HK 3K");
        } else if (id.equals("bbcs") || id.equals("bbct")) {
            def = new Choice(null, "Pad (default)",
                    "A B C on Y X R1, D on B; A = B+C, R2 = A+B, L1 = A+B+C, L2 = A+B+C+D");
            stick = new Choice("l1=1,west=2,north=3,r1=12,l2=4,south=23,east=123,r2=1234", "8-button stick",
                    "L1 Y X R1 = A B C A+B, L2 B A R2 = D B+C A+B+C A+B+C+D");
        } else {
            def = new Choice(null, "Pad (default)", "Y B X A = A B C D; R2 = A+B, R1 = C+D, L2 = A+B+C, L1 = B+C+D");
            stick = new Choice("l1=a,west=c,north=ab,r1=bc,l2=b,south=d,east=cd,r2=abc", "8-button stick",
                    "L1 Y X R1 = A C A+B B+C, L2 B A R2 = B D C+D A+B+C");
        }
        return new Choice[] {def, stick};
    }

    private void showGame(Found g) {
        open = g;
        backGoesToList(true);
        LinearLayout col = page();
        text(col, g.game[1], 24, true);
        text(col, g.dir.getPath(), 14, false);
        Button play = button(col, "Play", v -> {
            Intent i = new Intent().setComponent(new ComponentName(this, "org.libsdl.app.SDLActivity"));
            i.putExtra("ROM", g.dir.getPath());
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startActivity(i);
        });
        play.setGravity(Gravity.CENTER);

        if (!g.dir.canWrite())
            text(col, "This folder is read-only for the app: allow \"All files access\" to change its settings.",
                    15, false);

        Switch fps = new Switch(this);
        fps.setText("FPS counter");
        fps.setTextSize(18);
        fps.setPadding(0, dp(16), 0, dp(8));
        String f = envValue(readEnv(g.dir), "X2_FPS");
        fps.setChecked(f != null && !f.equals("0"));
        fps.setOnCheckedChangeListener((v, on) -> writeEnv(g.dir, "X2_FPS", on ? "1" : null));
        col.addView(fps);

        choices(col, g, "Pad layout", "X2_PAD", padLayouts(g.game[0]));
        choices(col, g, "On-screen touch pad", "X2_TOUCH", new Choice[] {
            new Choice(null, "Automatic", "shown on touch screens while no pad is connected"),
            new Choice("1", "Always", null),
            new Choice("0", "Never", null),
        });
        text(col, "Settings are saved in x2.env in the game folder; other lines there are kept.", 14, false);
        play.requestFocus();
    }
}
