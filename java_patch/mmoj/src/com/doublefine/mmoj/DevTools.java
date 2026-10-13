// SPDX-License-Identifier: CC0-1.0
package com.doublefine.mmoj;

import android.app.Activity;
import android.app.GameManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.view.Surface;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

/**
 * Middle Manager of Justice: immersive full screen and Android Game Mode frame rates.
 * Game Mode Performance = uncapped (display's highest refresh rate), Standard = 60 fps,
 * Battery = 30 fps. The cap is applied by libcqhost around each AKURender call; the rate is also
 * requested from the display with Surface.setFrameRate, which opts out of Android's default
 * 60 fps cap for games. Hooked into MoaiActivity.onResume and onWindowFocusChanged by
 * build_mmoj.sh.
 */
public final class DevTools {
    // Registered by libcqhost (devtools.cpp). The console natives are unused here.
    static native void nativeCommand(String cmd);
    static native String nativeOutput();
    static native float nativeFps();
    static native void nativeSetFrameCap(int fps);

    private static Activity sActivity;
    private static final Handler sHandler = new Handler(Looper.getMainLooper());
    private static boolean sTicking;
    private static int sAppliedCap = -1;
    private static Surface sRateSurface;
    private static float sRateRequested = -1;

    private DevTools() {}

    public static void onResume(Activity a) {
        if (sActivity != a) {
            sActivity = a;
            sAppliedCap = -1;
            sRateSurface = null;
        }
        applyFullscreen();
        if (!sTicking) {
            sTicking = true;
            sHandler.post(TICK);
        }
    }

    public static void onFocus(Activity a, boolean hasFocus) {
        if (hasFocus && a == sActivity) applyFullscreen();
    }

    /** Hides the status and navigation bars (a swipe from an edge shows them briefly). */
    @SuppressWarnings("deprecation")
    private static void applyFullscreen() {
        Window w = sActivity.getWindow();
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = w.getAttributes();
            int mode = Build.VERSION.SDK_INT >= 30
                    ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                    : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            if (lp.layoutInDisplayCutoutMode != mode) {
                lp.layoutInDisplayCutoutMode = mode;
                w.setAttributes(lp);
            }
        }
        if (Build.VERSION.SDK_INT >= 30) {
            w.setDecorFitsSystemWindows(false);
            WindowInsetsController c = w.getInsetsController();
            if (c != null) {
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                c.hide(WindowInsets.Type.systemBars());
            }
        } else {
            w.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    private static final Runnable TICK = new Runnable() {
        @Override public void run() {
            if (sActivity == null || sActivity.isFinishing()) {
                sTicking = false;
                return;
            }
            applyFrameCap();
            sHandler.postDelayed(this, 500);
        }
    };

    private static int gameMode() {
        if (Build.VERSION.SDK_INT < 31) return 0;
        GameManager gm = sActivity.getSystemService(GameManager.class);
        return gm == null ? 0 : gm.getGameMode();
    }

    /** The fps cap for the current Game Mode: 0 = uncapped. */
    private static int effectiveCap() {
        switch (gameMode()) {
            case 2: return 0;   // GAME_MODE_PERFORMANCE
            case 3: return 30;  // GAME_MODE_BATTERY
            default: return 60; // standard, or no Game Mode support
        }
    }

    private static void applyFrameCap() {
        int cap = effectiveCap();
        if (cap != sAppliedCap) {
            sAppliedCap = cap;
            nativeSetFrameCap(cap);
        }
        requestDisplayRate(cap == 0 ? maxRefreshRate() : cap);
    }

    @SuppressWarnings("deprecation")
    private static float maxRefreshRate() {
        float max = 60;
        for (float r : sActivity.getWindowManager().getDefaultDisplay().getSupportedRefreshRates())
            max = Math.max(max, r);
        return max;
    }

    /** Votes for a display rate on the game's surface (re-applied when the surface changes). */
    private static void requestDisplayRate(float fps) {
        if (Build.VERSION.SDK_INT < 30) return;
        SurfaceView v = findSurfaceView(sActivity.getWindow().getDecorView());
        Surface s = v == null ? null : v.getHolder().getSurface();
        if (s == null || !s.isValid()) return;
        if (s == sRateSurface && fps == sRateRequested) return;
        try {
            s.setFrameRate(fps, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
            sRateSurface = s;
            sRateRequested = fps;
        } catch (RuntimeException e) {
            sRateSurface = null;
        }
    }

    private static SurfaceView findSurfaceView(View v) {
        if (v instanceof SurfaceView) return (SurfaceView) v;
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) {
                SurfaceView s = findSurfaceView(g.getChildAt(i));
                if (s != null) return s;
            }
        }
        return null;
    }
}
