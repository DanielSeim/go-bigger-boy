package com.danielseim.gbb;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

final class TouchLayoutView extends View {
    private static final float[] WIDTHS = {42, 24, 24, 22, 22};
    private static final float[] HEIGHTS = {42, 24, 24, 10, 10};
    private static final String[] LABELS = {
            "D-pad", "A", "B", "Select", "Start"};

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final float density;
    private float[] layout;
    private int selected = -1;
    private float dragOffsetX;
    private float dragOffsetY;
    private boolean landscape;

    TouchLayoutView(Context context, float[] initial) {
        super(context);
        density = getResources().getDisplayMetrics().density;
        layout = new float[20];
        if (initial == null || initial.length < layout.length) {
            layout = defaultLayout();
        } else {
            System.arraycopy(initial, 0, layout, 0, layout.length);
        }
        setBackgroundColor(Color.rgb(26, 31, 42));
    }

    static float[] defaultLayout() {
        return new float[]{
                0.27f, 0.82f, 0.74f, 0.79f, 0.74f, 0.90f, 0.43f, 0.96f,
                0.57f, 0.96f,
                0.12f, 0.50f, 0.88f, 0.42f, 0.88f, 0.62f, 0.42f, 0.92f,
                0.58f, 0.92f};
    }

    float[] getLayout() {
        return layout.clone();
    }

    void setLayout(float[] values) {
        if (values == null || values.length < layout.length) return;
        System.arraycopy(values, 0, layout, 0, layout.length);
        invalidate();
    }

    void setLandscape(boolean value) {
        landscape = value;
        selected = -1;
        invalidate();
    }

    private float designWidth() {
        return landscape ? 320f : 180f;
    }

    private float designHeight() {
        return landscape ? 180f : 320f;
    }

    private float logicalScale() {
        return Math.min(getWidth() / designWidth(),
                getHeight() / designHeight());
    }

    private float offsetX() {
        return (getWidth() - designWidth() * logicalScale()) * 0.5f;
    }

    private float offsetY() {
        return (getHeight() - designHeight() * logicalScale()) * 0.5f;
    }

    private int positionIndex(int control) {
        return (landscape ? 10 : 0) + control * 2;
    }

    private RectF bounds(int index) {
        final float scale = logicalScale();
        final int position = positionIndex(index);
        final float centerX = offsetX() + layout[position] *
                designWidth() * scale;
        final float centerY = offsetY() + layout[position + 1] *
                designHeight() * scale;
        final float width = WIDTHS[index] * scale;
        final float height = HEIGHTS[index] * scale;
        return new RectF(centerX - width * 0.5f, centerY - height * 0.5f,
                centerX + width * 0.5f, centerY + height * 0.5f);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        final float scale = logicalScale();
        final float screenLeft = offsetX() + (landscape ? 80f : 10f) * scale;
        final float screenTop = offsetY() + (landscape ? 18f : 10f) * scale;
        final RectF screen = new RectF(screenLeft, screenTop,
                screenLeft + 160f * scale, screenTop + 144f * scale);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(Math.max(1f, scale));
        paint.setColor(Color.rgb(151, 170, 132));
        canvas.drawRect(screen, paint);
        paint.setStyle(Paint.Style.FILL);
        for (int index = 0; index < 5; ++index) {
            final RectF controlBounds = bounds(index);
            paint.setColor(index == selected
                    ? Color.rgb(255, 215, 92) : Color.rgb(124, 183, 140));
            canvas.drawRoundRect(controlBounds, 5f * scale, 5f * scale, paint);
            paint.setColor(Color.rgb(24, 35, 38));
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setTextSize(Math.max(9f, 8f * scale));
            canvas.drawText(LABELS[index], controlBounds.centerX(),
                    controlBounds.centerY() -
                            (paint.ascent() + paint.descent()) * 0.5f, paint);
        }
    }

    private int hit(float x, float y) {
        for (int index = 4; index >= 0; --index) {
            final RectF hitBounds = bounds(index);
            final float expansion = Math.max(dp(10), logicalScale() * 7f);
            hitBounds.inset(-expansion, -expansion);
            if (hitBounds.contains(x, y)) return index;
        }
        return -1;
    }

    private float dp(float value) {
        return value * density;
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        final float scale = logicalScale();
        switch (event.getActionMasked()) {
        case MotionEvent.ACTION_DOWN:
            selected = hit(event.getX(), event.getY());
            if (selected < 0) return false;
            dragOffsetX = event.getX() - bounds(selected).centerX();
            dragOffsetY = event.getY() - bounds(selected).centerY();
            invalidate();
            return true;
        case MotionEvent.ACTION_MOVE:
            if (selected < 0 || scale <= 0) return true;
            final float centerX = event.getX() - dragOffsetX;
            final float centerY = event.getY() - dragOffsetY;
            final int position = positionIndex(selected);
            layout[position] = Math.max(0.02f, Math.min(0.98f,
                    (centerX - offsetX()) / (designWidth() * scale)));
            layout[position + 1] = Math.max(0.02f, Math.min(0.98f,
                    (centerY - offsetY()) / (designHeight() * scale)));
            invalidate();
            return true;
        case MotionEvent.ACTION_UP:
            performClick();
            selected = -1;
            invalidate();
            return true;
        case MotionEvent.ACTION_CANCEL:
            selected = -1;
            invalidate();
            return true;
        default:
            return true;
        }
    }

    @Override
    public boolean performClick() {
        super.performClick();
        return true;
    }
}

