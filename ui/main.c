/* 流量台 — pure C Win32 UI.
 *
 * Threading: collection, storage, and querying all happen on this UI thread
 * (ETW owns its own worker internally). One WM_TIMER tick per second drives
 * tm_tick() and, in the history view, tm_history().
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "bridge.h"
#include "draw.h"
#include "format.h"
#include "theme.h"
#include "tray.h"

#define APP_NAME  L"流量台"
#define APP_CLASS L"TrafficMonMain"
#define MINI_CLASS L"TrafficMonMini"
#define TIMER_ID  1
#define WM_TM_EXIT (WM_APP + 2)
#define MINI_WIDTH 360
#define MINI_LIST_TOP 92
#define MINI_HEADER 22
#define MINI_ROW 24
#define MINI_FOOTER 36
#define WATCH_TEXT_CHARS (TM_MAX_WATCH * (TM_PATH_CHARS + 1))
#define MINI_CANDIDATES (TM_MAX_APPS + TM_MAX_WATCH)
#define MINI_PICK_BASE 50000
#define MINI_CLEAR_ID 49990
#define WATCH_FILE_NAME L"mini-watch.txt"
#define MINI_ALPHA_BASE 49970
#define MINI_ALPHA_MIN 40
#define MINI_SKIN_BASE 49950
static const int kMiniAlphaSteps[] = {100, 90, 80, 70, 60, 50, 40};

#define MINI_SKIN_COUNT tm_palette_count()

typedef struct {
    RECT navDash;
    RECT navHist;
    RECT chipDay;
    RECT chipMonth;
    RECT prev;
    RECT next;
    RECT periodTitle;
} Layout;

typedef struct {
    HDC dc;
    HBITMAP bmp;
    HGDIOBJ old;
    int w;
    int h;
} Surface;

typedef struct App {
    HINSTANCE instance;
    HWND hwnd;
    HICON icon;

    TmEngine* engine;
    TmSnapshot* snap;
    TmHistory* history;

    TmTheme theme;
    Layout layout;

    int view;          /* 0 dashboard, 1 history */
    int histMonth;     /* 0 day, 1 month */
    int32_t anchorDay;
    wchar_t histError[256];

    int hoverNav;      /* -1 none, 0 dashboard, 1 history */
    int hoverChip;     /* -1 none, 0 today, 1 this month */
    int hoverPrev;
    int hoverNext;

    int exitAfter;
    int ticks;
    int quitting;
    int shutdown;
    int trayAdded;

    UINT taskbarCreated;

    HDC memdc;
    HBITMAP membmp;
    HGDIOBJ oldbmp;
    int bw;
    int bh;

    HWND mini;
    int miniMode;
    int miniAlpha;
    int miniSkin;
    Surface miniBuf;
    wchar_t watchFile[TM_PATH_CHARS];
    wchar_t watchPaths[TM_MAX_WATCH][TM_PATH_CHARS];
    int watchTotal;
    wchar_t candidates[MINI_CANDIDATES][TM_PATH_CHARS];
    int candidateTotal;
} App;

static int S(const App* app, int px) { return tm_scale(app->theme.dpi, px); }
static int MaxI(int a, int b) { return a > b ? a : b; }

static int TextWidth(HDC dc, const wchar_t* text, HFONT font) {
    if (!text || !font) return 0;
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size = {0, 0};
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size);
    SelectObject(dc, old);
    return size.cx;
}

static void CopyString(wchar_t* dst, int cch, const wchar_t* src) {
    if (!dst || cch <= 0) return;
    if (!src) { dst[0] = L'\0'; return; }
    wcsncpy(dst, src, (size_t)cch - 1);
    dst[cch - 1] = L'\0';
}

static int DpiOfWindow(HWND hwnd) {
    typedef UINT (WINAPI* GetDpiForWindowFn)(HWND);
    static GetDpiForWindowFn fn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) fn = (GetDpiForWindowFn)(void*)GetProcAddress(user32, "GetDpiForWindow");
    }
    if (fn && hwnd) {
        const UINT dpi = fn(hwnd);
        if (dpi) return (int)dpi;
    }
    HDC dc = GetDC(hwnd);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return dpi > 0 ? dpi : 96;
}

/* ---------------------------------------------------------------- layout */

static void LayoutChrome(App* app, const RECT* client) {
    const int width = client->right;
    const int margin = S(app, 28);
    const int header = S(app, 64);
    const int item = S(app, 84);
    Layout* l = &app->layout;

    l->navHist.left = width - margin - item;
    l->navHist.right = width - margin;
    l->navHist.top = 0;
    l->navHist.bottom = header;
    l->navDash.left = l->navHist.left - item;
    l->navDash.right = l->navHist.left;
    l->navDash.top = 0;
    l->navDash.bottom = header;

    const int top = header + S(app, 24);
    const int chipW = S(app, 68);
    const int chipH = S(app, 28);
    l->chipDay.left = margin;
    l->chipDay.right = margin + chipW;
    l->chipDay.top = top;
    l->chipDay.bottom = top + chipH;
    l->chipMonth.left = l->chipDay.right + S(app, 8);
    l->chipMonth.right = l->chipMonth.left + chipW;
    l->chipMonth.top = top;
    l->chipMonth.bottom = top + chipH;

    l->prev.left = l->chipMonth.right + S(app, 24);
    l->prev.right = l->prev.left + chipH;
    l->prev.top = top;
    l->prev.bottom = top + chipH;
    l->periodTitle.left = l->prev.right + S(app, 8);
    l->periodTitle.right = l->periodTitle.left + S(app, 210);
    l->periodTitle.top = top;
    l->periodTitle.bottom = top + chipH;
    l->next.left = l->periodTitle.right + S(app, 8);
    l->next.right = l->next.left + chipH;
    l->next.top = top;
    l->next.bottom = top + chipH;
}

/* ------------------------------------------------------------ primitives */

static void DrawCardHeading(App* app, HDC dc, RECT card, const wchar_t* title,
                            const wchar_t* note) {
    const int pad = S(app, 20);
    RECT titleRect = {card.left + pad, card.top + pad,
                      card.left + pad + S(app, 260), card.top + pad + S(app, 36)};
    tm_text(dc, title, titleRect, app->theme.title, TM_COLOR_INK,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (note && note[0]) {
        const int width = TextWidth(dc, title, app->theme.title);
        RECT noteRect = {titleRect.left + width + S(app, 8), titleRect.top,
                         card.right - pad - S(app, 130), titleRect.bottom};
        tm_text(dc, note, noteRect, app->theme.small, TM_COLOR_INK_3,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

static void DrawLegend(HDC dc, App* app, int right, int centerY) {
    const int itemWidth = S(app, 58);
    const int box = S(app, 8);
    RECT up = {right - itemWidth, centerY - S(app, 10), right, centerY + S(app, 10)};
    RECT upSwatch = {up.left, centerY - box / 2, up.left + box, centerY + box / 2};
    tm_fill_round(dc, &upSwatch, S(app, 2), TM_COLOR_UP);
    RECT upText = {up.left + box + S(app, 6), up.top, up.right, up.bottom};
    tm_text(dc, L"上传", upText, app->theme.tiny, TM_COLOR_INK_2,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT down = {up.left - S(app, 12) - itemWidth, up.top, up.left - S(app, 12), up.bottom};
    RECT downSwatch = {down.left, centerY - box / 2, down.left + box, centerY + box / 2};
    tm_fill_round(dc, &downSwatch, S(app, 2), TM_COLOR_DOWN);
    RECT downText = {down.left + box + S(app, 6), down.top, down.right, down.bottom};
    tm_text(dc, L"下载", downText, app->theme.tiny, TM_COLOR_INK_2,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void DrawEmptyText(App* app, HDC dc, RECT card, int top,
                          const wchar_t* line1, const wchar_t* line2) {
    const int pad = S(app, 20);
    RECT r = {card.left + pad, top, card.right - pad, top + S(app, 22)};
    tm_text(dc, line1, r, app->theme.small, TM_COLOR_INK_3,
            DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (line2 && line2[0]) {
        r.top += S(app, 20);
        r.bottom += S(app, 20);
        tm_text(dc, line2, r, app->theme.small, TM_COLOR_INK_3,
                DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

static void LanText(const TmAppRow* row, wchar_t* out, int outChars) {
    const int64_t total = row->up + row->down;
    if (!row->lanKnown || total <= 0) {
        CopyString(out, outChars, L"—");
        return;
    }
    const double percent = (double)(row->upLan + row->downLan) * 100.0 / (double)total;
    tm_format_percent(percent, out, outChars);
}

/* Builds the three number columns shared by both app tables. */
static void AppColumns(const App* app, RECT card, int* nameLeft, int* downLeft,
                       int* upLeft, int* lanLeft, int* right) {
    const int pad = S(app, 20);
    *nameLeft = card.left + pad;
    *right = card.right - pad;
    *lanLeft = *right - S(app, 58);
    *upLeft = *lanLeft - S(app, 86);
    *downLeft = *upLeft - S(app, 92);
}

static void DrawAppTable(App* app, HDC dc, RECT card, const TmAppRow* rows, int count,
                         int64_t totalUp, int64_t totalDown) {
    int nameLeft, downLeft, upLeft, lanLeft, right;
    AppColumns(app, card, &nameLeft, &downLeft, &upLeft, &lanLeft, &right);

    const int pad = S(app, 20);
    const int rowHeight = S(app, 36);
    const int tableTop = card.top + pad + S(app, 36) + S(app, 4);
    RECT header = {nameLeft, tableTop, right, tableTop + S(app, 28)};

    RECT nameHeader = {nameLeft, header.top, downLeft - S(app, 8), header.bottom};
    RECT downHeader = {downLeft, header.top, upLeft - S(app, 8), header.bottom};
    RECT upHeader = {upLeft, header.top, lanLeft - S(app, 8), header.bottom};
    RECT lanHeader = {lanLeft, header.top, right, header.bottom};
    tm_text(dc, L"应用", nameHeader, app->theme.tiny, TM_COLOR_INK_2,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    tm_text(dc, L"下载", downHeader, app->theme.tiny, TM_COLOR_INK_2,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    tm_text(dc, L"上传", upHeader, app->theme.tiny, TM_COLOR_INK_2,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    tm_text(dc, L"局域网", lanHeader, app->theme.tiny, TM_COLOR_INK_2,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    tm_line(dc, nameLeft, header.bottom - 1, right, header.bottom - 1, TM_COLOR_BORDER, 1);

    const int available = card.bottom - pad - header.bottom;
    const int maxRows = available / rowHeight;
    if (maxRows <= 0) return;

    int shown = count < maxRows ? count : maxRows;
    int hasOther = 0;
    if (count > maxRows) {
        shown = maxRows > 1 ? maxRows - 1 : 1;
        hasOther = count > shown;
    }

    int shownRows = shown + (hasOther ? 1 : 0);
    int64_t shownUp = 0;
    int64_t shownDown = 0;
    for (int i = 0; i < shown; ++i) {
        const int y = header.bottom + i * rowHeight;
        RECT name = {nameLeft, y, downLeft - S(app, 8), y + rowHeight};
        RECT down = {downLeft, y, upLeft - S(app, 8), y + rowHeight};
        RECT up = {upLeft, y, lanLeft - S(app, 8), y + rowHeight};
        RECT lan = {lanLeft, y, right, y + rowHeight};
        tm_text(dc, rows[i].name, name, app->theme.body, TM_COLOR_INK,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        wchar_t buffer[32];
        tm_format_bytes(rows[i].down, buffer, 32);
        tm_text(dc, buffer, down, app->theme.mono, TM_COLOR_INK_2,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        tm_format_bytes(rows[i].up, buffer, 32);
        tm_text(dc, buffer, up, app->theme.mono, TM_COLOR_INK_2,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        LanText(&rows[i], buffer, 32);
        tm_text(dc, buffer, lan, app->theme.mono, TM_COLOR_INK_3,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        shownUp += rows[i].up;
        shownDown += rows[i].down;
        if (i + 1 < shownRows)
            tm_line(dc, nameLeft, y + rowHeight - 1, right, y + rowHeight - 1,
                    TM_COLOR_SURFACE_MUTED, 1);
    }

    if (hasOther) {
        const int y = header.bottom + shown * rowHeight;
        int64_t otherUp = totalUp - shownUp;
        int64_t otherDown = totalDown - shownDown;
        if (otherUp < 0) otherUp = 0;
        if (otherDown < 0) otherDown = 0;
        RECT name = {nameLeft, y, downLeft - S(app, 8), y + rowHeight};
        RECT down = {downLeft, y, upLeft - S(app, 8), y + rowHeight};
        RECT up = {upLeft, y, lanLeft - S(app, 8), y + rowHeight};
        RECT lan = {lanLeft, y, right, y + rowHeight};
        tm_text(dc, L"其他应用", name, app->theme.body, TM_COLOR_INK_3,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        wchar_t buffer[32];
        tm_format_bytes(otherDown, buffer, 32);
        tm_text(dc, buffer, down, app->theme.mono, TM_COLOR_INK_3,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        tm_format_bytes(otherUp, buffer, 32);
        tm_text(dc, buffer, up, app->theme.mono, TM_COLOR_INK_3,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        tm_text(dc, L"—", lan, app->theme.mono, TM_COLOR_INK_3,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

static void DrawNicCard(App* app, HDC dc, RECT card, const TmSnapshot* snap) {
    tm_card(dc, &card, S(app, 10));
    DrawCardHeading(app, dc, card, L"网卡", L"实时速度");

    /* Two number columns only, anchored to the card's right padding. */
    const int pad = S(app, 20);
    const int right = card.right - pad;
    const int nameLeft = card.left + pad;
    const int upLeft = right - S(app, 86);
    const int downLeft = upLeft - S(app, 92);
    const int rowHeight = S(app, 36);
    const int tableTop = card.top + pad + S(app, 36) + S(app, 4);
    RECT header = {nameLeft, tableTop, right, tableTop + S(app, 28)};

    RECT nameHeader = {nameLeft, header.top, downLeft - S(app, 8), header.bottom};
    RECT downHeader = {downLeft, header.top, upLeft - S(app, 8), header.bottom};
    RECT upHeader = {upLeft, header.top, right, header.bottom};
    tm_text(dc, L"接口", nameHeader, app->theme.tiny, TM_COLOR_INK_2,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    tm_text(dc, L"下载", downHeader, app->theme.tiny, TM_COLOR_INK_2,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    tm_text(dc, L"上传", upHeader, app->theme.tiny, TM_COLOR_INK_2,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    tm_line(dc, nameLeft, header.bottom - 1, right, header.bottom - 1, TM_COLOR_BORDER, 1);

    const int available = card.bottom - pad - header.bottom;
    const int maxRows = available / rowHeight;
    if (maxRows <= 0) return;

    const int count = snap->nicCount;
    if (count == 0) {
        DrawEmptyText(app, dc, card, header.bottom + S(app, 8),
                      snap->running ? L"正在读取网卡计数…" : L"采集尚未开始。",
                      snap->running ? L"首个采样在一秒内出现。" : NULL);
        return;
    }

    int shown = count < maxRows ? count : maxRows;
    int hasOther = 0;
    if (count > maxRows) {
        shown = maxRows > 1 ? maxRows - 1 : 1;
        hasOther = count > shown;
    }
    const int shownRows = shown + (hasOther ? 1 : 0);
    uint64_t otherUp = 0;
    uint64_t otherDown = 0;

    for (int i = 0; i < shown; ++i) {
        const int y = header.bottom + i * rowHeight;
        RECT name = {nameLeft, y, downLeft - S(app, 8), y + rowHeight};
        RECT down = {downLeft, y, upLeft - S(app, 8), y + rowHeight};
        RECT up = {upLeft, y, right, y + rowHeight};
        wchar_t buffer[32];
        tm_text(dc, snap->nics[i].alias, name, app->theme.body, TM_COLOR_INK,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        tm_format_rate(snap->nics[i].downPerSec, buffer, 32);
        tm_text(dc, buffer, down, app->theme.mono, TM_COLOR_INK_2,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        tm_format_rate(snap->nics[i].upPerSec, buffer, 32);
        tm_text(dc, buffer, up, app->theme.mono, TM_COLOR_INK_2,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        if (i + 1 < shownRows)
            tm_line(dc, nameLeft, y + rowHeight - 1, right, y + rowHeight - 1,
                    TM_COLOR_SURFACE_MUTED, 1);
    }

    if (hasOther) {
        for (int i = shown; i < count; ++i) {
            otherDown += snap->nics[i].downPerSec;
            otherUp += snap->nics[i].upPerSec;
        }
        const int y = header.bottom + shown * rowHeight;
        RECT name = {nameLeft, y, downLeft - S(app, 8), y + rowHeight};
        RECT down = {downLeft, y, upLeft - S(app, 8), y + rowHeight};
        RECT up = {upLeft, y, right, y + rowHeight};
        wchar_t label[64];
        swprintf(label, 64, L"其他 %d 个接口", count - shown);
        tm_text(dc, label, name, app->theme.body, TM_COLOR_INK_3,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        wchar_t buffer[32];
        tm_format_rate(otherDown, buffer, 32);
        tm_text(dc, buffer, down, app->theme.mono, TM_COLOR_INK_3,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        tm_format_rate(otherUp, buffer, 32);
        tm_text(dc, buffer, up, app->theme.mono, TM_COLOR_INK_3,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

/* ---------------------------------------------------------------- header */

static void DrawHeader(App* app, HDC dc, const RECT* client) {
    const int width = client->right;
    const int margin = S(app, 28);
    const int header = S(app, 64);
    const int centerY = header / 2;

    tm_line(dc, 0, header - 1, width, header - 1, TM_COLOR_BORDER, 1);

    const int iconSize = S(app, 26);
    DrawIconEx(dc, margin, centerY - iconSize / 2, app->icon, iconSize, iconSize,
               0, NULL, DI_NORMAL);

    const int titleX = margin + iconSize + S(app, 12);
    RECT titleRect = {titleX, 0, titleX + S(app, 130), header};
    tm_text(dc, APP_NAME, titleRect, app->theme.display_cjk, TM_COLOR_INK,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const int titleWidth = TextWidth(dc, APP_NAME, app->theme.display_cjk);

    const wchar_t* subtitle = L"本机网络测量";
    RECT subtitleRect = {titleX + titleWidth + S(app, 10), 0,
                         titleX + titleWidth + S(app, 120), header};
    tm_text(dc, subtitle, subtitleRect, app->theme.small, TM_COLOR_INK_3,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const int subtitleWidth = TextWidth(dc, subtitle, app->theme.small);

    const wchar_t* status = app->snap->status[0] ? app->snap->status : L"正在启动";
    const int statusWidth = TextWidth(dc, status, app->theme.tiny);
    int pillLeft = titleX + titleWidth + S(app, 10) + subtitleWidth + S(app, 20);
    int pillRight = pillLeft + statusWidth + S(app, 30);
    const int pillLimit = app->layout.navDash.left - S(app, 10);
    if (pillRight > pillLimit) pillRight = pillLimit;
    RECT pill = {pillLeft, centerY - S(app, 12), pillRight, centerY + S(app, 12)};
    if (pill.right - pill.left > S(app, 40)) {
        tm_fill_round(dc, &pill, S(app, 12), TM_COLOR_SURFACE_MUTED);
        COLORREF dotColor = TM_COLOR_INK_3;
        if (app->snap->running) dotColor = app->snap->etw ? TM_COLOR_SUCCESS : TM_COLOR_WARNING;
        if (!app->snap->running && app->snap->error[0]) dotColor = TM_COLOR_ERROR;
        tm_dot(dc, pill.left + S(app, 12), centerY, S(app, 3), dotColor);
        RECT textRect = {pill.left + S(app, 21), pill.top, pill.right - S(app, 8), pill.bottom};
        tm_text(dc, status, textRect, app->theme.tiny, TM_COLOR_INK_2,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    const wchar_t* labels[2] = {L"仪表盘", L"历史"};
    const RECT* rects[2] = {&app->layout.navDash, &app->layout.navHist};
    for (int i = 0; i < 2; ++i) {
        const int active = app->view == i;
        if (!active && app->hoverNav == i)
            tm_fill_rect(dc, rects[i], TM_COLOR_SURFACE_MUTED);
        tm_text(dc, labels[i], *rects[i], app->theme.body,
                active ? TM_COLOR_DOWN : (app->hoverNav == i ? TM_COLOR_INK : TM_COLOR_INK_2),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (active) {
            tm_line(dc, rects[i]->left + S(app, 8), header - 2,
                    rects[i]->right - S(app, 8), header - 2, TM_COLOR_DOWN, 2);
        }
    }
}

/* ------------------------------------------------------------- dashboard */

static void DrawMetric(HDC dc, App* app, int x, int y, const wchar_t* label,
                       const wchar_t* value, const wchar_t* unit, COLORREF color) {
    RECT labelRect = {x, y, x + S(app, 210), y + S(app, 20)};
    tm_text(dc, label, labelRect, app->theme.small, TM_COLOR_INK_2,
            DT_LEFT | DT_TOP | DT_SINGLELINE);

    RECT valueRect = {x, y + S(app, 18), x + S(app, 200), y + S(app, 62)};
    tm_text(dc, value, valueRect, app->theme.display_xl, color,
            DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
    const int valueWidth = TextWidth(dc, value, app->theme.display_xl);

    RECT unitRect = {x + valueWidth + S(app, 6), y + S(app, 18),
                     x + valueWidth + S(app, 80), y + S(app, 62)};
    tm_text(dc, unit, unitRect, app->theme.body, TM_COLOR_INK_3,
            DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
}

static void DrawSideStat(HDC dc, App* app, RECT r, const wchar_t* label,
                         int64_t up, int64_t down) {
    RECT labelRect = {r.left, r.top, r.right, r.top + S(app, 16)};
    tm_text(dc, label, labelRect, app->theme.tiny, TM_COLOR_INK_2,
            DT_RIGHT | DT_TOP | DT_SINGLELINE);
    wchar_t downText[32];
    wchar_t upText[32];
    wchar_t line[96];
    tm_format_bytes(down, downText, 32);
    tm_format_bytes(up, upText, 32);
    swprintf(line, 96, L"↓ %ls · ↑ %ls", downText, upText);
    RECT valueRect = {r.left - S(app, 20), r.top + S(app, 18), r.right, r.top + S(app, 42)};
    tm_text(dc, line, valueRect, app->theme.mono, TM_COLOR_INK,
            DT_RIGHT | DT_TOP | DT_SINGLELINE);
}

static void DrawBandAxis(HDC dc, App* app, RECT plot) {
    static const wchar_t* kLabels[5] = {L"-60s", L"-45s", L"-30s", L"-15s", L"现在"};
    for (int i = 0; i < 5; ++i) {
        const int x = plot.left + (plot.right - plot.left) * i / 4;
        RECT r;
        UINT flags = DT_CENTER | DT_TOP | DT_SINGLELINE;
        if (i == 0) {
            r.left = plot.left;
            r.right = plot.left + S(app, 60);
            flags = DT_LEFT | DT_TOP | DT_SINGLELINE;
        } else if (i == 4) {
            r.left = plot.right - S(app, 60);
            r.right = plot.right;
            flags = DT_RIGHT | DT_TOP | DT_SINGLELINE;
        } else {
            r.left = x - S(app, 30);
            r.right = x + S(app, 30);
        }
        r.top = plot.bottom + S(app, 3);
        r.bottom = r.top + S(app, 16);
        tm_text(dc, kLabels[i], r, app->theme.mono_small, TM_COLOR_INK_3, flags);
    }
}

static void DrawDashboard(App* app, HDC dc, const RECT* client) {
    const int width = client->right;
    const int margin = S(app, 28);
    const int header = S(app, 64);
    const int contentTop = header + S(app, 20);
    const int footerTop = client->bottom - S(app, 48);

    wchar_t downValue[32], downUnit[16], upValue[32], upUnit[16];
    tm_split_rate(app->snap->downPerSec, downValue, 32, downUnit, 16);
    tm_split_rate(app->snap->upPerSec, upValue, 32, upUnit, 16);
    DrawMetric(dc, app, margin, contentTop, L"下载", downValue, downUnit, TM_COLOR_DOWN);
    DrawMetric(dc, app, margin + S(app, 236), contentTop, L"上传", upValue, upUnit,
               TM_COLOR_UP);

    const int sideWidth = S(app, 210);
    const int sideGap = S(app, 24);
    RECT monthSide = {width - margin - sideWidth, contentTop, width - margin,
                      contentTop + S(app, 64)};
    RECT todaySide = {monthSide.left - sideGap - sideWidth, contentTop,
                      monthSide.left - sideGap, contentTop + S(app, 64)};
    DrawSideStat(dc, app, todaySide, L"今日", app->snap->todayUp, app->snap->todayDown);
    DrawSideStat(dc, app, monthSide, L"本月", app->snap->monthUp, app->snap->monthDown);

    RECT band = {margin, contentTop + S(app, 88), width - margin,
                 contentTop + S(app, 88) + S(app, 228)};
    tm_card(dc, &band, S(app, 10));
    DrawCardHeading(app, dc, band, L"最近 60 秒", L"按网卡总量");
    DrawLegend(dc, app, band.right - S(app, 20), band.top + S(app, 38));

    RECT plot = {band.left + S(app, 20), band.top + S(app, 64),
                 band.right - S(app, 20), band.bottom - S(app, 40)};
    tm_draw_signal(dc, plot, app->snap->signalUp, app->snap->signalDown,
                   app->snap->signalCount, &app->theme);
    DrawBandAxis(dc, app, plot);

    const int bottomTop = band.bottom + S(app, 16);
    const int bottomBottom = footerTop - S(app, 8);
    if (bottomBottom - bottomTop < S(app, 90)) return;

    const int gap = S(app, 16);
    const int nicWidth = (width - 2 * margin - gap) * 2 / 5;
    RECT nicCard = {margin, bottomTop, margin + nicWidth, bottomBottom};
    RECT appCard = {nicCard.right + gap, bottomTop, width - margin, bottomBottom};

    DrawNicCard(app, dc, nicCard, app->snap);

    tm_card(dc, &appCard, S(app, 10));
    DrawCardHeading(app, dc, appCard, L"今日应用", L"应用口径 · 不与网卡相加");
    const int tableTop = appCard.top + S(app, 20) + S(app, 40);
    if (!app->snap->appBytesAvailable) {
        DrawEmptyText(app, dc, appCard, tableTop,
                      L"按应用统计需要管理员权限。",
                      L"网卡速度与累计流量仍可正常查看。");
    } else if (app->snap->appCount == 0) {
        DrawEmptyText(app, dc, appCard, tableTop,
                      L"今天还没有应用流量。",
                      L"保持程序运行，流量会按应用归类。");
    } else {
        DrawAppTable(app, dc, appCard, app->snap->apps, app->snap->appCount,
                     app->snap->appTotalUp, app->snap->appTotalDown);
    }
}

/* --------------------------------------------------------------- history */

static int AtLatestPeriod(const App* app) {
    const int32_t today = tm_today();
    if (app->histMonth)
        return tm_month_start(app->anchorDay) >= tm_month_start(today);
    return app->anchorDay >= today;
}

static void DrawChevron(App* app, HDC dc, RECT r, int pointingRight, COLORREF color) {
    const int cx = (r.left + r.right) / 2;
    const int cy = (r.top + r.bottom) / 2;
    const int dx = S(app, 4);
    const int dy = S(app, 6);
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ old = SelectObject(dc, pen);
    if (pointingRight) {
        MoveToEx(dc, cx - dx, cy - dy, NULL);
        LineTo(dc, cx + dx - 1, cy);
        LineTo(dc, cx - dx, cy + dy);
    } else {
        MoveToEx(dc, cx + dx, cy - dy, NULL);
        LineTo(dc, cx - dx + 1, cy);
        LineTo(dc, cx + dx, cy + dy);
    }
    SelectObject(dc, old);
    DeleteObject(pen);
}

static void DrawChip(App* app, HDC dc, RECT r, const wchar_t* text, int active, int hover) {
    if (active) {
        tm_fill_round(dc, &r, S(app, 14), TM_COLOR_DOWN);
    } else if (hover) {
        tm_fill_round(dc, &r, S(app, 14), TM_COLOR_SURFACE_MUTED);
        tm_stroke_round(dc, &r, S(app, 14), TM_COLOR_BORDER_STRONG, 1);
    } else {
        tm_stroke_round(dc, &r, S(app, 14), TM_COLOR_BORDER, 1);
    }
    tm_text(dc, text, r, app->theme.small,
            active ? TM_COLOR_SURFACE : TM_COLOR_INK_2,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void DrawArrowButton(App* app, HDC dc, RECT r, int pointingRight,
                            int hover, int enabled) {
    if (hover && enabled) tm_fill_round(dc, &r, S(app, 6), TM_COLOR_SURFACE_MUTED);
    DrawChevron(app, dc, r, pointingRight,
                enabled ? (hover ? TM_COLOR_INK : TM_COLOR_INK_2) : TM_COLOR_BORDER_STRONG);
}

static void DrawStatBlock(HDC dc, App* app, RECT r, const wchar_t* label, int64_t value) {
    RECT labelRect = {r.left, r.top, r.right, r.top + S(app, 16)};
    tm_text(dc, label, labelRect, app->theme.tiny, TM_COLOR_INK_2,
            DT_RIGHT | DT_TOP | DT_SINGLELINE);
    wchar_t text[32];
    tm_format_bytes(value, text, 32);
    RECT valueRect = {r.left, r.top + S(app, 16), r.right, r.top + S(app, 42)};
    tm_text(dc, text, valueRect, app->theme.mono_large, TM_COLOR_INK,
            DT_RIGHT | DT_TOP | DT_SINGLELINE);
}

static void DrawHistoryAxis(HDC dc, App* app, RECT plot, const TmHistory* history) {
    const int count = history->count;
    if (count <= 0) return;
    const double slot = (double)(plot.right - plot.left) / (double)count;

    int indexes[TM_MAX_BUCKETS];
    int labels = 0;
    if (history->monthMode) {
        static const int kDays[] = {0, 4, 9, 14, 19, 24};
        for (int i = 0; i < 6; ++i) {
            if (kDays[i] < count) indexes[labels++] = kDays[i];
        }
        if (count - 1 > indexes[labels - 1]) indexes[labels++] = count - 1;
    } else {
        static const int kHours[] = {0, 3, 6, 9, 12, 15, 18, 21, 23};
        for (int i = 0; i < 9; ++i) {
            if (kHours[i] < count) indexes[labels++] = kHours[i];
        }
    }

    for (int i = 0; i < labels; ++i) {
        const int bucket = indexes[i];
        const int x = plot.left + (int)(slot * ((double)bucket + 0.5));
        wchar_t text[24];
        if (history->monthMode) {
            swprintf(text, 24, L"%d", history->label[bucket]);
        } else {
            tm_format_hour(history->label[bucket], text, 24);
        }
        RECT r;
        UINT flags;
        if (i == 0) {
            r.left = x;
            r.right = x + S(app, 46);
            flags = DT_LEFT | DT_TOP | DT_SINGLELINE;
        } else if (i == labels - 1) {
            r.left = x - S(app, 46);
            r.right = x;
            flags = DT_RIGHT | DT_TOP | DT_SINGLELINE;
        } else {
            r.left = x - S(app, 28);
            r.right = x + S(app, 28);
            flags = DT_CENTER | DT_TOP | DT_SINGLELINE;
        }
        r.top = plot.bottom + S(app, 3);
        r.bottom = r.top + S(app, 16);
        tm_text(dc, text, r, app->theme.mono_small, TM_COLOR_INK_3, flags);
    }
}

static void DrawHistory(App* app, HDC dc, const RECT* client) {
    const int width = client->right;
    const int margin = S(app, 28);
    const int header = S(app, 64);
    const int contentTop = header + S(app, 20);
    const int footerTop = client->bottom - S(app, 48);
    const int32_t today = tm_today();

    const int dayActive = !app->histMonth && app->anchorDay == today;
    const int monthActive = app->histMonth &&
                            tm_month_start(app->anchorDay) == tm_month_start(today);
    DrawChip(app, dc, app->layout.chipDay, L"今天", dayActive, app->hoverChip == 0);
    DrawChip(app, dc, app->layout.chipMonth, L"本月", monthActive, app->hoverChip == 1);

    const int latest = AtLatestPeriod(app);
    DrawArrowButton(app, dc, app->layout.prev, 0, app->hoverPrev, 1);
    DrawArrowButton(app, dc, app->layout.next, 1, app->hoverNext, !latest);

    wchar_t period[64];
    if (app->histMonth)
        tm_format_month(app->anchorDay, period, 64);
    else
        tm_format_date(app->anchorDay, period, 64);
    tm_text(dc, period, app->layout.periodTitle, app->theme.title, TM_COLOR_INK,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const int blockWidth = S(app, 180);
    const int blockGap = S(app, 24);
    RECT upBlock = {width - margin - blockWidth, contentTop, width - margin,
                    contentTop + S(app, 44)};
    RECT downBlock = {upBlock.left - blockGap - blockWidth, contentTop,
                      upBlock.left - blockGap, contentTop + S(app, 44)};
    DrawStatBlock(dc, app, downBlock, L"下载合计", app->history->totalDown);
    DrawStatBlock(dc, app, upBlock, L"上传合计", app->history->totalUp);

    RECT chart = {margin, contentTop + S(app, 52), width - margin,
                  contentTop + S(app, 52) + S(app, 300)};
    if (chart.bottom > footerTop - S(app, 90)) chart.bottom = footerTop - S(app, 90);
    tm_card(dc, &chart, S(app, 10));
    DrawCardHeading(app, dc, chart,
                    app->histMonth ? L"每日流量" : L"每小时流量",
                    L"按网卡口径 · 本地时间");
    DrawLegend(dc, app, chart.right - S(app, 20), chart.top + S(app, 38));

    RECT plot = {chart.left + S(app, 20), chart.top + S(app, 64),
                 chart.right - S(app, 20), chart.bottom - S(app, 40)};
    tm_draw_bars(dc, plot, app->history->up, app->history->down, app->history->count,
                 &app->theme);
    DrawHistoryAxis(dc, app, plot, app->history);

    if (app->history->totalUp + app->history->totalDown == 0) {
        RECT empty = {plot.left, plot.top, plot.right, plot.bottom};
        tm_text(dc, L"这个时段还没有流量记录。", empty, app->theme.small,
                TM_COLOR_INK_3, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    RECT apps = {margin, chart.bottom + S(app, 16), width - margin, footerTop - S(app, 8)};
    if (apps.bottom - apps.top < S(app, 80)) return;
    tm_card(dc, &apps, S(app, 10));
    DrawCardHeading(app, dc, apps, L"应用排行", L"按应用口径 · 当前时段");
    const int tableTop = apps.top + S(app, 20) + S(app, 40);
    if (!app->snap->appBytesAvailable) {
        DrawEmptyText(app, dc, apps, tableTop,
                      L"应用排行需要管理员权限。",
                      L"切换回仪表盘仍可查看网卡速度。");
    } else if (app->history->appCount == 0) {
        DrawEmptyText(app, dc, apps, tableTop,
                      L"这个时段还没有应用流量记录。",
                      L"选择其他日期，或保持程序运行以开始采集。");
    } else {
        DrawAppTable(app, dc, apps, app->history->apps, app->history->appCount,
                     app->history->appTotalUp, app->history->appTotalDown);
    }
}

/* ---------------------------------------------------------------- footer */

static void DrawFooter(App* app, HDC dc, const RECT* client) {
    const int margin = S(app, 28);
    RECT r = {margin, client->bottom - S(app, 48), client->right - margin,
              client->bottom - S(app, 16)};

    const wchar_t* left = NULL;
    COLORREF color = TM_COLOR_INK_3;
    if (app->snap->error[0]) {
        left = app->snap->error;
        color = (!app->snap->etw && app->snap->running) ? TM_COLOR_WARNING : TM_COLOR_ERROR;
    } else if (app->histError[0]) {
        left = app->histError;
        color = TM_COLOR_WARNING;
    }
    wchar_t pathLine[640];
    if (!left) {
        swprintf(pathLine, 640, L"数据仅保存在本机 · %ls", app->snap->dbPath);
        left = pathLine;
    }

    RECT leftRect = {r.left, r.top, r.left + (r.right - r.left) * 58 / 100, r.bottom};
    tm_text(dc, left, leftRect, app->theme.tiny, color,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    tm_text(dc, L"网卡总量含系统与协议开销，不可与应用合计相加", r, app->theme.tiny,
            TM_COLOR_INK_3, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

/* ----------------------------------------------------------------- paint */

static void Paint(App* app, HDC dc) {
    RECT client;
    GetClientRect(app->hwnd, &client);
    tm_fill_rect(dc, &client, TM_COLOR_BG);
    LayoutChrome(app, &client);
    DrawHeader(app, dc, &client);
    if (app->view == 0)
        DrawDashboard(app, dc, &client);
    else
        DrawHistory(app, dc, &client);
    DrawFooter(app, dc, &client);
}

static void FreeBackbuffer(App* app) {
    if (app->memdc) {
        if (app->oldbmp) SelectObject(app->memdc, app->oldbmp);
        if (app->membmp) DeleteObject(app->membmp);
        DeleteDC(app->memdc);
    }
    app->memdc = NULL;
    app->membmp = NULL;
    app->oldbmp = NULL;
    app->bw = 0;
    app->bh = 0;
}

static void EnsureBackbuffer(App* app, int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (app->memdc && app->bw == width && app->bh == height) return;
    FreeBackbuffer(app);
    HDC screen = GetDC(app->hwnd);
    if (!screen) return;
    app->memdc = CreateCompatibleDC(screen);
    app->membmp = CreateCompatibleBitmap(screen, width, height);
    ReleaseDC(app->hwnd, screen);
    if (!app->memdc || !app->membmp) {
        FreeBackbuffer(app);
        return;
    }
    app->oldbmp = SelectObject(app->memdc, app->membmp);
    app->bw = width;
    app->bh = height;
}

/* --------------------------------------------------------------- runtime */

static void RefreshHistory(App* app) {
    if (!app->engine || !app->history) return;
    if (app->anchorDay <= 0) app->anchorDay = tm_today();
    wchar_t error[256] = {0};
    if (tm_history(app->engine, app->histMonth, app->anchorDay, app->history,
                   error, 256) != 0) {
        CopyString(app->histError, 256, error[0] ? error : L"无法查询历史记录");
    } else {
        app->histError[0] = L'\0';
    }
}

static void UpdateHover(App* app, int x, int y) {
    RECT client;
    GetClientRect(app->hwnd, &client);
    LayoutChrome(app, &client);
    POINT p = {x, y};

    int nav = -1;
    if (PtInRect(&app->layout.navDash, p)) nav = 0;
    else if (PtInRect(&app->layout.navHist, p)) nav = 1;

    int chip = -1;
    int prev = 0;
    int next = 0;
    if (app->view == 1) {
        if (PtInRect(&app->layout.chipDay, p)) chip = 0;
        else if (PtInRect(&app->layout.chipMonth, p)) chip = 1;
        prev = PtInRect(&app->layout.prev, p) ? 1 : 0;
        next = (PtInRect(&app->layout.next, p) && !AtLatestPeriod(app)) ? 1 : 0;
    }

    if (nav != app->hoverNav || chip != app->hoverChip ||
        prev != app->hoverPrev || next != app->hoverNext) {
        app->hoverNav = nav;
        app->hoverChip = chip;
        app->hoverPrev = prev;
        app->hoverNext = next;
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}

static void OnClick(App* app, int x, int y) {
    RECT client;
    GetClientRect(app->hwnd, &client);
    LayoutChrome(app, &client);
    POINT p = {x, y};

    if (PtInRect(&app->layout.navDash, p)) {
        if (app->view != 0) {
            app->view = 0;
            InvalidateRect(app->hwnd, NULL, FALSE);
        }
        return;
    }
    if (PtInRect(&app->layout.navHist, p)) {
        if (app->view != 1) {
            app->view = 1;
            if (app->anchorDay <= 0) app->anchorDay = tm_today();
            RefreshHistory(app);
            InvalidateRect(app->hwnd, NULL, FALSE);
        }
        return;
    }
    if (app->view != 1) return;

    if (PtInRect(&app->layout.chipDay, p)) {
        app->histMonth = 0;
        app->anchorDay = tm_today();
        RefreshHistory(app);
    } else if (PtInRect(&app->layout.chipMonth, p)) {
        app->histMonth = 1;
        app->anchorDay = tm_today();
        RefreshHistory(app);
    } else if (PtInRect(&app->layout.prev, p)) {
        app->anchorDay = app->histMonth ? tm_prev_month(app->anchorDay)
                                        : tm_prev_day(app->anchorDay);
        RefreshHistory(app);
    } else if (PtInRect(&app->layout.next, p) && !AtLatestPeriod(app)) {
        app->anchorDay = app->histMonth ? tm_next_month(app->anchorDay)
                                        : tm_next_day(app->anchorDay);
        RefreshHistory(app);
    } else {
        return;
    }
    InvalidateRect(app->hwnd, NULL, FALSE);
}

static void ShowMainWindow(App* app);

/* ------------------------------------------------------------ mini window */

static void SurfaceFree(Surface* s) {
    if (s->dc) {
        if (s->old) SelectObject(s->dc, s->old);
        if (s->bmp) DeleteObject(s->bmp);
        DeleteDC(s->dc);
    }
    memset(s, 0, sizeof(*s));
}

static void SurfaceEnsure(Surface* s, HWND hwnd, int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (s->dc && s->w == width && s->h == height) return;
    SurfaceFree(s);
    HDC screen = GetDC(hwnd);
    if (!screen) return;
    s->dc = CreateCompatibleDC(screen);
    s->bmp = CreateCompatibleBitmap(screen, width, height);
    ReleaseDC(hwnd, screen);
    if (!s->dc || !s->bmp) {
        SurfaceFree(s);
        return;
    }
    s->old = SelectObject(s->dc, s->bmp);
    s->w = width;
    s->h = height;
}

static int WatchIndex(const App* app, const wchar_t* path) {
    for (int i = 0; i < app->watchTotal; ++i) {
        if (_wcsicmp(app->watchPaths[i], path) == 0) return i;
    }
    return -1;
}

static void WatchAdd(App* app, const wchar_t* line) {
    while (*line == L' ' || *line == L'\t') ++line;
    wchar_t path[TM_PATH_CHARS];
    CopyString(path, TM_PATH_CHARS, line);
    size_t length = wcslen(path);
    while (length && (path[length - 1] == L'\r' || path[length - 1] == L' ' ||
                      path[length - 1] == L'\t')) {
        path[--length] = L'\0';
    }
    if (!length || app->watchTotal >= TM_MAX_WATCH || WatchIndex(app, path) >= 0) return;
    CopyString(app->watchPaths[app->watchTotal++], TM_PATH_CHARS, path);
}

static void WatchPush(App* app) {
    wchar_t joined[WATCH_TEXT_CHARS] = {0};
    for (int i = 0; i < app->watchTotal; ++i) {
        if (i) wcscat(joined, L"\n");
        wcscat(joined, app->watchPaths[i]);
    }
    if (app->engine) tm_set_watch(app->engine, joined);
}

static void WatchSave(const App* app) {
    if (!app->watchFile[0]) return;
    wchar_t joined[WATCH_TEXT_CHARS + 32];
    swprintf(joined, WATCH_TEXT_CHARS + 32, L"@alpha=%d\n@skin=%d", app->miniAlpha, app->miniSkin);
    for (int i = 0; i < app->watchTotal; ++i) {
        wcscat(joined, L"\n");
        wcscat(joined, app->watchPaths[i]);
    }
    const int length = (int)wcslen(joined);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, joined, length, NULL, 0, NULL, NULL);
    if (bytes <= 0) return;
    char* utf8 = (char*)malloc((size_t)bytes);
    if (!utf8) return;
    WideCharToMultiByte(CP_UTF8, 0, joined, length, utf8, bytes, NULL, NULL);
    HANDLE file = CreateFileW(app->watchFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, utf8, (DWORD)bytes, &written, NULL);
        CloseHandle(file);
    }
    free(utf8);
}

static void WatchApply(App* app) {
    WatchPush(app);
    WatchSave(app);
}

static void WatchLoad(App* app) {
    if (!app->watchFile[0]) return;
    HANDLE file = CreateFileW(app->watchFile, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    const DWORD size = GetFileSize(file, NULL);
    if (size == 0 || size > (DWORD)(WATCH_TEXT_CHARS * 3)) {
        CloseHandle(file);
        return;
    }
    char* bytes = (char*)malloc(size);
    DWORD got = 0;
    const BOOL ok = bytes && ReadFile(file, bytes, size, &got, NULL);
    CloseHandle(file);
    if (ok && got) {
        const int chars = MultiByteToWideChar(CP_UTF8, 0, bytes, (int)got, NULL, 0);
        wchar_t* text = chars > 0 ? (wchar_t*)malloc((size_t)(chars + 1) * sizeof(wchar_t)) : NULL;
        if (text) {
            MultiByteToWideChar(CP_UTF8, 0, bytes, (int)got, text, chars);
            text[chars] = L'\0';
            wchar_t* cursor = text;
            if (*cursor == 0xFEFF) ++cursor;
            while (*cursor) {
                wchar_t* eol = wcschr(cursor, L'\n');
                if (eol) *eol = L'\0';
                if (wcsncmp(cursor, L"@alpha=", 7) == 0) {
                    const int alpha = _wtoi(cursor + 7);
                    if (alpha >= MINI_ALPHA_MIN && alpha <= 100) app->miniAlpha = alpha;
                } else if (wcsncmp(cursor, L"@skin=", 6) == 0) {
                    const int skin = _wtoi(cursor + 6);
                    if (skin >= 0 && skin < MINI_SKIN_COUNT) {
                        app->miniSkin = skin;
                        tm_set_palette(skin);
                    }
                } else {
                    WatchAdd(app, cursor);
                }
                if (!eol) break;
                cursor = eol + 1;
            }
        }
        free(text);
    }
    free(bytes);
    WatchPush(app);
}

static int MiniHeight(const App* app) {
    const int rows = app->watchTotal > 0 ? app->watchTotal : 1;
    return S(app, MINI_LIST_TOP) + S(app, MINI_HEADER) + rows * S(app, MINI_ROW) +
           S(app, MINI_FOOTER);
}

static void MiniPlace(App* app) {
    if (!app->mini) return;
    RECT work;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) return;
    const int margin = S(app, 16);
    const int width = S(app, MINI_WIDTH);
    const int height = MiniHeight(app);
    SetWindowPos(app->mini, HWND_TOPMOST, work.right - width - margin,
                 work.bottom - height - margin, width, height, SWP_NOACTIVATE);
    InvalidateRect(app->mini, NULL, FALSE);
}

static void MiniApplyAlpha(App* app) {
    if (app->mini) {
        SetLayeredWindowAttributes(app->mini, 0, (BYTE)(app->miniAlpha * 255 / 100), LWA_ALPHA);
    }
}

static void MiniMetric(HDC dc, App* app, int x, int y, int width,
                       const wchar_t* label, const wchar_t* value, const wchar_t* unit,
                       COLORREF color) {
    RECT labelRect = {x, y, x + width, y + S(app, 16)};
    tm_text(dc, label, labelRect, app->theme.tiny, TM_COLOR_INK_3, DT_LEFT | DT_TOP | DT_SINGLELINE);

    RECT valueRect = {x, y + S(app, 16), x + width, y + S(app, 56)};
    tm_text(dc, value, valueRect, app->theme.display_xl, color, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
    const int valueWidth = TextWidth(dc, value, app->theme.display_xl);

    RECT unitRect = {x + valueWidth + S(app, 4), y + S(app, 16), x + width, y + S(app, 56)};
    tm_text(dc, unit, unitRect, app->theme.body, TM_COLOR_INK_2, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
}

static void MiniPaint(App* app, HDC dc, const RECT* client) {
    const int width = client->right;
    const int pad = S(app, 14);
    tm_fill_rect(dc, client, TM_COLOR_BG);
    RECT card = {client->left + 1, client->top + 1, client->right - 1, client->bottom - 1};
    tm_fill_round(dc, &card, S(app, 12), TM_COLOR_SURFACE);
    tm_stroke_round(dc, &card, S(app, 12), TM_COLOR_BORDER, 1);

    RECT titleRect = {pad, pad - S(app, 2), width - pad, pad + S(app, 16)};
    tm_text(dc, L"流量监控", titleRect, app->theme.small, TM_COLOR_INK_3,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    wchar_t downValue[32], downUnit[16], upValue[32], upUnit[16];
    tm_split_rate(app->snap->downPerSec, downValue, 32, downUnit, 16);
    tm_split_rate(app->snap->upPerSec, upValue, 32, upUnit, 16);
    const int half = (width - 2 * pad) / 2;
    const int metricTop = pad + S(app, 22);
    MiniMetric(dc, app, pad, metricTop, half - S(app, 6), L"下载", downValue, downUnit,
               TM_COLOR_DOWN);
    MiniMetric(dc, app, pad + half, metricTop, half - S(app, 6), L"上传", upValue, upUnit,
               TM_COLOR_UP);

    const int listTop = S(app, MINI_LIST_TOP);
    const int headH = S(app, MINI_HEADER);
    const int rowH = S(app, MINI_ROW);
    const int upRight = width - pad;
    const int upLeft = upRight - S(app, 80);
    const int downRight = upLeft - S(app, 10);
    const int downLeft = downRight - S(app, 80);
    const int nameRight = downLeft - S(app, 10);

    RECT nameHead = {pad, listTop, nameRight, listTop + headH};
    RECT downHead = {downLeft, listTop, downRight, listTop + headH};
    RECT upHead = {upLeft, listTop, upRight, listTop + headH};
    tm_text(dc, L"程序", nameHead, app->theme.tiny, TM_COLOR_INK_3,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    tm_text(dc, L"下载", downHead, app->theme.tiny, TM_COLOR_INK_3,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    tm_text(dc, L"上传", upHead, app->theme.tiny, TM_COLOR_INK_3,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    tm_line(dc, pad, listTop + headH - 1, upRight, listTop + headH - 1, TM_COLOR_BORDER, 1);

    const int rowsTop = listTop + headH;
    if (app->watchTotal == 0) {
        RECT empty = {pad, rowsTop, upRight, rowsTop + rowH};
        tm_text(dc, L"未选择程序 · 右键托盘图标添加", empty, app->theme.small, TM_COLOR_INK_3,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    for (int i = 0; i < app->watchTotal; ++i) {
        const int y = rowsTop + i * rowH;
        const wchar_t* slash = wcsrchr(app->watchPaths[i], L'\\');
        RECT nameRect = {pad, y, nameRight, y + rowH};
        RECT downRect = {downLeft, y, downRight, y + rowH};
        RECT upRect = {upLeft, y, upRight, y + rowH};
        tm_text(dc, slash ? slash + 1 : app->watchPaths[i], nameRect, app->theme.body,
                TM_COLOR_INK, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (app->snap->etw && i < app->snap->watchCount) {
            wchar_t buffer[32];
            tm_format_rate(app->snap->watch[i].downPerSec, buffer, 32);
            tm_text(dc, buffer, downRect, app->theme.mono, TM_COLOR_INK_2,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            tm_format_rate(app->snap->watch[i].upPerSec, buffer, 32);
            tm_text(dc, buffer, upRect, app->theme.mono, TM_COLOR_INK_2,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else {
            tm_text(dc, L"—", downRect, app->theme.mono, TM_COLOR_INK_3,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            tm_text(dc, L"—", upRect, app->theme.mono, TM_COLOR_INK_3,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    RECT footer = {pad, client->bottom - S(app, MINI_FOOTER), upRight, client->bottom - S(app, 8)};
    if (app->snap->etw) {
        tm_text(dc, L"左键打开主窗口", footer, app->theme.tiny, TM_COLOR_INK_3,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        tm_text(dc, L"按程序统计需要管理员权限", footer, app->theme.tiny, TM_COLOR_WARNING,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}


static void MiniCandidates(App* app) {
    app->candidateTotal = 0;
    for (int i = 0; i < app->snap->appCount && app->candidateTotal < MINI_CANDIDATES; ++i) {
        const wchar_t* path = app->snap->apps[i].path;
        if (!path[0]) continue;
        int known = 0;
        for (int j = 0; j < app->candidateTotal; ++j) {
            if (_wcsicmp(app->candidates[j], path) == 0) known = 1;
        }
        if (!known) CopyString(app->candidates[app->candidateTotal++], TM_PATH_CHARS, path);
    }
    for (int i = 0; i < app->watchTotal && app->candidateTotal < MINI_CANDIDATES; ++i) {
        int known = 0;
        for (int j = 0; j < app->candidateTotal; ++j) {
            if (_wcsicmp(app->candidates[j], app->watchPaths[i]) == 0) known = 1;
        }
        if (!known) CopyString(app->candidates[app->candidateTotal++], TM_PATH_CHARS, app->watchPaths[i]);
    }
}

static HMENU MiniProgramMenu(App* app) {
    MiniCandidates(app);
    HMENU menu = CreatePopupMenu();
    if (!menu) return NULL;
    if (app->candidateTotal == 0) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"今天还没有应用流量");
    }
    for (int i = 0; i < app->candidateTotal; ++i) {
        const wchar_t* path = app->candidates[i];
        const wchar_t* slash = wcsrchr(path, L'\\');
        const UINT flags = MF_STRING | (WatchIndex(app, path) >= 0 ? MF_CHECKED : 0);
        AppendMenuW(menu, flags, MINI_PICK_BASE + i, slash ? slash + 1 : path);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (app->watchTotal ? 0 : MF_GRAYED), MINI_CLEAR_ID, L"清除全部");
    return menu;
}

static HMENU MiniOpacityMenu(App* app) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return NULL;
    for (int i = 0; i < (int)(sizeof(kMiniAlphaSteps) / sizeof(kMiniAlphaSteps[0])); ++i) {
        wchar_t label[16];
        swprintf(label, 16, L"%d%%", kMiniAlphaSteps[i]);
        const UINT flags = MF_STRING | (kMiniAlphaSteps[i] == app->miniAlpha ? MF_CHECKED : 0);
        AppendMenuW(menu, flags, MINI_ALPHA_BASE + i, label);
    }
    return menu;
}

static HMENU MiniSkinMenu(App* app) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return NULL;
    for (int i = 0; i < MINI_SKIN_COUNT; ++i) {
        const UINT flags = MF_STRING | (i == app->miniSkin ? MF_CHECKED : 0);
        AppendMenuW(menu, flags, MINI_SKIN_BASE + i, tm_palette_name(i));
    }
    return menu;
}

static int MiniHandleCommand(App* app, UINT command) {
    const UINT alphaCount = (UINT)(sizeof(kMiniAlphaSteps) / sizeof(kMiniAlphaSteps[0]));
    if (command >= MINI_SKIN_BASE && command < MINI_SKIN_BASE + (UINT)MINI_SKIN_COUNT) {
        app->miniSkin = (int)(command - MINI_SKIN_BASE);
        tm_set_palette(app->miniSkin);
        WatchSave(app);
        if (app->mini) InvalidateRect(app->mini, NULL, FALSE);
        if (app->hwnd) InvalidateRect(app->hwnd, NULL, FALSE);
        return 1;
    }
    if (command >= MINI_ALPHA_BASE && command < MINI_ALPHA_BASE + alphaCount) {
        app->miniAlpha = kMiniAlphaSteps[command - MINI_ALPHA_BASE];
        MiniApplyAlpha(app);
        WatchSave(app);
        return 1;
    }
    if (command == MINI_CLEAR_ID) {
        app->watchTotal = 0;
        WatchApply(app);
        MiniPlace(app);
        return 1;
    }
    if (command >= MINI_PICK_BASE && command < MINI_PICK_BASE + (UINT)app->candidateTotal) {
        const wchar_t* path = app->candidates[command - MINI_PICK_BASE];
        const int index = WatchIndex(app, path);
        if (index >= 0) {
            memmove(app->watchPaths[index], app->watchPaths[index + 1],
                    (size_t)(app->watchTotal - index - 1) * sizeof(app->watchPaths[0]));
            app->watchTotal--;
        } else if (app->watchTotal < TM_MAX_WATCH) {
            CopyString(app->watchPaths[app->watchTotal++], TM_PATH_CHARS, path);
        }
        WatchApply(app);
        MiniPlace(app);
        return 1;
    }
    return 0;
}

static void MiniEnter(App* app) {
    if (!app->mini) {
        app->mini = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW | WS_EX_LAYERED, MINI_CLASS,
                                    APP_NAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL, app->instance, app);
        if (!app->mini) return;
    }
    app->miniMode = 1;
    MiniApplyAlpha(app);
    MiniPlace(app);
    ShowWindow(app->mini, SW_SHOWNOACTIVATE);
    ShowWindow(app->hwnd, SW_HIDE);
}

static void LeaveMini(App* app) {
    app->miniMode = 0;
    if (app->mini) ShowWindow(app->mini, SW_HIDE);
}

static void ShowMainWindow(App* app) {
    LeaveMini(app);
    if (IsIconic(app->hwnd)) ShowWindow(app->hwnd, SW_RESTORE);
    ShowWindow(app->hwnd, SW_SHOW);
    SetForegroundWindow(app->hwnd);
    InvalidateRect(app->hwnd, NULL, FALSE);
}

static void Shutdown(App* app) {
    if (app->shutdown) return;
    app->shutdown = 1;
    KillTimer(app->hwnd, TIMER_ID);
    tm_tray_remove();
    if (app->mini) DestroyWindow(app->mini);
    if (app->engine) tm_stop(app->engine);
    FreeBackbuffer(app);
    tm_theme_free(&app->theme);
    free(app->snap);
    app->snap = NULL;
    free(app->history);
    app->history = NULL;
    if (app->engine) {
        tm_destroy(app->engine);
        app->engine = NULL;
    }
}

static void UpdateTrayTip(App* app) {
    if (!app->trayAdded || !app->snap) return;
    wchar_t down[32];
    wchar_t up[32];
    wchar_t tip[128];
    tm_format_rate(app->snap->downPerSec, down, 32);
    tm_format_rate(app->snap->upPerSec, up, 32);
    swprintf(tip, 128, L"%ls · ↓ %ls · ↑ %ls", APP_NAME, down, up);
    tm_tray_update(tip);
}

/* --------------------------------------------------------------- window */

static LRESULT CALLBACK MiniWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = (App*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (message) {
        case WM_CREATE: {
            CREATESTRUCTW* create = (CREATESTRUCTW*)lParam;
            app = (App*)create->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (app && app->snap) {
                RECT client;
                GetClientRect(hwnd, &client);
                SurfaceEnsure(&app->miniBuf, hwnd, client.right, client.bottom);
                if (app->miniBuf.dc) {
                    MiniPaint(app, app->miniBuf.dc, &client);
                    BitBlt(dc, 0, 0, client.right, client.bottom, app->miniBuf.dc, 0, 0, SRCCOPY);
                }
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ACTIVATE:
            if (app && app->miniMode && LOWORD(wParam) != WA_INACTIVE) {
                ShowMainWindow(app);
            }
            return 0;
        case WM_LBUTTONUP:
            if (app) ShowMainWindow(app);
            return 0;
        case WM_KEYDOWN:
            if (app && wParam == VK_ESCAPE) ShowMainWindow(app);
            return 0;
        case WM_DESTROY:
            if (app) {
                SurfaceFree(&app->miniBuf);
                app->mini = NULL;
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = (App*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (message) {
        case WM_CREATE: {
            CREATESTRUCTW* create = (CREATESTRUCTW*)lParam;
            app = (App*)create->lpCreateParams;
            app->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);
            app->theme.dpi = DpiOfWindow(hwnd);
            tm_theme_init(&app->theme, app->theme.dpi);
            app->icon = LoadIconW(app->instance, MAKEINTRESOURCEW(101));
            if (!app->icon) app->icon = LoadIconW(NULL, IDI_APPLICATION);
            app->taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
            SetTimer(hwnd, TIMER_ID, 1000, NULL);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO* info = (MINMAXINFO*)lParam;
            const int dpi = app && app->theme.dpi ? app->theme.dpi : 96;
            info->ptMinTrackSize.x = MulDiv(1000, dpi, 96);
            info->ptMinTrackSize.y = MulDiv(700, dpi, 96);
            return 0;
        }
        case WM_SIZE:
            FreeBackbuffer(app);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (app && app->snap) {
                RECT client;
                GetClientRect(hwnd, &client);
                EnsureBackbuffer(app, client.right, client.bottom);
                if (app->memdc) {
                    Paint(app, app->memdc);
                    BitBlt(dc, 0, 0, client.right, client.bottom, app->memdc, 0, 0, SRCCOPY);
                } else {
                    Paint(app, dc);
                }
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DPICHANGED: {
            if (!app) break;
            tm_theme_free(&app->theme);
            tm_theme_init(&app->theme, (int)HIWORD(wParam));
            FreeBackbuffer(app);
            RECT* suggested = (RECT*)lParam;
            SetWindowPos(hwnd, NULL, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!app) break;
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            UpdateHover(app, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        }
        case WM_MOUSELEAVE:
            if (!app) break;
            app->hoverNav = -1;
            app->hoverChip = -1;
            app->hoverPrev = 0;
            app->hoverNext = 0;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_SETCURSOR: {
            if (!app) break;
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            RECT client;
            GetClientRect(hwnd, &client);
            LayoutChrome(app, &client);
            int clickable = PtInRect(&app->layout.navDash, p) ||
                            PtInRect(&app->layout.navHist, p);
            if (app->view == 1) {
                clickable = clickable || PtInRect(&app->layout.chipDay, p) ||
                            PtInRect(&app->layout.chipMonth, p) ||
                            PtInRect(&app->layout.prev, p) ||
                            (PtInRect(&app->layout.next, p) && !AtLatestPeriod(app));
            }
            if (clickable) {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONUP:
            if (app) OnClick(app, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_KEYDOWN:
            if (!app) break;
            if (wParam == VK_TAB) {
                app->view = app->view ? 0 : 1;
                if (app->view) {
                    if (app->anchorDay <= 0) app->anchorDay = tm_today();
                    RefreshHistory(app);
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (app->view == 1 && (wParam == VK_LEFT || wParam == VK_RIGHT)) {
                if (wParam == VK_LEFT) {
                    app->anchorDay = app->histMonth ? tm_prev_month(app->anchorDay)
                                                    : tm_prev_day(app->anchorDay);
                } else if (!AtLatestPeriod(app)) {
                    app->anchorDay = app->histMonth ? tm_next_month(app->anchorDay)
                                                    : tm_next_day(app->anchorDay);
                }
                RefreshHistory(app);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            break;
        case WM_TIMER:
            if (!app || wParam != TIMER_ID) break;
            tm_tick(app->engine, app->snap);
            if (app->view == 1) RefreshHistory(app);
            UpdateTrayTip(app);
            ++app->ticks;
            InvalidateRect(hwnd, NULL, FALSE);
            if (app->miniMode && app->mini) InvalidateRect(app->mini, NULL, FALSE);
            if (app->exitAfter > 0 && app->ticks >= app->exitAfter) {
                app->quitting = 1;
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_CLOSE:
            if (!app) break;
            if (app->quitting) {
                DestroyWindow(hwnd);
            } else {
                ShowWindow(hwnd, SW_HIDE);
            }
            return 0;
        case WM_COMMAND:
            if (!app) break;
            switch (LOWORD(wParam)) {
                case TM_TRAY_OPEN:
                    ShowMainWindow(app);
                    return 0;
                case TM_TRAY_MINI:
                    if (app->miniMode) ShowMainWindow(app);
                    else MiniEnter(app);
                    return 0;
                case TM_TRAY_EXIT:
                    app->quitting = 1;
                    DestroyWindow(hwnd);
                    return 0;
                default:
                    if (MiniHandleCommand(app, LOWORD(wParam))) return 0;
                    break;
            }
            break;
        case TM_TRAY_CALLBACK:
            if (!app) break;
            {
                const UINT mouseMessage = LOWORD(lParam);
                if (mouseMessage == WM_RBUTTONUP || mouseMessage == WM_CONTEXTMENU) {
                    tm_tray_menu(hwnd, app->miniMode, MiniProgramMenu(app), MiniOpacityMenu(app),
                                 MiniSkinMenu(app));
                } else if (mouseMessage == WM_LBUTTONUP || mouseMessage == WM_LBUTTONDBLCLK) {
                    ShowMainWindow(app);
                }
            }
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wParam && app) {
                app->quitting = 1;
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_DESTROY:
            if (app) {
                Shutdown(app);
            }
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
        default:
            if (app && app->taskbarCreated && message == app->taskbarCreated) {
                app->trayAdded = tm_tray_add(hwnd, app->icon, APP_NAME);
                UpdateTrayTip(app);
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

/* ------------------------------------------------------------------ main */

static void ParseArguments(int* argc, wchar_t*** argv, int* noEtw, wchar_t* dbPath,
                           int dbChars, int* exitAfter, int* startMini) {
    *argv = CommandLineToArgvW(GetCommandLineW(), argc);
    if (!*argv) return;
    for (int i = 1; i < *argc; ++i) {
        if (wcscmp((*argv)[i], L"--no-etw") == 0) {
            *noEtw = 1;
        } else if (wcscmp((*argv)[i], L"--mini") == 0) {
            *startMini = 1;
        } else if (wcscmp((*argv)[i], L"--db") == 0 && i + 1 < *argc) {
            CopyString(dbPath, dbChars, (*argv)[++i]);
        } else if (wcscmp((*argv)[i], L"--exit-after") == 0 && i + 1 < *argc) {
            const long seconds = wcstol((*argv)[++i], NULL, 10);
            if (seconds > 0 && seconds < 86400) *exitAfter = (int)seconds;
        }
    }
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR commandLine, int show) {
    (void)previous;
    (void)commandLine;
    (void)show;

    int argc = 0;
    wchar_t** argv = NULL;
    int noEtw = 0;
    int exitAfter = 0;
    int startMini = 0;
    wchar_t dbOption[TM_PATH_CHARS] = {0};
    ParseArguments(&argc, &argv, &noEtw, dbOption, TM_PATH_CHARS, &exitAfter, &startMini);

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\TrafficMon.UI");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(APP_CLASS, NULL);
        if (existing) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        } else {
            MessageBoxW(NULL, L"流量台已在运行。", APP_NAME, MB_ICONINFORMATION);
        }
        CloseHandle(mutex);
        if (argv) LocalFree(argv);
        return 0;
    }

    wchar_t dbPath[TM_PATH_CHARS];
    CopyString(dbPath, TM_PATH_CHARS, dbOption);
    wchar_t error[TM_ERROR_CHARS] = {0};
    if (!dbPath[0]) {
        if (tm_default_db_path(dbPath, TM_PATH_CHARS, error, TM_ERROR_CHARS) != 0) {
            MessageBoxW(NULL, error[0] ? error : L"无法确定数据库位置。", APP_NAME, MB_ICONERROR);
            if (mutex) CloseHandle(mutex);
            if (argv) LocalFree(argv);
            return 1;
        }
    }

    TmEngine* engine = tm_create(dbPath, noEtw, error, TM_ERROR_CHARS);
    if (!engine) {
        MessageBoxW(NULL, error[0] ? error : L"无法打开数据库。", APP_NAME, MB_ICONERROR);
        if (mutex) CloseHandle(mutex);
        if (argv) LocalFree(argv);
        return 1;
    }

    App* app = (App*)calloc(1, sizeof(App));
    if (!app) {
        tm_destroy(engine);
        if (mutex) CloseHandle(mutex);
        if (argv) LocalFree(argv);
        return 1;
    }
    app->instance = instance;
    app->engine = engine;
    app->snap = (TmSnapshot*)calloc(1, sizeof(TmSnapshot));
    app->history = (TmHistory*)calloc(1, sizeof(TmHistory));
    app->miniAlpha = 100;
    app->view = 0;
    app->histMonth = 0;
    app->anchorDay = tm_today();
    app->hoverNav = -1;
    app->hoverChip = -1;
    app->exitAfter = exitAfter;
    CopyString(app->watchFile, TM_PATH_CHARS, dbPath);
    wchar_t* dbSlash = wcsrchr(app->watchFile, L'\\');
    wchar_t* dbSlashFwd = wcsrchr(app->watchFile, L'/');
    if (dbSlashFwd > dbSlash) dbSlash = dbSlashFwd;
    CopyString(dbSlash ? dbSlash + 1 : app->watchFile,
               TM_PATH_CHARS - (int)(dbSlash ? dbSlash + 1 - app->watchFile : 0), WATCH_FILE_NAME);
    if (!app->snap || !app->history) {
        free(app->snap);
        free(app->history);
        free(app);
        tm_destroy(engine);
        if (mutex) CloseHandle(mutex);
        if (argv) LocalFree(argv);
        return 1;
    }

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = APP_CLASS;
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"无法注册窗口类。", APP_NAME, MB_ICONERROR);
        free(app->snap);
        free(app->history);
        free(app);
        tm_destroy(engine);
        if (mutex) CloseHandle(mutex);
        if (argv) LocalFree(argv);
        return 1;
    }

    WNDCLASSEXW miniClass;
    memset(&miniClass, 0, sizeof(miniClass));
    miniClass.cbSize = sizeof(miniClass);
    miniClass.style = CS_HREDRAW | CS_VREDRAW;
    miniClass.lpfnWndProc = MiniWndProc;
    miniClass.hInstance = instance;
    miniClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
    miniClass.hbrBackground = NULL;
    miniClass.lpszClassName = MINI_CLASS;
    RegisterClassExW(&miniClass);

    const int dpi = DpiOfWindow(NULL);
    int windowWidth = MulDiv(1180, dpi, 96);
    int windowHeight = MulDiv(860, dpi, 96);
    RECT work;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        const int maxW = (work.right - work.left) * 9 / 10;
        const int maxH = (work.bottom - work.top) * 9 / 10;
        if (windowWidth > maxW) windowWidth = maxW;
        if (windowHeight > maxH) windowHeight = maxH;
    }
    RECT windowRect = {0, 0, windowWidth, windowHeight};
    AdjustWindowRectEx(&windowRect, WS_OVERLAPPEDWINDOW, FALSE, 0);

    HWND hwnd = CreateWindowExW(0, APP_CLASS, APP_NAME, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                windowRect.right - windowRect.left,
                                windowRect.bottom - windowRect.top,
                                NULL, NULL, instance, app);
    if (!hwnd) {
        MessageBoxW(NULL, L"无法创建主窗口。", APP_NAME, MB_ICONERROR);
        free(app->snap);
        free(app->history);
        free(app);
        tm_destroy(engine);
        if (mutex) CloseHandle(mutex);
        if (argv) LocalFree(argv);
        return 1;
    }
    app->hwnd = hwnd;

    if (tm_start(engine) != 0) {
        TmSnapshot failure;
        tm_tick(engine, &failure);
        MessageBoxW(NULL, failure.error[0] ? failure.error : L"无法启动采集。",
                    APP_NAME, MB_ICONERROR);
        DestroyWindow(hwnd);  /* WM_DESTROY -> Shutdown() frees engine and buffers */
        free(app);
        if (mutex) CloseHandle(mutex);
        if (argv) LocalFree(argv);
        return 1;
    }

    tm_tick(engine, app->snap);
    UpdateTrayTip(app);
    app->trayAdded = tm_tray_add(hwnd, app->icon, APP_NAME);
    UpdateTrayTip(app);

    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        RECT current;
        GetWindowRect(hwnd, &current);
        const int w = current.right - current.left;
        const int h = current.bottom - current.top;
        const int x = work.left + ((work.right - work.left) - w) / 2;
        const int y = work.top + ((work.bottom - work.top) - h) / 2;
        SetWindowPos(hwnd, NULL, MaxI(work.left, x), MaxI(work.top, y), 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    WatchLoad(app);
    if (startMini) MiniEnter(app);

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (!app->shutdown) {
        Shutdown(app);
        if (IsWindow(hwnd)) DestroyWindow(hwnd);
    }
    free(app);
    if (mutex) CloseHandle(mutex);
    if (argv) LocalFree(argv);
    return 0;
}