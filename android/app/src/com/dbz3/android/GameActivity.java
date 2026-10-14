package com.dbz3.android;

import android.os.Bundle;
import android.system.Os;
import android.util.Log;

import org.libsdl.app.SDLActivity;

/**
 * Hosts the recompiled game. Runs in its own process (":game") so a native
 * crash does not take the launcher down and the log stays readable.
 */
public class GameActivity extends SDLActivity {
    public static final String EXTRA_ROOT = "root";
    public static final String OPT_TOUCH = "opt_touch";
    public static final String OPT_VSYNC = "opt_vsync";
    public static final String OPT_OCCLUSION = "opt_occlusion";
    public static final String OPT_ASYNC = "opt_async";
    public static final String OPT_MSAA = "opt_msaa";
    public static final String OPT_SPARSE = "opt_sparse";
    public static final String OPT_DYNREND = "opt_dynrend";

    boolean opt(String key, boolean def) { return getIntent().getBooleanExtra(key, def); }

    @Override protected void onCreate(Bundle b) {
        String root = getIntent().getStringExtra(EXTRA_ROOT);
        if (root == null) root = MainActivity.defaultRoot();
        try {
            Os.setenv("REX_ANDROID_ROOT", root, true);
        } catch (Exception e) {
            Log.e("DBZ3", "setenv failed", e);
        }
        super.onCreate(b);
        // On-screen gamepad over the SDL surface (toggle with the ◐ button).
        if (mLayout != null && opt(OPT_TOUCH, true)) {
            mLayout.addView(new TouchControls(this), new android.view.ViewGroup.LayoutParams(
                    android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                    android.view.ViewGroup.LayoutParams.MATCH_PARENT));
        }
    }

    @Override protected String[] getLibraries() {
        // SDL3 (with its JNI_OnLoad) is linked into librexruntime.so; libmain.so has SDL_main.
        return new String[] {"c++_shared", "rexruntime", "main"};
    }

    @Override protected String[] getArguments() {
        java.util.List<String> a = new java.util.ArrayList<>(java.util.Arrays.asList(baseArguments()));
        a.add("--dbz3_vsync=" + opt(OPT_VSYNC, true));
        a.add("--dbz3_occlusion_queries=" + opt(OPT_OCCLUSION, false));
        a.add("--dbz3_async_shaders=" + opt(OPT_ASYNC, true));
        a.add("--dbz3_native_2x_msaa=" + opt(OPT_MSAA, false));
        // Sparse binding of the 512 MB guest-memory buffer: cheap on desktop GPUs,
        // often slow on Adreno drivers. Off = one plain allocation.
        a.add("--vulkan_sparse_shared_memory=" + opt(OPT_SPARSE, false));
        a.add("--vulkan_dynamic_rendering=" + opt(OPT_DYNREND, true));
        return a.toArray(new String[0]);
    }

    String[] baseArguments() {
        return new String[] {
            "--gpu_backend=vulkan",
            "--dbz3_gpu_backend=vulkan",
            "--dbz3_skip_launcher=true",
            "--dbz3_region=eu",
            "--log_level=info",
            "--log_flush_interval=1",
            // fps + per-thread CPU load every 5 s (diagnosis while the port matures)
            "--dbz3_perf_logging=true",
            // No FidelityFX on Android: plain bilinear output, manual preset.
            "--dbz3_present_effect=bilinear",
            "--dbz3_quality_preset=manual",
            // Touches must not turn into mouse-look.
            "--dbz3_mnk_mouse=false",
            "--dbz3_mnk_mode=true",
            "--dbz3_input_backend=sdl",
            "--dbz3_input_glyphs=playstation",
        };
    }
}
