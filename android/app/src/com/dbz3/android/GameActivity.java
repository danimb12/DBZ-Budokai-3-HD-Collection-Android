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
        if (mLayout != null) {
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
        return new String[] {
            "--gpu_backend=vulkan",
            "--dbz3_gpu_backend=vulkan",
            "--dbz3_skip_launcher=true",
            "--dbz3_region=eu",
            "--log_level=info",
            "--log_flush_interval=1",
            // No FidelityFX on Android: plain bilinear output, manual preset.
            "--dbz3_present_effect=bilinear",
            "--dbz3_quality_preset=manual",
            // 2x MSAA is costly on a tiler GPU with EDRAM emulation: off by default
            // (can be re-enabled from the in-game quick menu, SELECT+START).
            "--dbz3_native_2x_msaa=false",
            // Touches must not turn into mouse-look.
            "--dbz3_mnk_mouse=false",
            "--dbz3_mnk_mode=true",
            "--dbz3_input_backend=sdl",
            "--dbz3_input_glyphs=playstation",
        };
    }
}
