/* Signal Paper theme: colors, DPI scaling, and the small font set used by the C UI.
 * Only plain GDI fonts (no GDI+, which is C++-only). */
#ifndef TM_THEME_H
#define TM_THEME_H

#include <windows.h>

/* Palette: one active skin drives both windows. TM_COLOR_* read the active
 * palette, so drawing call sites stay unchanged. Default skin matches
 * design-system-signal-paper/design-tokens.json. */
typedef struct TmPalette {
    COLORREF bg;
    COLORREF surface;
    COLORREF surface_muted;
    COLORREF border;
    COLORREF border_strong;
    COLORREF ink;
    COLORREF ink_2;
    COLORREF ink_3;
    COLORREF down;
    COLORREF down_soft;
    COLORREF up;
    COLORREF up_soft;
    COLORREF success;
    COLORREF warning;
    COLORREF error;
    COLORREF info;
} TmPalette;

int tm_palette_count(void);
const wchar_t* tm_palette_name(int index);
void tm_set_palette(int index);
int tm_palette_index(void);
const TmPalette* tm_active(void);

#define TM_COLOR_BG              (tm_active()->bg)
#define TM_COLOR_SURFACE         (tm_active()->surface)
#define TM_COLOR_SURFACE_MUTED   (tm_active()->surface_muted)
#define TM_COLOR_BORDER          (tm_active()->border)
#define TM_COLOR_BORDER_STRONG   (tm_active()->border_strong)
#define TM_COLOR_INK             (tm_active()->ink)
#define TM_COLOR_INK_2           (tm_active()->ink_2)
#define TM_COLOR_INK_3           (tm_active()->ink_3)
#define TM_COLOR_DOWN            (tm_active()->down)
#define TM_COLOR_DOWN_SOFT       (tm_active()->down_soft)
#define TM_COLOR_UP              (tm_active()->up)
#define TM_COLOR_UP_SOFT         (tm_active()->up_soft)
#define TM_COLOR_SUCCESS         (tm_active()->success)
#define TM_COLOR_WARNING         (tm_active()->warning)
#define TM_COLOR_ERROR           (tm_active()->error)
#define TM_COLOR_INFO            (tm_active()->info)

typedef struct TmTheme {
    int dpi;
    HFONT display_xl;    /* 34 px Bahnschrift semibold — live rates */
    HFONT display_lg;    /* 20 px Bahnschrift semibold — history totals */
    HFONT display_md;    /* 18 px Bahnschrift semibold — Latin display text */
    HFONT display_cjk;   /* 20 px YaHei UI semibold — Chinese window title */
    HFONT title;         /* 15 px YaHei UI semibold — card titles */
    HFONT body;          /* 13 px YaHei UI — app names, nav */
    HFONT body_strong;   /* 13 px YaHei UI semibold */
    HFONT small;         /* 12 px YaHei UI — status, captions */
    HFONT tiny;          /* 11 px YaHei UI — axis, column headers, footer */
    HFONT mono;          /* 12 px Consolas — table numbers */
    HFONT mono_small;    /* 11 px Consolas — axis values */
    HFONT mono_large;    /* 15 px Consolas — history totals */
} TmTheme;

int tm_scale(int dpi, int px);
void tm_theme_init(TmTheme* theme, int dpi);
void tm_theme_free(TmTheme* theme);

#endif /* TM_THEME_H */