// SPDX-License-Identifier: CC0-1.0
package com.doublefine.cq;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.GameManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * Developer tools for the 64-bit port, opened with a three-finger tap:
 *  - the game's Lua debug command line (revived by libcqhost; its UI was stripped from retail),
 *  - an FPS counter overlay,
 *  - the render resolution (16:9 by default: the game's UI is cut off at the screen's 20:9),
 *  - a frame-rate cap, which by default follows Android Game Mode: Performance = uncapped,
 *    Standard = 60 fps, Battery = 30 fps. The rate is also requested from the display with
 *    Surface.setFrameRate, which opts out of Android's default 60 fps cap for games.
 * Game controllers are read here and handed to libcqhost's virtual SDL pad (nativePad); the
 * engine sees it as an SDL game controller.
 * Also keeps the game in immersive full screen (system bars hidden, drawing under the camera
 * cutout). Hooked into CqActivity.onResume, onWindowFocusChanged, dispatchTouchEvent,
 * dispatchKeyEvent and dispatchGenericMotionEvent by build_apk.sh.
 */
public final class DevTools {
    static native void nativeCommand(String cmd);
    static native String nativeOutput();
    static native float nativeFps();
    static native void nativeSetFrameCap(int fps);
    static native void nativePad(int buttons, int lx, int ly, int rx, int ry, int lt, int rt);

    private static final String[] CAP_LABELS = {"Auto (Game Mode)", "Uncapped", "60 fps", "30 fps"};
    private static final int[] CAP_VALUES = {-1, 0, 60, 30};
    private static final int RES_SHARP = -1, RES_FULL = 0;
    private static final String[] RES_LABELS = {
        "16:9, full height (sharp)", "16:9, 1080p", "16:9, 720p (original)", "Full screen (UI is cut off)",
    };
    private static final int[] RES_VALUES = {RES_SHARP, 1080, 720, RES_FULL};
    private static final String[] EXAMPLES = {
        "debug.AcquireAllCostumes()", "debug.GetAllStamps()", "debug.GetAllTricks()",
        "debug.ToggleSkipCutscenes()", "cq.WinBattle()", "app.GetCodeBuildVersion()",
    };

    private static Activity sActivity;
    private static SharedPreferences sPrefs;
    private static final Handler sHandler = new Handler(Looper.getMainLooper());
    private static TextView sFpsView;
    private static boolean sTicking;
    private static int sAppliedCap = -1;
    private static Surface sRateSurface;
    private static float sRateRequested = -1;
    private static long sLastMenu;
    private static String sLastCommand = "";

    private DevTools() {}

    /**
     * Called from CqActivity.restrictTo720p() during onCreate, before onResume. The game lays its
     * UI out for the render buffer's shape, and on a 20:9 screen the full-size buffer pushes menus
     * and HUD off the edges. A 16:9 buffer (stretched to the screen like the retail 720p cap)
     * keeps the UI intact, so the default is 16:9 at the display's full height.
     */
    @SuppressWarnings("deprecation")
    public static boolean force720(Activity a) {
        int h = a.getSharedPreferences("devtools", Context.MODE_PRIVATE).getInt("res", RES_SHARP);
        if (h == RES_FULL) return false;
        if (h == RES_SHARP) {
            android.graphics.Point p = new android.graphics.Point();
            a.getWindowManager().getDefaultDisplay().getRealSize(p);
            h = Math.min(p.x, p.y);
        }
        try {
            Class.forName("org.libsdl.app.SDLActivity")
                    .getMethod("SetMaxScreenResolution", int.class, int.class)
                    .invoke(null, h * 16 / 9, h);
        } catch (ReflectiveOperationException e) {
            android.util.Log.e("DevTools", "SetMaxScreenResolution", e);
        }
        return false;  // the cap is set; skip the game's own 1280x720 one
    }

    public static void onResume(Activity a) {
        if (sActivity != a) {
            sActivity = a;
            sPrefs = a.getSharedPreferences("devtools", Context.MODE_PRIVATE);
            sFpsView = null;
            sAppliedCap = -1;
            sRateSurface = null;
        }
        applyFullscreen();
        updateFpsView();
        watchControllers();
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

    /** Called for every touch event before the game sees it. */
    public static void onTouch(MotionEvent ev) {
        if (ev.getActionMasked() == MotionEvent.ACTION_POINTER_DOWN && ev.getPointerCount() == 3) {
            long now = System.currentTimeMillis();
            if (now - sLastMenu > 1000) {
                sLastMenu = now;
                sHandler.post(DevTools::showMenu);
            }
        }
    }

    // ---------------------------------------------------------------- game controllers

    // Virtual pad button bits, in SDL game controller order (see libcqhost gamepad.cpp).
    private static final int PAD_A = 0, PAD_B = 1, PAD_X = 2, PAD_Y = 3, PAD_BACK = 4, PAD_GUIDE = 5,
            PAD_START = 6, PAD_LS = 7, PAD_RS = 8, PAD_LB = 9, PAD_RB = 10,
            PAD_UP = 11, PAD_DOWN = 12, PAD_LEFT = 13, PAD_RIGHT = 14;
    private static final int DPAD_BITS = (1 << PAD_UP) | (1 << PAD_DOWN) | (1 << PAD_LEFT) | (1 << PAD_RIGHT);
    private static int sPadKeys;     // buttons held, from key events
    private static int sPadHat;      // d-pad from the hat axes
    private static boolean sL2Key, sR2Key;  // digital triggers
    private static float sLx, sLy, sRx, sRy, sLt, sRt;

    // The UI shows its PC/console menu highlight and controller button prompts only when
    // input.IsTouchMode() is false / input.IsGamepadMode() is true, which the engine reports only
    // after gamepad input, and menus check once when built. So while a controller is connected,
    // report gamepad mode from the start.
    private static final String TOUCH_MODE_OVERRIDE =
            "if input and not _cqIsTouchMode then "
            + "_cqIsTouchMode = input.IsTouchMode; _cqIsGamepadMode = input.IsGamepadMode; "
            + "input.IsTouchMode = function() if _cqPad then return false end return _cqIsTouchMode() end; "
            + "input.IsGamepadMode = function() if _cqPad then return true end return _cqIsGamepadMode() end end";
    private static boolean sWatching;
    private static Boolean sPadReported;

    private static void watchControllers() {
        if (!sWatching) {
            sWatching = true;
            nativeCommand(TOUCH_MODE_OVERRIDE);  // queued until the game's Lua console exists
            sActivity.getSystemService(InputManager.class).registerInputDeviceListener(
                    new InputManager.InputDeviceListener() {
                        @Override public void onInputDeviceAdded(int id) { reportControllers(); }
                        @Override public void onInputDeviceRemoved(int id) { reportControllers(); }
                        @Override public void onInputDeviceChanged(int id) { reportControllers(); }
                    }, sHandler);
        }
        reportControllers();
    }

    private static void reportControllers() {
        boolean any = false;
        for (int id : InputDevice.getDeviceIds()) any |= isController(InputDevice.getDevice(id));
        if (sPadReported == null || sPadReported != any) {
            sPadReported = any;
            nativeCommand("_cqPad = " + any);
        }
    }

    private static boolean isController(InputDevice d) {
        if (d == null || d.isVirtual()) return false;
        int s = d.getSources();
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private static int padButton(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: return PAD_A;
            case KeyEvent.KEYCODE_BUTTON_B: return PAD_B;
            case KeyEvent.KEYCODE_BUTTON_X: return PAD_X;
            case KeyEvent.KEYCODE_BUTTON_Y: return PAD_Y;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return PAD_BACK;
            case KeyEvent.KEYCODE_BUTTON_MODE: return PAD_GUIDE;
            case KeyEvent.KEYCODE_BUTTON_START: return PAD_START;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return PAD_LS;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return PAD_RS;
            case KeyEvent.KEYCODE_BUTTON_L1: return PAD_LB;
            case KeyEvent.KEYCODE_BUTTON_R1: return PAD_RB;
            case KeyEvent.KEYCODE_DPAD_UP: return PAD_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return PAD_DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT: return PAD_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return PAD_RIGHT;
            default: return -1;
        }
    }

    /** Called for every key event; returns true for controller input, which the game must not see as keys. */
    public static boolean onKey(KeyEvent ev) {
        if (!isController(ev.getDevice())) return false;
        int code = ev.getKeyCode();
        boolean down = ev.getAction() == KeyEvent.ACTION_DOWN;
        if (ev.getAction() != KeyEvent.ACTION_DOWN && ev.getAction() != KeyEvent.ACTION_UP) return false;
        if (code == KeyEvent.KEYCODE_BUTTON_L2) {
            sL2Key = down;
        } else if (code == KeyEvent.KEYCODE_BUTTON_R2) {
            sR2Key = down;
        } else {
            int b = padButton(code);
            if (b < 0) return false;  // e.g. BACK, volume: leave to the system
            sPadKeys = down ? sPadKeys | (1 << b) : sPadKeys & ~(1 << b);
        }
        sendPad();
        return true;
    }

    /** Called for every generic motion event; returns true for controller sticks, triggers and hats. */
    public static boolean onMotion(MotionEvent ev) {
        if ((ev.getSource() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK
                || ev.getActionMasked() != MotionEvent.ACTION_MOVE || !isController(ev.getDevice())) {
            return false;
        }
        InputDevice d = ev.getDevice();
        sLx = ev.getAxisValue(MotionEvent.AXIS_X);
        sLy = ev.getAxisValue(MotionEvent.AXIS_Y);
        boolean zrz = d.getMotionRange(MotionEvent.AXIS_Z, ev.getSource()) != null;  // most pads; some use RX/RY
        sRx = ev.getAxisValue(zrz ? MotionEvent.AXIS_Z : MotionEvent.AXIS_RX);
        sRy = ev.getAxisValue(zrz ? MotionEvent.AXIS_RZ : MotionEvent.AXIS_RY);
        sLt = Math.max(ev.getAxisValue(MotionEvent.AXIS_LTRIGGER), ev.getAxisValue(MotionEvent.AXIS_BRAKE));
        sRt = Math.max(ev.getAxisValue(MotionEvent.AXIS_RTRIGGER), ev.getAxisValue(MotionEvent.AXIS_GAS));
        float hx = ev.getAxisValue(MotionEvent.AXIS_HAT_X), hy = ev.getAxisValue(MotionEvent.AXIS_HAT_Y);
        sPadHat = (hx < -0.5f ? 1 << PAD_LEFT : 0) | (hx > 0.5f ? 1 << PAD_RIGHT : 0)
                | (hy < -0.5f ? 1 << PAD_UP : 0) | (hy > 0.5f ? 1 << PAD_DOWN : 0);
        sendPad();
        return true;
    }

    private static int stick(float v) {
        return Math.max(-32768, Math.min(32767, Math.round(v * 32767)));
    }

    private static int trigger(float v, boolean key) {
        return key ? 32767 : Math.max(0, Math.min(32767, Math.round(v * 32767)));
    }

    private static void sendPad() {
        nativePad(sPadKeys | (sPadHat & DPAD_BITS), stick(sLx), stick(sLy), stick(sRx), stick(sRy),
                trigger(sLt, sL2Key), trigger(sRt, sR2Key));
    }

    private static final Runnable TICK = new Runnable() {
        @Override public void run() {
            if (sActivity == null || sActivity.isFinishing()) {
                sTicking = false;
                return;
            }
            if (sFpsView != null && sFpsView.getVisibility() == View.VISIBLE) {
                sFpsView.setText(String.format("%.0f FPS", nativeFps()));
            }
            applyFrameCap();
            // adb: run-as com.doublefine.cq touch files/devmenu (or files/devconsole)
            java.io.File menu = new java.io.File(sActivity.getFilesDir(), "devmenu");
            java.io.File console = new java.io.File(sActivity.getFilesDir(), "devconsole");
            if (menu.delete()) showMenu();
            if (console.delete()) showConsole();
            sHandler.postDelayed(this, 500);
        }
    };

    // ---------------------------------------------------------------- frame cap / Game Mode

    private static int gameMode() {
        if (Build.VERSION.SDK_INT < 31) return 0;
        GameManager gm = sActivity.getSystemService(GameManager.class);
        return gm == null ? 0 : gm.getGameMode();
    }

    private static String gameModeName(int m) {
        switch (m) {
            case 1: return "Standard";
            case 2: return "Performance";
            case 3: return "Battery";
            default: return "unsupported";
        }
    }

    /** The fps cap in effect: 0 = uncapped. */
    private static int effectiveCap() {
        int cap = sPrefs.getInt("cap", -1);
        if (cap >= 0) return cap;
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

    private static float maxRefreshRate() {
        float max = 60;
        for (float r : sActivity.getWindowManager().getDefaultDisplay().getSupportedRefreshRates())
            max = Math.max(max, r);
        return max;
    }

    /** Votes for a display rate on the game's surface (re-applied when the surface changes). */
    private static void requestDisplayRate(float fps) {
        if (Build.VERSION.SDK_INT < 30) return;
        Surface s = gameSurface();
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

    private static Surface gameSurface() {
        try {
            java.lang.reflect.Field f = Class.forName("org.libsdl.app.SDLActivity").getDeclaredField("mSurface");
            f.setAccessible(true);
            SurfaceView v = (SurfaceView) f.get(null);
            return v == null ? null : v.getHolder().getSurface();
        } catch (ReflectiveOperationException e) {
            return null;
        }
    }

    // ---------------------------------------------------------------- FPS overlay

    private static void updateFpsView() {
        boolean on = sPrefs.getBoolean("fps", false);
        if (on && sFpsView == null) {
            TextView v = new TextView(sActivity);
            v.setTextColor(Color.rgb(0x7c, 0xff, 0x7c));
            v.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
            v.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
            v.setBackgroundColor(Color.argb(0x90, 0, 0, 0));
            int pad = dp(4);
            v.setPadding(pad * 2, pad, pad * 2, pad);
            FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                    Gravity.TOP | Gravity.END);  // the camera cutout is on the left in landscape
            lp.setMargins(0, dp(8), dp(8), 0);
            v.setText("-- FPS");
            sActivity.addContentView(v, lp);
            sFpsView = v;
        }
        if (sFpsView != null) sFpsView.setVisibility(on ? View.VISIBLE : View.GONE);
    }

    private static int dp(int v) {
        return Math.round(v * sActivity.getResources().getDisplayMetrics().density);
    }

    // ---------------------------------------------------------------- menus

    private static void showMenu() {
        if (sActivity == null || sActivity.isFinishing()) return;
        boolean fps = sPrefs.getBoolean("fps", false);
        int capIndex = capIndex();
        String capText = CAP_LABELS[capIndex];
        if (CAP_VALUES[capIndex] < 0) {
            int c = effectiveCap();
            capText += " - " + gameModeName(gameMode()) + ": " + (c == 0 ? "uncapped" : c + " fps");
        }
        String[] items = {
            "FPS counter: " + (fps ? "ON" : "OFF"),
            "Debug command line…",
            "Frame rate cap: " + capText,
            "Resolution: " + RES_LABELS[resIndex()],
        };
        dialog().setTitle("Costume Quest dev tools")
                .setItems(items, (d, which) -> {
                    if (which == 0) {
                        sPrefs.edit().putBoolean("fps", !fps).apply();
                        updateFpsView();
                    } else if (which == 1) {
                        showConsole();
                    } else if (which == 2) {
                        showCapPicker();
                    } else {
                        showResPicker();
                    }
                })
                .setNegativeButton("Close", null)
                .show();
    }

    private static int capIndex() {
        int cap = sPrefs.getInt("cap", -1);
        for (int i = 0; i < CAP_VALUES.length; i++) if (CAP_VALUES[i] == cap) return i;
        return 0;
    }

    private static void showCapPicker() {
        dialog().setTitle("Frame rate cap")
                .setSingleChoiceItems(CAP_LABELS, capIndex(), (d, which) -> {
                    sPrefs.edit().putInt("cap", CAP_VALUES[which]).apply();
                    applyFrameCap();
                    d.dismiss();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private static int resIndex() {
        int h = sPrefs.getInt("res", RES_SHARP);
        for (int i = 0; i < RES_VALUES.length; i++) if (RES_VALUES[i] == h) return i;
        return 0;
    }

    private static void showResPicker() {
        int cur = resIndex();
        dialog().setTitle("Render resolution")
                .setSingleChoiceItems(RES_LABELS, cur, (d, which) -> {
                    d.dismiss();
                    if (which == cur) return;
                    sPrefs.edit().putInt("res", RES_VALUES[which]).commit();
                    showRestartPrompt();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    /** The resolution cap is applied once at startup, so changing it needs a relaunch. */
    private static void showRestartPrompt() {
        dialog().setTitle("Restart required")
                .setMessage("The resolution change takes effect the next time the game starts. "
                        + "Unsaved progress is lost when restarting now.")
                .setPositiveButton("Restart now", (d, w) -> restart())
                .setNegativeButton("Later", null)
                .show();
    }

    /** Relaunches from the foreground (a delayed background launch is blocked), then exits. */
    private static void restart() {
        Intent i = sActivity.getPackageManager().getLaunchIntentForPackage(sActivity.getPackageName());
        if (i == null) return;
        sActivity.startActivity(Intent.makeRestartActivityTask(i.getComponent()));
        Runtime.getRuntime().exit(0);
    }

    private static void showConsole() {
        LinearLayout root = new LinearLayout(sActivity);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(12);
        root.setPadding(pad, pad / 2, pad, 0);

        final TextView out = new TextView(sActivity);
        out.setTypeface(Typeface.MONOSPACE);
        out.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        out.setTextIsSelectable(true);
        final ScrollView scroll = new ScrollView(sActivity);
        scroll.addView(out);
        root.addView(scroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        LinearLayout row = new LinearLayout(sActivity);
        row.setOrientation(LinearLayout.HORIZONTAL);
        final EditText input = new EditText(sActivity);
        input.setTypeface(Typeface.MONOSPACE);
        input.setSingleLine(true);
        input.setHint("Lua, e.g. debug.GetAllStamps()");
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        input.setImeOptions(EditorInfo.IME_ACTION_GO | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        input.setText(sLastCommand);
        row.addView(input, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        Button run = new Button(sActivity);
        run.setText("Run");
        row.addView(run);
        Button examples = new Button(sActivity);
        examples.setText("…");
        row.addView(examples);
        root.addView(row);

        final Runnable refresh = new Runnable() {
            @Override public void run() {
                String text = nativeOutput();
                if (text.isEmpty()) text = "Lua 5.1 console. Expressions print their value; "
                        + "print() works. Tables: debug, cq, app, game, appconfig.\n";
                out.setText(text);
                scroll.post(() -> scroll.fullScroll(View.FOCUS_DOWN));
            }
        };
        final Runnable submit = () -> {
            String cmd = input.getText().toString().trim();
            if (cmd.isEmpty()) return;
            sLastCommand = cmd;
            nativeCommand(cmd);
            input.setText("");
            // Commands run on the game thread at its next frame.
            sHandler.postDelayed(refresh, 150);
            sHandler.postDelayed(refresh, 600);
        };
        run.setOnClickListener(v -> submit.run());
        input.setOnEditorActionListener((v, actionId, ev) -> {
            if (actionId == EditorInfo.IME_ACTION_GO
                    || (ev != null && ev.getKeyCode() == KeyEvent.KEYCODE_ENTER
                        && ev.getAction() == KeyEvent.ACTION_DOWN)) {
                submit.run();
                return true;
            }
            return false;
        });
        examples.setOnClickListener(v -> dialog().setTitle("Examples")
                .setItems(EXAMPLES, (d, which) -> {
                    input.setText(EXAMPLES[which]);
                    input.setSelection(input.length());
                })
                .show());

        AlertDialog d = dialog().setTitle("Debug command line")
                .setView(root)
                .setNegativeButton("Close", null)
                .create();
        d.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE
                | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        d.show();
        d.getWindow().setLayout(
                (int) (sActivity.getResources().getDisplayMetrics().widthPixels * 0.9),
                (int) (sActivity.getResources().getDisplayMetrics().heightPixels * 0.9));
        refresh.run();
        input.requestFocus();
    }

    private static AlertDialog.Builder dialog() {
        return new AlertDialog.Builder(sActivity, android.R.style.Theme_Material_Dialog_Alert);
    }
}
