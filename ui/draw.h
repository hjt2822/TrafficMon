/* GDI primitives and the two chart marks used by the Signal Paper UI.
 * The caller owns the double buffer; these functions draw into an HDC. */
#ifndef TM_DRAW_H
#define TM_DRAW_H

#include <windows.h>
#include <stdint.h>
#include "theme.h"

void tm_fill_rect(HDC dc, const RECT* r, COLORREF color);
void tm_fill_round(HDC dc, const RECT* r, int radius, COLORREF color);
void tm_stroke_round(HDC dc, const RECT* r, int radius, COLORREF color, int width);
void tm_card(HDC dc, const RECT* r, int radius);
void tm_line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int width);
void tm_dot(HDC dc, int cx, int cy, int radius, COLORREF color);

/* Draws single-line text clipped to r. Pass DT_LEFT/DT_RIGHT/DT_CENTER,
 * DT_VCENTER | DT_SINGLELINE, and optionally DT_END_ELLIPSIS. */
void tm_text(HDC dc, const wchar_t* text, RECT r, HFONT font, COLORREF color, UINT flags);

/* The 60-second signal band. count == 0 draws only the baseline. */
void tm_draw_signal(HDC dc, RECT plot, const uint64_t* up, const uint64_t* down,
                    int count, const TmTheme* theme);

/* Grouped history bars (download then upload per bucket). */
void tm_draw_bars(HDC dc, RECT plot, const int64_t* up, const int64_t* down,
                  int count, const TmTheme* theme);

#endif /* TM_DRAW_H */