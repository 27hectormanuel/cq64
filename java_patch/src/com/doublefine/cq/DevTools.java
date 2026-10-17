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
 * Developer tools for the 64-bit port, opened with a three-finger tap or LB + RB + A:
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
    static native String nativeCommandList();  // functions the game's Lua exposes (LIST_LUA)
    static native String nativeCommandHints();  // what they take (luahints.cpp); slow first call
    static native void nativePad(int buttons, int lx, int ly, int rx, int ry, int lt, int rt);
    static native void nativePadIcons(int set);  // button icons: 0 Xbox, 1 PlayStation

    private static final String[] CAP_LABELS = {"Auto (Game Mode)", "Uncapped", "60 fps", "30 fps"};
    private static final int[] CAP_VALUES = {-1, 0, 60, 30};
    private static final int RES_SHARP = -1, RES_FULL = 0;
    private static final String[] RES_LABELS = {
        "16:9, full height (sharp)", "16:9, 1080p", "16:9, 720p (original)", "Full screen (UI is cut off)",
    };
    private static final int[] RES_VALUES = {RES_SHARP, 1080, 720, RES_FULL};
    // Commands at the top of the console's list (label, Lua).
    private static final String[][] COMMANDS = {
        {"Win battle", "cq.WinBattle()"},
        {"Start a battle", "cq.StartBattle()"},
        {"Get all costumes", "debug.AcquireAllCostumes()"},
        {"Get all battle stamps", "debug.GetAllStamps()"},
        {"Get all tricks", "debug.GetAllTricks()"},
        {"Toggle skip cutscenes", "debug.ToggleSkipCutscenes()"},
        {"Complete trick-or-treating", "debug.CompleteTrickOrTreating()"},
        {"Kill monster encounters", "debug.KillMonsterEncounters()"},
        {"Teleport to open world", "debug.TeleportToOpenWorld()"},
        {"Game build version", "app.GetCodeBuildVersion()"},
    };
    // Lua the console runs once to list every function the game's Lua tables expose (standard
    // libraries left out), one "table.name" per line, via hostlist() (devtools.cpp).
    private static final String LIST_LUA = "\u0001"
            + "local skip = {string=1, table=1, math=1, io=1, os=1, coroutine=1, package=1, _G=1, bit=1, jit=1} "
            + "local o = {} "
            + "for n, m in pairs(_G) do if type(n) == 'string' and type(m) == 'table' and not skip[n] then "
            + "pcall(function() for k, v in pairs(m) do "
            + "if type(k) == 'string' and type(v) == 'function' then o[#o + 1] = n .. '.' .. k end end end) "
            + "end end "
            + "table.sort(o) hostlist(table.concat(o, '\\n'))";
    private static String[] sLuaCommands;  // from LIST_LUA, once per run
    private static java.util.Map<String, Call> sCalls;  // argument hints by "table.name", once per run

    /** How to call a command: the list's signature and, if it takes arguments, a call to edit. */
    private static final class Call {
        final String signature, template;  // template null: run as "name()"
        Call(String signature, String template) { this.signature = signature; this.template = template; }
    }

    /**
     * Turns nativeCommandHints() (luahints.cpp) into a Call for each listed command. Preference:
     * a literal call from the game's scripts, the script's parameter list, then the engine
     * function's argument types (a boolean setter with a matching getter becomes a toggle).
     */
    private static java.util.Map<String, Call> parseHints(String hints, String[] names) {
        java.util.Map<String, String[]> engine = new java.util.HashMap<>();  // short name -> lo, hi, types
        java.util.Map<String, String> params = new java.util.HashMap<>();
        java.util.Map<String, String> example = new java.util.HashMap<>();
        for (String line : hints.split("\n")) {
            String[] f = line.split("\t", -1);
            if (f[0].equals("C") && f.length >= 5) engine.putIfAbsent(f[1], new String[] {f[2], f[3], f[4]});
            else if (f[0].equals("S") && f.length >= 3) params.put(f[1], f[2]);
            else if (f[0].equals("E") && f.length >= 3) example.putIfAbsent(f[1], f[2]);
        }
        java.util.Set<String> all = new java.util.HashSet<>(java.util.Arrays.asList(names));
        java.util.Map<String, Call> out = new java.util.HashMap<>();
        for (String full : names) {
            String table = full.substring(0, full.indexOf('.')), name = full.substring(full.indexOf('.') + 1);
            String ex = example.get(full), p = params.get(full);
            String[] e = engine.get(name);
            if (ex != null) {
                out.put(full, new Call(p != null && !p.isEmpty() ? p : ex, full + "(" + ex + ")"));
            } else if (p != null) {
                out.put(full, new Call(p, p.isEmpty() ? null : full + "(" + p + ")"));
            } else if (e != null) {
                int lo = Integer.parseInt(e[0]), hi = Integer.parseInt(e[1]);
                String[] types = e[2].isEmpty() ? new String[0] : e[2].split(",");
                int n = Math.max(lo, types.length);
                if (hi == 0 || n == 0) {
                    out.put(full, new Call("", null));
                    continue;
                }
                StringBuilder sig = new StringBuilder(), args = new StringBuilder();
                for (int i = 0; i < n; i++) {
                    String t = i < types.length ? types[i] : "any";
                    if (i > 0) { sig.append(", "); args.append(", "); }
                    sig.append(t);
                    if (lo >= 0 && i >= lo) sig.append('?');
                    args.append(placeholder(t, table, name, i, all));
                }
                out.put(full, new Call(sig.toString(), full + "(" + args + ")"));
            }
        }
        return out;
    }

    /** An argument to start from: for "SetX(bool)", the opposite of the matching getter. */
    private static String placeholder(String type, String table, String name, int index,
                                      java.util.Set<String> all) {
        switch (type) {
            case "bool":
                if (index == 0 && name.startsWith("Set")) {
                    String x = name.substring(3);
                    for (String g : new String[] {"Get" + x, "Is" + x, "Is" + x + "Enabled", "Get" + x + "Enabled"})
                        if (all.contains(table + "." + g)) return "not " + table + "." + g + "()";
                }
                return "true";
            case "number": return "1";
            case "string": case "enum": case "mission": case "domain": return "''";
            case "resource": return "RESOURCE('')";
            default: return "nil";
        }
    }

    private static Activity sActivity;
    private static SharedPreferences sPrefs;
    private static final Handler sHandler = new Handler(Looper.getMainLooper());
    private static TextView sFpsView;
    private static boolean sTicking;
    private static int sAppliedCap = -1;
    private static Surface sRateSurface;
    private static float sRateRequested = -1;
    private static long sLastMenu;

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
        if (ev.getActionMasked() == MotionEvent.ACTION_DOWN && !sTouched) {
            sTouched = true;  // controller mode off until the next controller input
            reportControllers();
        }
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
    // Controller mode follows the input used last: on while a controller is connected, off after a
    // touch until the next controller input. Reporting it for as long as a controller was merely
    // connected left the battle HUD's ability button on Y after switching to touch.
    private static boolean sTouched;

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
        InputDevice first = null, last = null;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (!isController(d)) continue;
            if (first == null) first = d;
            if (id == sLastPadId) last = d;
        }
        boolean any = first != null && !sTouched;
        if (sPadReported == null || sPadReported != any) {
            sPadReported = any;
            nativeCommand("_cqPad = " + any);
        }
        if (any) reportIcons(last != null ? last : first);
    }

    // Button icons follow the controller last used (or the first connected one): PlayStation icons
    // for Sony controllers (DualShock 3/4, DualSense), Xbox icons for everything else.
    private static final int VENDOR_SONY = 0x054c;
    private static int sLastPadId = -1, sIconsReported = -1;

    private static void reportIcons(InputDevice d) {
        int set = d.getVendorId() == VENDOR_SONY ? 1 : 0;
        if (set != sIconsReported) {
            sIconsReported = set;
            nativePadIcons(set);
        }
    }

    private static void padUsed(InputDevice d) {
        if (sTouched) {
            sTouched = false;
            reportControllers();
        }
        if (d.getId() != sLastPadId) {
            sLastPadId = d.getId();
            reportIcons(d);
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
        padUsed(ev.getDevice());
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
            int shoulders = (1 << PAD_LB) | (1 << PAD_RB);
            if (b == PAD_A && down && (sPadKeys & shoulders) == shoulders) {
                openMenuFromPad();  // LB + RB + A: the developer menu, like a three-finger tap
                return true;
            }
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
        padUsed(d);
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

    /** LB + RB + A: opens the developer menu and lets go of the held buttons in the game. */
    private static void openMenuFromPad() {
        sPadKeys = 0;
        sPadHat = 0;
        sL2Key = sR2Key = false;
        sLx = sLy = sRx = sRy = sLt = sRt = 0;
        sendPad();
        long now = System.currentTimeMillis();
        if (now - sLastMenu > 1000) {
            sLastMenu = now;
            sHandler.post(DevTools::showMenu);
        }
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
            // A rate the display runs at is paced by its vsync alone. libcqhost's own sleep
            // before the swap isn't aligned with vsync and makes about 1 frame in 6 miss it
            // (33 ms instead of 16.7), so it's only for rates the display doesn't offer.
            nativeSetFrameCap(displayOffers(cap) ? 0 : cap);
        }
        requestDisplayRate(cap == 0 ? maxRefreshRate() : cap);
    }

    private static boolean displayOffers(int fps) {
        if (fps <= 0 || Build.VERSION.SDK_INT < 30) return false;
        for (float r : sActivity.getWindowManager().getDefaultDisplay().getSupportedRefreshRates())
            if (Math.abs(r - fps) < 0.5f) return true;
        return false;
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
        String[] items = {
            "FPS counter: " + (fps ? "ON" : "OFF"),
            "Debug command line…",
            "FPS & Performance…",
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
                        showPerformanceMenu();
                    } else {
                        showResPicker();
                    }
                })
                .setNegativeButton("Close", null)
                .show();
    }

    /** FPS & Performance: the frame rate cap, the simulation rate and the performance boost. */
    private static void showPerformanceMenu() {
        int capIndex = capIndex();
        String capText = CAP_LABELS[capIndex];
        if (CAP_VALUES[capIndex] < 0) {
            int c = effectiveCap();
            capText += " - " + gameModeName(gameMode()) + ": " + (c == 0 ? "uncapped" : c + " fps");
        }
        String[] items = {
            "Frame rate cap: " + capText,
            "High frame rate: " + simRateLabel(simRate()),
            "Performance boost: " + (boostOn() ? "ON" : "OFF"),
        };
        dialog().setTitle("FPS & Performance")
                .setItems(items, (d, which) -> {
                    if (which == 0) {
                        showCapPicker();
                    } else if (which == 1) {
                        showSimRatePicker();
                    } else {
                        setBoost(!boostOn());
                        showRestartPrompt();
                    }
                })
                .setNegativeButton("Back", (d, w) -> showMenu())
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
    // ---------------------------------------------------------------- simulation rate

    // The game world (movement, animation, battles) advances in fixed simulation ticks, 30 a
    // second, however fast frames are drawn. libcqhost (simrate.cpp) can run it at 60 or 120 Hz,
    // as Costume Quest 2's "60 FPS" option does; files/simrate holds the rate, read at startup.
    private static final int[] SIM_RATES = {30, 60, 120};

    private static int simRate() {
        try (java.io.BufferedReader r = new java.io.BufferedReader(
                new java.io.FileReader(new java.io.File(sActivity.getFilesDir(), "simrate")))) {
            int hz = Integer.parseInt(r.readLine().trim());
            return hz == 60 || hz == 120 ? hz : 30;
        } catch (Exception e) {
            return 30;
        }
    }

    private static String simRateLabel(int hz) {
        switch (hz) {
            case 60: return "60 fps (like Costume Quest 2)";
            case 120: return "120 fps (experimental)";
            default: return "OFF (30 fps, original)";
        }
    }

    private static void showSimRatePicker() {
        int cur = simRate();
        String[] labels = new String[SIM_RATES.length];
        int checked = 0;
        for (int i = 0; i < SIM_RATES.length; i++) {
            labels[i] = simRateLabel(SIM_RATES[i]);
            if (SIM_RATES[i] == cur) checked = i;
        }
        dialog().setTitle("High frame rate (world, battles)")
                .setSingleChoiceItems(labels, checked, (d, which) -> {
                    d.dismiss();
                    int hz = SIM_RATES[which];
                    if (hz == cur) return;
                    java.io.File f = new java.io.File(sActivity.getFilesDir(), "simrate");
                    if (hz == 30) {
                        f.delete();
                    } else {
                        try (java.io.FileWriter w = new java.io.FileWriter(f)) {
                            w.write(hz + "\n");
                        } catch (java.io.IOException e) {
                            return;
                        }
                    }
                    showRestartPrompt();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    // Performance boost (libcqhost, read at startup): the engine's simulation and scene graph
    // thread pools get 2 workers each, as in Costume Quest 2 on PC (files/workers), and the
    // game's busiest thread is kept on the fastest core with Android performance hints asking
    // for 60 fps clocks, and the render thread no longer waits for the GPU each frame
    // (files/affinity).
    private static final String[] BOOST_FILES = {"workers", "affinity"};

    private static boolean boostOn() {
        for (String n : BOOST_FILES)
            if (!new java.io.File(sActivity.getFilesDir(), n).exists()) return false;
        return true;
    }

    private static void setBoost(boolean on) {
        for (String n : BOOST_FILES) {
            java.io.File f = new java.io.File(sActivity.getFilesDir(), n);
            try {
                if (on) f.createNewFile();
                else f.delete();
            } catch (java.io.IOException e) {
                // left as it was; the menu shows the state
            }
        }
    }

    private static void showRestartPrompt() {
        dialog().setTitle("Restart required")
                .setMessage("The change takes effect the next time the game starts. "
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

    /** A row of the console's command list: a section header (cmd null) or a command. */
    private static final class Entry {
        final String text, cmd;
        Entry(String text, String cmd) { this.text = text; this.cmd = cmd; }
        @Override public String toString() { return text; }
    }

    /**
     * Debug console: output on the left; on the right the list of every command the game's Lua
     * exposes (handy ones first, then a section per table). Touch: tap a command to select it,
     * tap again to run it; long press copies it into the text box. Controller: d-pad moves, A
     * runs, B closes, X shows / hides the text box, L1 / R1 jump between sections. The text box
     * (keyboard button) is hidden at first; typing in it filters the list, Enter runs the text.
     */
    private static void showConsole() {
        final AlertDialog[] dlg = new AlertDialog[1];
        LinearLayout root = new LinearLayout(sActivity);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(12);
        root.setPadding(pad, pad / 2, pad, pad / 2);

        LinearLayout panes = new LinearLayout(sActivity);
        panes.setOrientation(LinearLayout.HORIZONTAL);
        final TextView out = new TextView(sActivity);
        out.setTypeface(Typeface.MONOSPACE);
        out.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        out.setTextIsSelectable(true);
        final ScrollView scroll = new ScrollView(sActivity);
        scroll.setFocusable(false);
        scroll.addView(out);
        panes.addView(scroll, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 3f));

        final java.util.ArrayList<Entry> shown = new java.util.ArrayList<>();
        final android.widget.ListView list = new android.widget.ListView(sActivity);
        list.setChoiceMode(android.widget.AbsListView.CHOICE_MODE_SINGLE);
        list.setFastScrollEnabled(true);
        // Visible highlights (the dialog theme's are nearly invisible): the controller / key
        // selection, and the row a first tap selected.
        list.setSelector(new android.graphics.drawable.ColorDrawable(0x80FFC107));
        list.setDrawSelectorOnTop(true);
        final android.widget.ArrayAdapter<Entry> adapter = new android.widget.ArrayAdapter<Entry>(
                sActivity, android.R.layout.simple_list_item_activated_1, shown) {
            @Override public boolean areAllItemsEnabled() { return false; }
            @Override public boolean isEnabled(int i) { return getItem(i).cmd != null; }
            @Override public View getView(int i, View convert, ViewGroup parent) {
                TextView v = (TextView) super.getView(i, convert, parent);
                boolean header = getItem(i).cmd == null;
                v.setTypeface(header ? Typeface.DEFAULT_BOLD : Typeface.MONOSPACE);
                v.setTextSize(TypedValue.COMPLEX_UNIT_SP, header ? 14 : 12);
                v.setTextColor(header ? 0xFFFFC107 : 0xFFFFFFFF);
                v.setMinHeight(dp(header ? 32 : 40));
                android.graphics.drawable.StateListDrawable bg = new android.graphics.drawable.StateListDrawable();
                bg.addState(new int[] {android.R.attr.state_activated},
                        new android.graphics.drawable.ColorDrawable(0x60FFC107));
                bg.addState(new int[0], new android.graphics.drawable.ColorDrawable(0));
                v.setBackground(bg);
                return v;
            }
        };
        list.setAdapter(adapter);
        LinearLayout.LayoutParams listParams =
                new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 2f);
        listParams.leftMargin = dp(8);
        panes.addView(list, listParams);
        root.addView(panes, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        final LinearLayout inputRow = new LinearLayout(sActivity);
        inputRow.setOrientation(LinearLayout.HORIZONTAL);
        inputRow.setVisibility(View.GONE);
        final EditText input = new EditText(sActivity);
        input.setTypeface(Typeface.MONOSPACE);
        input.setSingleLine(true);
        input.setHint("Lua, or text to filter the list");
        // Visible-password: keyboards then skip autocorrect and the auto space after ".".
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        input.setImeOptions(EditorInfo.IME_ACTION_GO | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        inputRow.addView(input, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        Button run = new Button(sActivity);
        run.setText("Run");
        inputRow.addView(run);
        root.addView(inputRow);

        LinearLayout bar = new LinearLayout(sActivity);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        final Button keyboard = new Button(sActivity);
        keyboard.setText("⌨");  // keyboard symbol
        keyboard.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22);
        keyboard.setContentDescription("Type a command");
        bar.addView(keyboard);
        final TextView hint = new TextView(sActivity);
        hint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        hint.setPadding(dp(8), 0, dp(8), 0);
        bar.addView(hint, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        Button close = new Button(sActivity);
        close.setText("Close");
        bar.addView(close);
        root.addView(bar);

        final Runnable refresh = new Runnable() {
            @Override public void run() {
                String text = nativeOutput();
                if (text.isEmpty()) text = "Lua 5.1 console. Expressions print their value; "
                        + "print() works.\n";
                out.setText(text);
                scroll.post(() -> scroll.fullScroll(View.FOCUS_DOWN));
            }
        };
        final java.util.function.Consumer<String> runCommand = cmd -> {
            nativeCommand(cmd);
            // Commands run on the game thread at its next frame.
            sHandler.postDelayed(refresh, 150);
            sHandler.postDelayed(refresh, 600);
        };
        // Fills the list: the handy commands, then the game's functions by table, keeping only
        // entries containing the filter text.
        final Runnable fill = () -> {
            String f = input.getText().toString().trim().toLowerCase(java.util.Locale.ROOT);
            if (f.indexOf('(') >= 0) f = "";  // a call being written, not a filter
            shown.clear();
            shown.add(new Entry("Handy", null));
            for (String[] c : COMMANDS)
                if (f.isEmpty() || (c[0] + c[1]).toLowerCase(java.util.Locale.ROOT).contains(f))
                    shown.add(new Entry(c[0] + "  —  " + c[1], c[1]));
            if (sLuaCommands != null) {
                String table = null;
                for (String name : sLuaCommands) {
                    if (!f.isEmpty() && !name.toLowerCase(java.util.Locale.ROOT).contains(f)) continue;
                    String t = name.substring(0, name.indexOf('.'));
                    if (!t.equals(table)) shown.add(new Entry(table = t, null));
                    Call call = sCalls == null ? null : sCalls.get(name);
                    shown.add(new Entry(name + "(" + (call == null ? "" : call.signature) + ")",
                            call != null && call.template != null ? call.template : name + "()"));
                }
            }
            // Drop headers left without commands.
            for (int i = shown.size() - 1; i >= 0; i--)
                if (shown.get(i).cmd == null && (i + 1 == shown.size() || shown.get(i + 1).cmd == null))
                    shown.remove(i);
            list.clearChoices();
            adapter.notifyDataSetChanged();
            hint.setText(sLuaCommands == null ? "Reading the game's commands…"
                    : (shown.size() + " rows · tap to select, tap again to run · "
                       + "pad: A run, X type, L1/R1 sections"));
        };
        if (sLuaCommands == null) {
            nativeCommand(LIST_LUA);  // runs at the game's next frame
            sHandler.postDelayed(new Runnable() {
                int tries;
                @Override public void run() {
                    String l = nativeCommandList();
                    if (!l.isEmpty()) {
                        sLuaCommands = l.split("\n");
                        fill.run();
                        final String[] names = sLuaCommands;
                        new Thread(() -> {  // scans the game's code and scripts: off the UI thread
                            java.util.Map<String, Call> calls = parseHints(nativeCommandHints(), names);
                            sHandler.post(() -> {
                                sCalls = calls;
                                fill.run();
                            });
                        }, "cq-lua-hints").start();
                    } else if (++tries < 50) {
                        sHandler.postDelayed(this, 200);
                    }
                }
            }, 200);
        }
        fill.run();

        // Commands that take arguments are not run as is: their call goes into the text box to
        // edit, with Run focused (A runs it with a controller).
        final java.util.function.Consumer<String> editCommand = cmd -> {
            input.setText(cmd);
            input.setSelection(cmd.indexOf('(') + 1, cmd.length() - 1);
            if (inputRow.getVisibility() != View.VISIBLE) inputRow.setVisibility(View.VISIBLE);
            if (list.isInTouchMode()) {
                input.requestFocus();
                input.post(() -> sActivity.getSystemService(android.view.inputmethod.InputMethodManager.class)
                        .showSoftInput(input, 0));
            } else {
                run.requestFocus();
            }
        };
        list.setOnItemClickListener((parent, v, i, id) -> {
            String cmd = shown.get(i).cmd;
            // Touch: a first tap selects, a second runs. Controller / keys: A runs at once.
            if (!list.isInTouchMode() || list.getCheckedItemPosition() == i) {
                if (cmd.endsWith("()")) runCommand.accept(cmd);
                else editCommand.accept(cmd);
            }
            list.setItemChecked(i, true);
        });
        list.setOnItemLongClickListener((parent, v, i, id) -> {
            input.setText(shown.get(i).cmd);
            input.setSelection(input.length());
            if (inputRow.getVisibility() != View.VISIBLE) keyboard.callOnClick();
            return true;
        });

        final Runnable submit = () -> {
            String cmd = input.getText().toString().trim();
            if (cmd.isEmpty()) return;
            input.setText("");
            runCommand.accept(cmd);
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
        input.addTextChangedListener(new android.text.TextWatcher() {
            @Override public void beforeTextChanged(CharSequence t, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence t, int a, int b, int c) {}
            @Override public void afterTextChanged(android.text.Editable e) { fill.run(); }
        });

        final android.view.inputmethod.InputMethodManager imm =
                sActivity.getSystemService(android.view.inputmethod.InputMethodManager.class);
        keyboard.setOnClickListener(v -> {
            if (inputRow.getVisibility() == View.VISIBLE) {
                imm.hideSoftInputFromWindow(input.getWindowToken(), 0);
                inputRow.setVisibility(View.GONE);
                if (input.length() > 0) input.setText("");  // clear the filter
                list.requestFocus();
            } else {
                inputRow.setVisibility(View.VISIBLE);
                input.requestFocus();
                input.post(() -> imm.showSoftInput(input, 0));
            }
        });
        close.setOnClickListener(v -> dlg[0].dismiss());

        AlertDialog d = dialog().setTitle("Debug command line").setView(root).create();
        dlg[0] = d;
        // Controller shortcuts (B closes through the system's BUTTON_B -> BACK fallback).
        d.setOnKeyListener((di, code, ev) -> {
            if (ev.getAction() != KeyEvent.ACTION_DOWN) return false;
            switch (code) {
                case KeyEvent.KEYCODE_BUTTON_X:
                    keyboard.callOnClick();
                    return true;
                case KeyEvent.KEYCODE_BUTTON_L1:
                case KeyEvent.KEYCODE_BUTTON_R1: {
                    int step = code == KeyEvent.KEYCODE_BUTTON_R1 ? 1 : -1;
                    int i = list.getSelectedItemPosition();
                    if (i < 0) i = list.getFirstVisiblePosition();
                    if (i < 0 || i >= shown.size()) return true;
                    // Find the previous/next section header, then select its first command.
                    if (step < 0) {
                        while (i > 0 && shown.get(i).cmd != null) i--;  // this section's header
                        i--;
                    } else {
                        i++;
                    }
                    while (i >= 0 && i < shown.size() && shown.get(i).cmd != null) i += step;
                    if (i < 0) i = 0;
                    if (i < shown.size() - 1) {
                        list.requestFocus();
                        list.setSelection(i + 1);  // the section's first command
                    }
                    return true;
                }
                default:
                    return false;
            }
        });
        d.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN
                | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        d.show();
        // Full height, so the dialog shrinks to the space above the keyboard (adjustResize) and
        // the text box stays visible while typing.
        d.getWindow().setLayout(
                (int) (sActivity.getResources().getDisplayMetrics().widthPixels * 0.9),
                ViewGroup.LayoutParams.MATCH_PARENT);
        refresh.run();
        list.requestFocus();
    }

    private static AlertDialog.Builder dialog() {
        return new AlertDialog.Builder(sActivity, android.R.style.Theme_Material_Dialog_Alert);
    }
}
