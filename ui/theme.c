#include "theme.h"

#include <string.h>

typedef struct {
    const wchar_t* name;
    TmPalette palette;
} SkinEntry;

static const SkinEntry kSkins[] = {
    {L"纸白", {
        RGB(0xF2, 0xF4, 0xF1), RGB(0xFF, 0xFF, 0xFF), RGB(0xE8, 0xED, 0xE9),
        RGB(0xD7, 0xDF, 0xDA), RGB(0xB9, 0xC6, 0xC0),
        RGB(0x16, 0x21, 0x1D), RGB(0x55, 0x64, 0x5E), RGB(0x66, 0x75, 0x6E),
        RGB(0x0E, 0x5E, 0x56), RGB(0xDC, 0xEA, 0xE5),
        RGB(0xA8, 0x5F, 0x2A), RGB(0xF0, 0xE3, 0xD8),
        RGB(0x2F, 0x7D, 0x5A), RGB(0x9A, 0x6B, 0x12), RGB(0xB0, 0x36, 0x2D), RGB(0x3E, 0x6B, 0x7A)}},
    {L"墨夜", {
        RGB(0x14, 0x18, 0x1F), RGB(0x1E, 0x24, 0x2D), RGB(0x26, 0x2D, 0x38),
        RGB(0x2F, 0x38, 0x44), RGB(0x3C, 0x47, 0x55),
        RGB(0xE8, 0xEC, 0xF2), RGB(0xA9, 0xB4, 0xC2), RGB(0x76, 0x84, 0x94),
        RGB(0x3D, 0xD6, 0xC0), RGB(0x1B, 0x3B, 0x38),
        RGB(0xF2, 0xA6, 0x5A), RGB(0x3D, 0x2D, 0x1E),
        RGB(0x4C, 0xC3, 0x8A), RGB(0xE0, 0xB0, 0x4A), RGB(0xF0, 0x71, 0x67), RGB(0x6C, 0xB4, 0xD0)}},
    {L"海雾", {
        RGB(0xDD, 0xEB, 0xEF), RGB(0xF4, 0xFA, 0xFB), RGB(0xD2, 0xE4, 0xE9),
        RGB(0xB8, 0xD2, 0xD9), RGB(0x93, 0xB5, 0xBF),
        RGB(0x12, 0x33, 0x3B), RGB(0x3B, 0x5F, 0x68), RGB(0x5E, 0x7D, 0x85),
        RGB(0x1F, 0x7A, 0x8C), RGB(0xD0, 0xE8, 0xED),
        RGB(0x2E, 0x5E, 0xAA), RGB(0xDC, 0xE5, 0xF5),
        RGB(0x2F, 0x7D, 0x5A), RGB(0x9A, 0x6B, 0x12), RGB(0xB0, 0x36, 0x2D), RGB(0x1F, 0x7A, 0x8C)}},
    {L"琥珀", {
        RGB(0x2A, 0x20, 0x14), RGB(0x3A, 0x2C, 0x1B), RGB(0x46, 0x35, 0x20),
        RGB(0x5A, 0x44, 0x26), RGB(0x7A, 0x5E, 0x36),
        RGB(0xF6, 0xEA, 0xD4), RGB(0xC9, 0xB3, 0x8E), RGB(0x9A, 0x85, 0x62),
        RGB(0xE8, 0xB2, 0x4A), RGB(0x4A, 0x3A, 0x1C),
        RGB(0xE0, 0x7A, 0x5F), RGB(0x4D, 0x2D, 0x22),
        RGB(0x8C, 0xC0, 0x8A), RGB(0xE8, 0xB2, 0x4A), RGB(0xF0, 0x8A, 0x7A), RGB(0x9C, 0xC3, 0xC9)}},
};

#define SKIN_COUNT ((int)(sizeof(kSkins) / sizeof(kSkins[0])))

static int g_palette = 0;

int tm_palette_count(void) { return SKIN_COUNT; }

const wchar_t* tm_palette_name(int index) {
    if (index < 0 || index >= SKIN_COUNT) index = 0;
    return kSkins[index].name;
}

void tm_set_palette(int index) {
    if (index < 0 || index >= SKIN_COUNT) index = 0;
    g_palette = index;
}

int tm_palette_index(void) { return g_palette; }

const TmPalette* tm_active(void) { return &kSkins[g_palette].palette; }

int tm_scale(int dpi, int px) {
    if (dpi <= 0) dpi = 96;
    return MulDiv(px, dpi, 96);
}

static int CALLBACK FontFound(const LOGFONTW* logfont, const TEXTMETRICW* metrics,
                              DWORD type, LPARAM param) {
    (void)logfont;
    (void)metrics;
    (void)type;
    *(int*)param = 1;
    return 0;  /* one match is enough */
}

static int HasFace(const wchar_t* face) {
    LOGFONTW filter;
    memset(&filter, 0, sizeof(filter));
    filter.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(filter.lfFaceName, face, LF_FACESIZE);
    HDC dc = GetDC(NULL);
    if (!dc) return 0;
    int found = 0;
    EnumFontFamiliesExW(dc, &filter, FontFound, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

static const wchar_t* PickFace(const wchar_t* const* candidates, int count) {
    for (int i = 0; i < count; ++i) {
        if (HasFace(candidates[i])) return candidates[i];
    }
    return L"Tahoma";
}

static const wchar_t* DisplayFace(void) {
    static const wchar_t* candidates[] = {
        L"Bahnschrift", L"DIN Alternate", L"Segoe UI Semibold", L"Segoe UI", L"Tahoma"
    };
    return PickFace(candidates, (int)(sizeof(candidates) / sizeof(candidates[0])));
}

static const wchar_t* BodyFace(void) {
    static const wchar_t* candidates[] = {
        L"Microsoft YaHei UI", L"Microsoft YaHei", L"Segoe UI", L"Tahoma", L"Arial"
    };
    return PickFace(candidates, (int)(sizeof(candidates) / sizeof(candidates[0])));
}

static const wchar_t* MonoFace(void) {
    static const wchar_t* candidates[] = {
        L"Consolas", L"Cascadia Mono", L"Courier New", L"Tahoma"
    };
    return PickFace(candidates, (int)(sizeof(candidates) / sizeof(candidates[0])));
}

static HFONT MakeFont(const wchar_t* face, int px, int dpi, int weight) {
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -MulDiv(px, dpi, 96);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    return CreateFontIndirectW(&lf);
}

static void CreateAll(TmTheme* theme) {
    const wchar_t* display = DisplayFace();
    const wchar_t* body = BodyFace();
    const wchar_t* mono = MonoFace();
    const int dpi = theme->dpi;

    theme->display_xl = MakeFont(display, 34, dpi, FW_SEMIBOLD);
    theme->display_lg = MakeFont(display, 20, dpi, FW_SEMIBOLD);
    theme->display_md = MakeFont(display, 18, dpi, FW_SEMIBOLD);
    theme->display_cjk = MakeFont(body, 20, dpi, FW_SEMIBOLD);
    theme->title = MakeFont(body, 15, dpi, FW_SEMIBOLD);
    theme->body = MakeFont(body, 13, dpi, FW_NORMAL);
    theme->body_strong = MakeFont(body, 13, dpi, FW_SEMIBOLD);
    theme->small = MakeFont(body, 12, dpi, FW_NORMAL);
    theme->tiny = MakeFont(body, 11, dpi, FW_NORMAL);
    theme->mono = MakeFont(mono, 12, dpi, FW_NORMAL);
    theme->mono_small = MakeFont(mono, 11, dpi, FW_NORMAL);
    theme->mono_large = MakeFont(mono, 15, dpi, FW_NORMAL);
}

void tm_theme_init(TmTheme* theme, int dpi) {
    if (!theme) return;
    memset(theme, 0, sizeof(*theme));
    theme->dpi = dpi > 0 ? dpi : 96;
    CreateAll(theme);
}

void tm_theme_free(TmTheme* theme) {
    if (!theme) return;
    HFONT* fonts[] = {
        &theme->display_xl, &theme->display_lg, &theme->display_md,
        &theme->display_cjk,
        &theme->title, &theme->body, &theme->body_strong,
        &theme->small, &theme->tiny, &theme->mono,
        &theme->mono_small, &theme->mono_large
    };
    for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); ++i) {
        if (*fonts[i]) {
            DeleteObject(*fonts[i]);
            *fonts[i] = NULL;
        }
    }
    theme->dpi = 96;
}