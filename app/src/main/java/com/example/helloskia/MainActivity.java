package com.example.helloskia;

import android.app.Activity;
import android.os.Bundle;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

public class MainActivity extends Activity {

    static {
        System.loadLibrary("native-lib");
    }

    private native void nativeInit(Surface surface, int width, int height, float textSizePx, int boldWeight);
    private native void nativeResize(int width, int height, float textSizePx, int boldWeight);
    private native void nativeRender();
    private native void nativeDestroy();

    private SurfaceView surfaceView;
    private android.widget.TextView tvNormal;
    private android.widget.TextView tvBold;
    private android.widget.TextView tvBoldItalic;
    private boolean isInitialized = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        surfaceView = findViewById(R.id.surfaceView);
        tvNormal = findViewById(R.id.tvNormal);
        tvBold = findViewById(R.id.tvBold);
        tvBoldItalic = findViewById(R.id.tvBoldItalic);

        android.graphics.Typeface tfBase = android.graphics.Typeface.create("sec", android.graphics.Typeface.NORMAL);
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.P) {
            tvNormal.setTypeface(android.graphics.Typeface.create(tfBase, 400, false));
            tvBold.setTypeface(android.graphics.Typeface.create(tfBase, 700, false));
            tvBoldItalic.setTypeface(android.graphics.Typeface.create(tfBase, 700, true));
        } else {
            tvNormal.setTypeface(tfBase, android.graphics.Typeface.NORMAL);
            tvBold.setTypeface(tfBase, android.graphics.Typeface.BOLD);
            tvBoldItalic.setTypeface(tfBase, android.graphics.Typeface.BOLD_ITALIC);
        }

        surfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                // Calculate optimal font size based on cell dimensions and device DPI
                int cellHeight = height / 3;
                int cellWidth = width;

                // 5 lines of text. Each line height is approx 1.30 * textSizePx.
                // We want the text block to occupy ~70% to 72% of cell height for balanced margins:
                float targetByHeight = (cellHeight * 0.70f) / (5.0f * 1.30f);
                // Longest line is ~5 full-width characters (Korean "안녕하세요" / Hebrew / Arabic),
                // we want it to occupy at most ~80% of cell width:
                float targetByWidth = (cellWidth * 0.80f) / 5.0f;

                float optimalTextSizePx = Math.min(targetByHeight, targetByWidth);

                android.util.DisplayMetrics dm = getResources().getDisplayMetrics();
                // Ensure a sensible minimum size based on DPI (at least 20sp in pixels)
                float minSizePx = 20.0f * dm.density;
                if (optimalTextSizePx < minSizePx) {
                    optimalTextSizePx = minSizePx;
                }

                // Apply dynamically to all 3 TextViews
                tvNormal.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, optimalTextSizePx);
                tvBold.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, optimalTextSizePx);
                if (tvBoldItalic != null) {
                    tvBoldItalic.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, optimalTextSizePx);
                }

                float textSizePx = tvNormal.getTextSize();
                android.text.TextPaint paintNormal = tvNormal.getPaint();
                android.text.TextPaint paintBold = tvBold.getPaint();

                android.graphics.Typeface tfNormal = tvNormal.getTypeface();
                android.graphics.Typeface tfBold = tvBold.getTypeface();

                int boldWeight = 700;
                int normalWeight = 400;
                if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.P) {
                    if (tfNormal != null) normalWeight = tfNormal.getWeight();
                    if (tfBold != null) boldWeight = tfBold.getWeight();
                }

                android.util.Log.i("HelloSkia_Java", String.format(
                        "[Android TextView] optimalTextSizePx=%.2f (DPI=%d, density=%.2f), Normal(weight=%d, isBold=%b, fakeBold=%b), Bold(weight=%d, isBold=%b, fakeBold=%b, skewX=%.2f)",
                        textSizePx, dm.densityDpi, dm.density, normalWeight, tfNormal != null && tfNormal.isBold(), paintNormal.isFakeBoldText(),
                        boldWeight, tfBold != null && tfBold.isBold(), paintBold.isFakeBoldText(), paintBold.getTextSkewX()));

                if (!isInitialized) {
                    nativeInit(holder.getSurface(), width, height, textSizePx, boldWeight);
                    isInitialized = true;
                } else {
                    nativeResize(width, height, textSizePx, boldWeight);
                }
                nativeRender();
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                nativeDestroy();
                isInitialized = false;
            }
        });
    }
}