package com.dbz3.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * On-screen gamepad drawn over the SDL surface. Each button presses a keyboard
 * key that the runtime's mouse-and-keyboard driver (mnk_input_driver.cpp)
 * already maps to an Xbox 360 control:
 *   A=Space  B=Backspace  X=L  Y=P  LB=1  RB=3  LT=Q  RT=E
 *   left stick=W/A/S/D  Start=Enter  Back=Tab
 * Labels use PlayStation glyphs (A=✕, B=○, X=□, Y=△).
 * Multi-touch: every pointer tracks the button under it; sliding between the
 * stick arrows works without lifting the finger.
 */
public class TouchControls extends View {
    static final class Btn {
        final String label;
        final int[] keys;       // Android keycodes pressed together
        final boolean round;
        final RectF r = new RectF();
        int pressedBy = 0;      // how many pointers hold it
        Btn(String label, boolean round, int... keys) { this.label = label; this.round = round; this.keys = keys; }
    }

    final Btn up = new Btn("▲", false, KeyEvent.KEYCODE_W);
    final Btn down = new Btn("▼", false, KeyEvent.KEYCODE_S);
    final Btn left = new Btn("◀", false, KeyEvent.KEYCODE_A);
    final Btn right = new Btn("▶", false, KeyEvent.KEYCODE_D);
    final Btn upLeft = new Btn("", false, KeyEvent.KEYCODE_W, KeyEvent.KEYCODE_A);
    final Btn upRight = new Btn("", false, KeyEvent.KEYCODE_W, KeyEvent.KEYCODE_D);
    final Btn downLeft = new Btn("", false, KeyEvent.KEYCODE_S, KeyEvent.KEYCODE_A);
    final Btn downRight = new Btn("", false, KeyEvent.KEYCODE_S, KeyEvent.KEYCODE_D);
    final Btn cross = new Btn("✕", true, KeyEvent.KEYCODE_SPACE);
    final Btn circle = new Btn("○", true, KeyEvent.KEYCODE_DEL);
    final Btn square = new Btn("□", true, KeyEvent.KEYCODE_L);
    final Btn triangle = new Btn("△", true, KeyEvent.KEYCODE_P);
    final Btn l1 = new Btn("L1", false, KeyEvent.KEYCODE_1);
    final Btn r1 = new Btn("R1", false, KeyEvent.KEYCODE_3);
    final Btn l2 = new Btn("L2", false, KeyEvent.KEYCODE_Q);
    final Btn r2 = new Btn("R2", false, KeyEvent.KEYCODE_E);
    final Btn start = new Btn("START", false, KeyEvent.KEYCODE_ENTER);
    final Btn select = new Btn("SELECT", false, KeyEvent.KEYCODE_TAB);
    final Btn toggle = new Btn("◐", true);

    final List<Btn> all = new ArrayList<>();
    final Map<Integer, Btn> pointers = new HashMap<>();
    final Map<Integer, Integer> keyRefs = new HashMap<>();
    final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    boolean visible = true;

    public TouchControls(Context c) {
        super(c);
        Btn[] bs = {up, down, left, right, upLeft, upRight, downLeft, downRight, cross, circle,
                square, triangle, l1, r1, l2, r2, start, select, toggle};
        for (Btn b : bs) all.add(b);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(3f);
        stroke.setColor(Color.argb(150, 255, 255, 255));
        text.setColor(Color.argb(210, 255, 255, 255));
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(Typeface.DEFAULT_BOLD);
    }

    @Override protected void onSizeChanged(int w, int h, int ow, int oh) {
        float u = Math.min(w, h) / 9f;           // base unit (~120 px on a 1080p-tall screen)
        float m = u * 0.35f;                      // margin from the edges
        // D-pad / stick, bottom-left: 3x3 grid, diagonals in the corners.
        float cx = m + u * 1.6f, cy = h - m - u * 1.6f, s = u * 0.95f;
        cell(upLeft, cx - s, cy - s, s); cell(up, cx, cy - s, s); cell(upRight, cx + s, cy - s, s);
        cell(left, cx - s, cy, s); cell(right, cx + s, cy, s);
        cell(downLeft, cx - s, cy + s, s); cell(down, cx, cy + s, s); cell(downRight, cx + s, cy + s, s);
        // Face buttons, bottom-right diamond.
        float fx = w - m - u * 1.6f, fy = h - m - u * 1.6f, d = u * 1.05f, r = u * 0.55f;
        circle(triangle, fx, fy - d, r); circle(cross, fx, fy + d, r);
        circle(square, fx - d, fy, r); circle(circle, fx + d, fy, r);
        // Shoulders, top corners.
        float bw = u * 1.5f, bh = u * 0.75f;
        l2.r.set(m, m, m + bw, m + bh); l1.r.set(m, m + bh * 1.25f, m + bw, m + bh * 2.25f);
        r2.r.set(w - m - bw, m, w - m, m + bh); r1.r.set(w - m - bw, m + bh * 1.25f, w - m, m + bh * 2.25f);
        // Start / Select, bottom centre.
        float sw = u * 1.5f, sh = u * 0.55f;
        select.r.set(w / 2f - sw - u * 0.2f, h - m - sh, w / 2f - u * 0.2f, h - m);
        start.r.set(w / 2f + u * 0.2f, h - m - sh, w / 2f + sw + u * 0.2f, h - m);
        // Show/hide toggle: top centre but clear of the status-bar swipe zone
        // (touches on the top ~40 px are taken by the system gesture).
        circle(toggle, w / 2f, m + u * 0.9f, u * 0.42f);
        text.setTextSize(u * 0.38f);
    }

    static void cell(Btn b, float x, float y, float s) { b.r.set(x - s / 2, y - s / 2, x + s / 2, y + s / 2); }
    static void circle(Btn b, float x, float y, float r) { b.r.set(x - r, y - r, x + r, y + r); }

    @Override protected void onDraw(Canvas c) {
        for (Btn b : all) {
            if (!visible && b != toggle) continue;
            boolean diag = b.label.isEmpty();
            fill.setColor(b.pressedBy > 0 ? Color.argb(130, 255, 140, 26)
                    : Color.argb(diag ? 25 : 60, 255, 255, 255));
            if (b.round) {
                c.drawOval(b.r, fill);
                c.drawOval(b.r, stroke);
            } else {
                float rad = b.r.height() * 0.25f;
                c.drawRoundRect(b.r, rad, rad, fill);
                if (!diag) c.drawRoundRect(b.r, rad, rad, stroke);
            }
            if (!diag) {
                float ty = b.r.centerY() - (text.descent() + text.ascent()) / 2;
                c.drawText(b.label, b.r.centerX(), ty, text);
            }
        }
    }

    Btn hit(float x, float y) {
        for (Btn b : all) {
            if (!visible && b != toggle) continue;
            RectF g = new RectF(b.r);
            float grow = b == toggle ? 0.35f : 0.08f;                // forgiving hit boxes
            g.inset(-b.r.width() * grow, -b.r.height() * grow);
            if (g.contains(x, y)) return b;
        }
        return null;
    }

    void press(Btn b) {
        if (b == null) return;
        if (b == toggle) {
            visible = !visible;
            releaseAll();
            invalidate();
            return;
        }
        if (b.pressedBy++ == 0) {
            for (int k : b.keys) {
                int n = keyRefs.getOrDefault(k, 0);
                if (n == 0) SDLActivity.onNativeKeyDown(k);
                keyRefs.put(k, n + 1);
            }
        }
    }

    void release(Btn b) {
        if (b == null || b == toggle || b.pressedBy == 0) return;
        if (--b.pressedBy == 0) {
            for (int k : b.keys) {
                int n = keyRefs.getOrDefault(k, 0);
                if (n <= 1) { keyRefs.remove(k); SDLActivity.onNativeKeyUp(k); }
                else keyRefs.put(k, n - 1);
            }
        }
    }

    void releaseAll() {
        for (Btn b : pointers.values()) release(b);
        pointers.clear();
    }

    @Override public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        int idx = e.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int id = e.getPointerId(idx);
                Btn b = hit(e.getX(idx), e.getY(idx));
                pointers.put(id, b);
                press(b);
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) {
                    int id = e.getPointerId(i);
                    Btn old = pointers.get(id);
                    if (old == toggle) continue;
                    Btn now = hit(e.getX(i), e.getY(i));
                    if (now == toggle) now = null;
                    if (now != old) { release(old); press(now); pointers.put(id, now); }
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int id = e.getPointerId(idx);
                Btn b = pointers.remove(id);
                if (b != toggle) release(b);
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                releaseAll();
                break;
        }
        invalidate();
        // Consume touches on buttons; let the rest fall through to SDL.
        return true;
    }
}
