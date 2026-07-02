package org.tvp.kirikiri2;

import android.content.Context;
import android.graphics.Color;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * Real-time engine log overlay.
 * Receives log lines from native TVPAddLog via JNI and displays them
 * in a semi-transparent scrolling text view at the top of the screen.
 */
public class LogOverlay extends ScrollView {
    private static LogOverlay sInstance;
    private TextView mTextView;
    private boolean mVisible = false;
    private static final int MAX_LINES = 300;

    public LogOverlay(Context context) {
        super(context);
        sInstance = this;

        mTextView = new TextView(context);
        mTextView.setTextColor(Color.rgb(200, 200, 200));
        mTextView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
        mTextView.setPadding(8, 4, 8, 4);
        mTextView.setIncludeFontPadding(false);
        addView(mTextView, new LayoutParams(
            LayoutParams.MATCH_PARENT,
            LayoutParams.WRAP_CONTENT));

        setBackgroundColor(Color.argb(140, 0, 0, 0));
        setVerticalScrollBarEnabled(true);
        setVisibility(View.GONE);

        int height = (int)(240 * getResources().getDisplayMetrics().density);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, height);
        lp.gravity = Gravity.TOP | Gravity.FILL_HORIZONTAL;
        setLayoutParams(lp);
    }

    /** Called from native via JNI — must be thread-safe. */
    public static void appendLog(String line) {
        if (sInstance != null) {
            sInstance.post(() -> sInstance.appendLine(line));
        }
    }

    private void appendLine(String line) {
        mTextView.append(line + "\n");
        // Trim excess lines
        String text = mTextView.getText().toString();
        int nlCount = 0;
        int cutIdx = 0;
        for (int i = text.length() - 1; i >= 0; i--) {
            if (text.charAt(i) == '\n') {
                nlCount++;
                if (nlCount > MAX_LINES) {
                    cutIdx = i + 1;
                    break;
                }
            }
        }
        if (cutIdx > 0) {
            mTextView.setText(text.substring(cutIdx));
        }
        // Auto-scroll to bottom
        post(() -> fullScroll(View.FOCUS_DOWN));
    }

    public void toggle() {
        mVisible = !mVisible;
        setVisibility(mVisible ? View.VISIBLE : View.GONE);
    }

    public boolean isOverlayVisible() {
        return mVisible;
    }

    public static void detach() {
        sInstance = null;
    }
}
