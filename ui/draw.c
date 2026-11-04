#include "draw.h"

#include <string.h>
#include "format.h"

#define SIGNAL_SLOTS 60

#ifndef CLR_NONE
#define CLR_NONE ((COLORREF)0xFFFFFFFF)
#endif

static void FillRound(HDC dc, RECT r, int radius, COLORREF fill, COLORREF border, int width) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = NULL;
    if (border != CLR_NONE) pen = CreatePen(PS_SOLID, width > 0 ? width : 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    if (pen) DeleteObject(pen);
    DeleteObject(brush);
}

void tm_fill_rect(HDC dc, const RECT* r, COLORREF color) {
    if (!dc || !r) return;
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, r, brush);
    DeleteObject(brush);
}

void tm_fill_round(HDC dc, const RECT* r, int radius, COLORREF color) {
    if (!dc || !r) return;
    FillRound(dc, *r, radius, color, CLR_NONE, 0);
}

void tm_stroke_round(HDC dc, const RECT* r, int radius, COLORREF color, int width) {
    if (!dc || !r) return;
    int w = width > 0 ? width : 1;
    RECT inset = *r;
    InflateRect(&inset, -w / 2, -w / 2);
    HPEN pen = CreatePen(PS_SOLID, w, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, inset.left, inset.top, inset.right, inset.bottom,
              radius * 2, radius * 2);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void tm_card(HDC dc, const RECT* r, int radius) {
    if (!dc || !r) return;
    FillRound(dc, *r, radius, TM_COLOR_SURFACE, TM_COLOR_BORDER, 1);
}

void tm_line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int width) {
    if (!dc) return;
    HPEN pen = CreatePen(PS_SOLID, width > 0 ? width : 1, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, NULL);
    LineTo(dc, x2, y2);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void tm_dot(HDC dc, int cx, int cy, int radius, COLORREF color) {
    if (!dc || radius <= 0) return;
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, cx - radius, cy - radius, cx + radius + 1, cy + radius + 1);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
}

void tm_text(HDC dc, const wchar_t* text, RECT r, HFONT font, COLORREF color, UINT flags) {
    if (!dc || !text) return;
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : NULL;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &r, flags | DT_NOPREFIX);
    if (oldFont) SelectObject(dc, oldFont);
}

static int64_t SignalMax(const uint64_t* up, const uint64_t* down, int count) {
    int64_t maximum = 4096;  /* keep an idle line flat instead of amplifying noise */
    for (int i = 0; i < count; ++i) {
        if ((int64_t)up[i] > maximum) maximum = (int64_t)up[i];
        if ((int64_t)down[i] > maximum) maximum = (int64_t)down[i];
    }
    return maximum;
}

static void SignalArea(HDC dc, RECT plot, const uint64_t* values, int count,
                       int64_t maximum, int top, int bottom, COLORREF fill) {
    if (count <= 0) return;
    const double slot = (double)(plot.right - plot.left - 1) / 59.0;
    POINT points[64];
    for (int i = 0; i < count; ++i) {
        const double x = (double)plot.right - 1.0 - (double)(count - 1 - i) * slot;
        const double ratio = (double)values[i] / (double)maximum;
        double y = (double)bottom - ratio * (double)(bottom - top);
        if (y < top) y = top;
        points[i].x = (LONG)(x + 0.5);
        points[i].y = (LONG)(y + 0.5);
    }
    points[count].x = points[count - 1].x;
    points[count].y = bottom;
    points[count + 1].x = points[0].x;
    points[count + 1].y = bottom;
    HBRUSH brush = CreateSolidBrush(fill);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, points, count + 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
}

static void SignalStroke(HDC dc, RECT plot, const uint64_t* values, int count,
                         int64_t maximum, int top, int bottom, COLORREF color) {
    if (count <= 0) return;
    const double slot = (double)(plot.right - plot.left - 1) / 59.0;
    POINT points[64];
    for (int i = 0; i < count; ++i) {
        const double x = (double)plot.right - 1.0 - (double)(count - 1 - i) * slot;
        const double ratio = (double)values[i] / (double)maximum;
        double y = (double)bottom - ratio * (double)(bottom - top);
        if (y < top) y = top;
        points[i].x = (LONG)(x + 0.5);
        points[i].y = (LONG)(y + 0.5);
    }
    if (count == 1) {
        tm_dot(dc, points[0].x, points[0].y, 1, color);
        return;
    }
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Polyline(dc, points, count);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void tm_draw_signal(HDC dc, RECT plot, const uint64_t* up, const uint64_t* down,
                    int count, const TmTheme* theme) {
    if (!dc || !theme) return;
    const int top = plot.top + 4;
    const int bottom = plot.bottom - 1;
    if (bottom <= top) return;

    for (int i = 0; i <= 4; ++i) {
        const int y = top + (bottom - top) * i / 4;
        tm_line(dc, plot.left, y, plot.right, y,
                i == 4 ? TM_COLOR_BORDER : TM_COLOR_SURFACE_MUTED, 1);
    }
    if (!up || !down || count <= 0) return;
    if (count > SIGNAL_SLOTS) count = SIGNAL_SLOTS;

    const int64_t maximum = SignalMax(up, down, count);

    int saved = SaveDC(dc);
    IntersectClipRect(dc, plot.left, plot.top, plot.right, plot.bottom);
    SignalArea(dc, plot, down, count, maximum, top, bottom, TM_COLOR_DOWN_SOFT);
    SignalArea(dc, plot, up, count, maximum, top, bottom, TM_COLOR_UP_SOFT);
    SignalStroke(dc, plot, down, count, maximum, top, bottom, TM_COLOR_DOWN);
    SignalStroke(dc, plot, up, count, maximum, top, bottom, TM_COLOR_UP);
    RestoreDC(dc, saved);

    wchar_t peak[32];
    tm_format_rate((uint64_t)maximum, peak, 32);
    RECT label = {plot.left + 4, plot.top, plot.left + 160, plot.top + 16};
    tm_text(dc, peak, label, theme->mono_small, TM_COLOR_INK_3,
            DT_LEFT | DT_TOP | DT_SINGLELINE);
}

void tm_draw_bars(HDC dc, RECT plot, const int64_t* up, const int64_t* down,
                  int count, const TmTheme* theme) {
    if (!dc || !theme || count <= 0) return;
    const int top = plot.top + 6;
    const int bottom = plot.bottom - 1;
    if (bottom <= top) return;

    for (int i = 0; i <= 4; ++i) {
        const int y = top + (bottom - top) * i / 4;
        tm_line(dc, plot.left, y, plot.right, y,
                i == 4 ? TM_COLOR_BORDER : TM_COLOR_SURFACE_MUTED, 1);
    }
    if (!up || !down) return;

    int64_t peak = 0;
    for (int i = 0; i < count; ++i) {
        if (up[i] > peak) peak = up[i];
        if (down[i] > peak) peak = down[i];
    }
    const int64_t maximum = peak > 0 ? peak : 1;

    const double slot = (double)(plot.right - plot.left) / (double)count;
    int barWidth = (int)(slot * 0.34 + 0.5);
    if (barWidth < 3) barWidth = 3;
    if (barWidth > 16) barWidth = 16;
    int gap = (int)(slot * 0.05 + 0.5);
    if (gap < 2) gap = 2;

    for (int i = 0; i < count; ++i) {
        const double center = (double)plot.left + slot * ((double)i + 0.5);
        const int downX = (int)(center - barWidth - gap / 2.0 + 0.5);
        const int upX = (int)(center + gap / 2.0 + 0.5);

        int downHeight = (int)((double)down[i] / (double)maximum * (double)(bottom - top));
        int upHeight = (int)((double)up[i] / (double)maximum * (double)(bottom - top));
        if (down[i] > 0 && downHeight < 2) downHeight = 2;
        if (up[i] > 0 && upHeight < 2) upHeight = 2;
        if (downHeight < 1) downHeight = 1;
        if (upHeight < 1) upHeight = 1;

        RECT downBar = {downX, bottom - downHeight, downX + barWidth, bottom + 1};
        RECT upBar = {upX, bottom - upHeight, upX + barWidth, bottom + 1};
        FillRound(dc, downBar, 2,
                  down[i] > 0 ? TM_COLOR_DOWN : TM_COLOR_SURFACE_MUTED, CLR_NONE, 0);
        FillRound(dc, upBar, 2,
                  up[i] > 0 ? TM_COLOR_UP : TM_COLOR_SURFACE_MUTED, CLR_NONE, 0);
    }

    if (peak > 0) {
        wchar_t peakText[32];
        tm_format_bytes(peak, peakText, 32);
        RECT label = {plot.left + 4, plot.top, plot.left + 160, plot.top + 16};
        tm_text(dc, peakText, label, theme->mono_small, TM_COLOR_INK_3,
                DT_LEFT | DT_TOP | DT_SINGLELINE);
    }
}