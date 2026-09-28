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

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        surfaceView = findViewById(R.id.surfaceView);
        tvNormal = findViewById(R.id.tvNormal);
        tvBold = findViewById(R.id.tvBold);

        surfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                float textSizePx = tvNormal.getTextSize();
                android.graphics.Typeface tfNormal = tvNormal.getTypeface();
                android.graphics.Typeface tfBold = tvBold.getTypeface();

                int boldWeight = 700;
                int normalWeight = 400;
                if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.P) {
                    if (tfNormal != null) normalWeight = tfNormal.getWeight();
                    if (tfBold != null) boldWeight = tfBold.getWeight();
                }
                android.text.TextPaint paintNormal = tvNormal.getPaint();
                android.text.TextPaint paintBold = tvBold.getPaint();

                android.util.Log.i("HelloSkia_Java", String.format(
                        "[Android TextView] textSizePx=%.2f, Normal(weight=%d, isBold=%b, fakeBold=%b), Bold(weight=%d, isBold=%b, fakeBold=%b, skewX=%.2f)",
                        textSizePx, normalWeight, tfNormal != null && tfNormal.isBold(), paintNormal.isFakeBoldText(),
                        boldWeight, tfBold != null && tfBold.isBold(), paintBold.isFakeBoldText(), paintBold.getTextSkewX()));

                nativeInit(holder.getSurface(), width, height, textSizePx, boldWeight);
                nativeRender();
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                nativeDestroy();
            }
        });
    }
}