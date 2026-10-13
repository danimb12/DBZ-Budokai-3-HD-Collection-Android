package com.dbz3.android;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.content.SharedPreferences;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.Locale;

public class MainActivity extends Activity {
    static { System.loadLibrary("dbz3probe"); }
    static native String runProbe();

    // EU/PAL files from baserom.md (name, size, sha256 or null = size only)
    static final String[][] EU_FILES = {
        {"eu/data_cmn.afs", "293423104", "C963502B00AF6B85C4C12A8AB28ACAE86B2EF57559682174D194DF78021B2F49"},
        {"eu/data_eng.afs", "64284672", "AC51F1D273EB7A2A50F685282723651AAFDD54159F4BF2237BECD5E484EA8958"},
        {"eu/data_fra.afs", "64727040", "E742E911F6656EE0C12DA77E29519FF089E22E2771790FD4797016FC5F230EDA"},
        {"eu/data_ger.afs", "66117632", "83C51CB4D6DB408868AF6487C0AA0CDDEAC20C32BB28324F08E6215E0B32A328"},
        {"eu/data_ita.afs", "64315392", "EFECE6723C2D7859FCB773E446CEA9489394837E632BC68C79A9066B2FFC7E4A"},
        {"eu/data_spn.afs", "65036288", "FAA80E60E16536FC43365762253A59770EF2D7DD897A49DCCDFBC334BD9A12A5"},
        {"eu/data_yah.afs", "1632256", "ECACA9299CA76A642A716B1B5A0B34C8E74E08AAEE5ADBA4E1F15A5E61B845A9"},
        {"eu/adx_jpn.afs", "727304192", "308EE1FF10E273D4A984504E8412B41401C58D01CC09A1AB0EEA2DC9EE758077"},
        {"eu/adx_usa.afs", "681025536", "D742C2A65CDC338E5643D3240B77EF52298A64538910FEB871E9F2AAB023AEF6"},
        {"eu/lang_jpn.afs", "19107840", "B76B558E2737E3C0E171540F6C31B1FDF3423D7F94D998F227E18F0BFBE344DE"},
        {"eu/lang_usa.afs", "17618944", "DF5CBF644F0754DC0A07D7477E3020473F34A1AF9167F51EA68FF3D9D9D10D77"},
        {"eu/opening.sfd", "76771328", null},
        {"eu/Ending00.sfd", "88363008", null},
    };
    static final String XEX_EU = "7193803AEE6124C8D0782EF2C37FF2E8D41DB8A11286D198C23352EA7622E924";
    static final String XEX_US = "B40BBA40CFD6C90CB269EBF5020818924F43109BCC86827A3CC37124C052A26B";

    TextView out;
    EditText path;
    StringBuilder report = new StringBuilder();
    Handler ui = new Handler(Looper.getMainLooper());
    volatile boolean busy = false;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(32, 48, 32, 32);
        root.setBackgroundColor(Color.rgb(18, 18, 22));

        TextView title = new TextView(this);
        title.setText("DBZ Budokai 3 · Android  v0.4.1");
        title.setTextSize(22);
        title.setTextColor(Color.rgb(255, 140, 26));
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);
        TextView sub = new TextView(this);
        sub.setText("Comprobación del dispositivo y de tus archivos del juego");
        sub.setTextColor(Color.LTGRAY);
        root.addView(sub);

        path = new EditText(this);
        path.setText(defaultRoot());
        path.setTextColor(Color.WHITE);
        path.setSingleLine(true);
        root.addView(path);

        Button play = button("▶  ARRANCAR JUEGO", v -> launchGame());
        play.setTextSize(18);
        root.addView(play);
        LinearLayout row1 = new LinearLayout(this);
        LinearLayout row2 = new LinearLayout(this);
        LinearLayout row3 = new LinearLayout(this);
        root.addView(row1);
        root.addView(row2);
        root.addView(row3);
        row3.addView(button("Compartir último log del juego", v -> shareLastLog()), weight());

        // Performance levers, passed to GameActivity as cvars (A/B tests without new builds).
        TextView opts = new TextView(this);
        opts.setText("Opciones (se aplican al arrancar):");
        opts.setTextColor(Color.LTGRAY);
        root.addView(opts);
        LinearLayout o1 = new LinearLayout(this);
        LinearLayout o2 = new LinearLayout(this);
        LinearLayout o3 = new LinearLayout(this);
        root.addView(o1);
        root.addView(o2);
        root.addView(o3);
        o1.addView(option(GameActivity.OPT_TOUCH, "Controles táctiles", true), weight());
        o1.addView(option(GameActivity.OPT_VSYNC, "VSync", true), weight());
        o2.addView(option(GameActivity.OPT_OCCLUSION, "Occlusion queries", false), weight());
        o2.addView(option(GameActivity.OPT_ASYNC, "Shaders asíncronos", true), weight());
        o3.addView(option(GameActivity.OPT_MSAA, "MSAA 2x", false), weight());
        row1.addView(button("1. Analizar móvil", v -> probe()), weight());
        row1.addView(button("2. Permiso archivos", v -> askFiles()), weight());
        row2.addView(button("3. Verificar juego", v -> verify()), weight());
        row2.addView(button("Compartir informe", v -> share()), weight());

        ScrollView sv = new ScrollView(this);
        out = new TextView(this);
        out.setTypeface(Typeface.MONOSPACE);
        out.setTextSize(11);
        out.setTextColor(Color.rgb(220, 220, 220));
        out.setTextIsSelectable(true);
        sv.addView(out);
        root.addView(sv, new LinearLayout.LayoutParams(-1, 0, 1f));
        setContentView(root);

        log("Pasos: 1) Analizar móvil  2) Dar permiso de archivos  3) Verificar juego.");
        log("Copia al móvil la carpeta del juego así:");
        log("  " + path.getText() + "/yae3_xenon.xex");
        log("  " + path.getText() + "/eu/  (todos los .afs y .sfd)");
        log("");
    }

    static String defaultRoot() {
        return Environment.getExternalStorageDirectory().getAbsolutePath() + "/DBZ3";
    }

    void launchGame() {
        if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
            log("Falta el permiso de archivos (botón 2).");
            return;
        }
        String base = path.getText().toString().trim();
        if (!new File(base, "eu/data_cmn.afs").isFile()) {
            log("No encuentro " + base + "/eu/data_cmn.afs. Revisa la carpeta (botón 3).");
            return;
        }
        new File(base, "logs").mkdirs();
        installAsset("gamecontrollerdb.txt", new File(base, "gamecontrollerdb.txt"));
        log("Arrancando el juego… Si se cierra, vuelve aquí y pulsa 'Compartir último log del juego'.");
        Intent i = new Intent(this, GameActivity.class);
        i.putExtra(GameActivity.EXTRA_ROOT, base);
        SharedPreferences p = prefs();
        i.putExtra(GameActivity.OPT_TOUCH, p.getBoolean(GameActivity.OPT_TOUCH, true));
        i.putExtra(GameActivity.OPT_VSYNC, p.getBoolean(GameActivity.OPT_VSYNC, true));
        i.putExtra(GameActivity.OPT_OCCLUSION, p.getBoolean(GameActivity.OPT_OCCLUSION, false));
        i.putExtra(GameActivity.OPT_ASYNC, p.getBoolean(GameActivity.OPT_ASYNC, true));
        i.putExtra(GameActivity.OPT_MSAA, p.getBoolean(GameActivity.OPT_MSAA, false));
        startActivity(i);
    }

    /** Copies a file shipped in the APK assets next to the game data (only if missing). */
    void installAsset(String name, File dst) {
        if (dst.isFile()) return;
        try (InputStream in = getAssets().open(name); FileOutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            log("Instalado " + dst.getName());
        } catch (Exception e) {
            log("No se pudo instalar " + name + ": " + e.getMessage());
        }
    }

    File lastLog() {
        File dir = new File(path.getText().toString().trim(), "logs");
        File[] fs = dir.listFiles((d, n) -> n.endsWith(".log"));
        File best = null;
        if (fs != null) for (File f : fs) if (best == null || f.lastModified() > best.lastModified()) best = f;
        return best;
    }

    void shareLastLog() {
        File f = lastLog();
        if (f == null) { log("Todavía no hay logs en " + path.getText() + "/logs"); return; }
        StringBuilder sb = new StringBuilder();
        try (InputStream in = new FileInputStream(f)) {
            byte[] buf = new byte[(int) Math.min(f.length(), 400_000)];
            long skip = Math.max(0, f.length() - buf.length);
            in.skip(skip);
            int n = in.read(buf);
            sb.append(new String(buf, 0, Math.max(0, n), "UTF-8"));
        } catch (Exception e) {
            sb.append("Error leyendo log: ").append(e);
        }
        Intent i = new Intent(Intent.ACTION_SEND);
        i.setType("text/plain");
        i.putExtra(Intent.EXTRA_SUBJECT, "DBZ3 Android log " + f.getName());
        i.putExtra(Intent.EXTRA_TEXT, "== " + f.getName() + " ==\n" + sb);
        startActivity(Intent.createChooser(i, "Compartir log"));
    }

    SharedPreferences prefs() { return getSharedPreferences("dbz3", MODE_PRIVATE); }

    CheckBox option(String key, String label, boolean def) {
        CheckBox c = new CheckBox(this);
        c.setText(label);
        c.setTextColor(Color.WHITE);
        c.setChecked(prefs().getBoolean(key, def));
        c.setOnCheckedChangeListener((b, on) -> prefs().edit().putBoolean(key, on).apply());
        return c;
    }

    LinearLayout.LayoutParams weight() { return new LinearLayout.LayoutParams(0, -2, 1f); }

    Button button(String t, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(t);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        return b;
    }

    void log(String s) {
        report.append(s).append('\n');
        ui.post(() -> out.setText(report.toString()));
    }

    void probe() {
        if (busy) return;
        busy = true;
        new Thread(() -> {
            log("===== DISPOSITIVO =====");
            log("Modelo: " + Build.MANUFACTURER + " " + Build.MODEL + " (" + Build.DEVICE + ")");
            log("Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")");
            if (Build.VERSION.SDK_INT >= 31) log("SoC: " + Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL);
            log("ABI: " + String.join(", ", Build.SUPPORTED_ABIS));
            ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
            ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
            am.getMemoryInfo(mi);
            log(String.format(Locale.ROOT, "RAM: %d MB total, %d MB libres", mi.totalMem >> 20, mi.availMem >> 20));
            log("Kernel: " + System.getProperty("os.version"));
            try { log(runProbe()); } catch (Throwable t) { log("ERROR en la sonda nativa: " + t); }
            busy = false;
        }).start();
    }

    void askFiles() {
        if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
            } catch (Exception e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        } else {
            log("Permiso de archivos: ya concedido.");
        }
    }

    void verify() {
        if (busy) return;
        if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
            log("Falta el permiso de archivos (botón 2).");
            return;
        }
        busy = true;
        final String base = path.getText().toString().trim();
        new Thread(() -> {
            log("===== ARCHIVOS DEL JUEGO en " + base + " =====");
            int bad = 0;
            File xex = null;
            for (String n : new String[]{"default.xex", "yae3_xenon.xex", "yae3_xenon_eu.xex"}) {
                File f = new File(base, n);
                if (f.isFile()) { xex = f; break; }
            }
            if (xex == null) { log("[FALTA] ejecutable .xex"); bad++; }
            else {
                String h = sha(xex, null);
                String reg = XEX_EU.equals(h) ? "EU/PAL ✔" : XEX_US.equals(h) ? "USA ✔ (este port usa EU de momento)" : "DESCONOCIDO ✘";
                log("[xex] " + xex.getName() + " → " + reg);
                if (reg.endsWith("✘")) bad++;
            }
            for (String[] e : EU_FILES) {
                File f = new File(base, e[0]);
                if (!f.isFile()) { log("[FALTA] " + e[0]); bad++; continue; }
                long want = Long.parseLong(e[1]);
                if (f.length() != want) { log("[TAMAÑO] " + e[0] + " " + f.length() + " (esperado " + want + ")"); bad++; continue; }
                if (e[2] == null) { log("[OK tamaño] " + e[0]); continue; }
                String h = sha(f, e[0]);
                if (e[2].equals(h)) log("[OK] " + e[0]);
                else { log("[SHA ✘] " + e[0] + " " + h); bad++; }
            }
            log(bad == 0 ? "Resultado: tus archivos coinciden con la copia EU esperada." : "Resultado: " + bad + " problema(s) en los archivos.");
            saveReport();
            busy = false;
        }).start();
    }

    String sha(File f, String label) {
        try (InputStream in = new FileInputStream(f)) {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] buf = new byte[1 << 20];
            long total = f.length(), done = 0, lastPct = -1;
            int r;
            while ((r = in.read(buf)) > 0) {
                md.update(buf, 0, r);
                done += r;
                long pct = done * 100 / Math.max(1, total);
                if (label != null && total > 100_000_000 && pct / 25 != lastPct) {
                    lastPct = pct / 25;
                    final String msg = "   … " + label + " " + pct + "%";
                    ui.post(() -> out.setText(report + msg));
                }
            }
            StringBuilder sb = new StringBuilder();
            for (byte x : md.digest()) sb.append(String.format("%02X", x));
            return sb.toString();
        } catch (Exception e) {
            return "ERROR " + e.getMessage();
        }
    }

    void saveReport() {
        try {
            File d = getExternalFilesDir(null);
            if (d != null) {
                try (FileOutputStream fo = new FileOutputStream(new File(d, "informe.txt"))) {
                    fo.write(report.toString().getBytes("UTF-8"));
                }
            }
        } catch (Exception ignored) {}
    }

    void share() {
        saveReport();
        Intent i = new Intent(Intent.ACTION_SEND);
        i.setType("text/plain");
        i.putExtra(Intent.EXTRA_SUBJECT, "Informe DBZ3 Android v0.2");
        i.putExtra(Intent.EXTRA_TEXT, report.toString());
        startActivity(Intent.createChooser(i, "Compartir informe"));
    }
}
