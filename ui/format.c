#include "format.h"

#include <stdio.h>

static void Copy(wchar_t* dst, int cch, const wchar_t* src) {
    if (!dst || cch <= 0) return;
    wcsncpy(dst, src, (size_t)cch - 1);
    dst[cch - 1] = L'\0';
}

static void FormatValue(double value, int unitIndex, wchar_t* out, int cch) {
    if (!out || cch <= 0) return;
    if (unitIndex == 0 || value >= 100.0) {
        swprintf(out, (size_t)cch, L"%.0f", value);
    } else if (value >= 10.0) {
        swprintf(out, (size_t)cch, L"%.1f", value);
    } else {
        swprintf(out, (size_t)cch, L"%.2f", value);
    }
    out[cch - 1] = L'\0';
}

static void SplitUnits(double value, int unitIndex, int perSecond,
                       wchar_t* valueOut, int valueChars,
                       wchar_t* unitOut, int unitChars) {
    static const wchar_t* kUnits[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    FormatValue(value, unitIndex, valueOut, valueChars);
    if (perSecond) {
        swprintf(unitOut, (size_t)unitChars, L"%ls/s", kUnits[unitIndex]);
    } else {
        Copy(unitOut, unitChars, kUnits[unitIndex]);
    }
    if (unitChars > 0) unitOut[unitChars - 1] = L'\0';
}

void tm_split_rate(uint64_t bytesPerSec, wchar_t* value, int valueChars,
                   wchar_t* unit, int unitChars) {
    double amount = (double)bytesPerSec;
    int index = 0;
    while (amount >= 1024.0 && index < 4) {
        amount /= 1024.0;
        ++index;
    }
    SplitUnits(amount, index, 1, value, valueChars, unit, unitChars);
}

void tm_split_bytes(int64_t bytes, wchar_t* value, int valueChars,
                    wchar_t* unit, int unitChars) {
    double amount = bytes > 0 ? (double)bytes : 0.0;
    int index = 0;
    while (amount >= 1024.0 && index < 4) {
        amount /= 1024.0;
        ++index;
    }
    SplitUnits(amount, index, 0, value, valueChars, unit, unitChars);
}

void tm_format_rate(uint64_t bytesPerSec, wchar_t* out, int outChars) {
    wchar_t value[32];
    wchar_t unit[8];
    tm_split_rate(bytesPerSec, value, 32, unit, 8);
    swprintf(out, (size_t)outChars, L"%ls %ls", value, unit);
    if (outChars > 0) out[outChars - 1] = L'\0';
}

void tm_format_bytes(int64_t bytes, wchar_t* out, int outChars) {
    wchar_t value[32];
    wchar_t unit[8];
    tm_split_bytes(bytes, value, 32, unit, 8);
    swprintf(out, (size_t)outChars, L"%ls %ls", value, unit);
    if (outChars > 0) out[outChars - 1] = L'\0';
}

void tm_format_percent(double percent, wchar_t* out, int outChars) {
    if (!out || outChars <= 0) return;
    if (percent < 0.0) percent = 0.0;
    if (percent > 100.0) percent = 100.0;
    if (percent >= 10.0 || percent == 0.0) {
        swprintf(out, (size_t)outChars, L"%.0f%%", percent);
    } else {
        swprintf(out, (size_t)outChars, L"%.1f%%", percent);
    }
    out[outChars - 1] = L'\0';
}

void tm_format_date(int32_t day, wchar_t* out, int outChars) {
    if (!out || outChars <= 0) return;
    if (day <= 0) {
        Copy(out, outChars, L"—");
        return;
    }
    swprintf(out, (size_t)outChars, L"%d年%d月%d日",
             day / 10000, (day / 100) % 100, day % 100);
    out[outChars - 1] = L'\0';
}

void tm_format_month(int32_t day, wchar_t* out, int outChars) {
    if (!out || outChars <= 0) return;
    if (day <= 0) {
        Copy(out, outChars, L"—");
        return;
    }
    swprintf(out, (size_t)outChars, L"%d年%d月", day / 10000, (day / 100) % 100);
    out[outChars - 1] = L'\0';
}

void tm_format_hour(int hour, wchar_t* out, int outChars) {
    if (!out || outChars <= 0) return;
    if (hour < 0) hour = 0;
    if (hour > 23) hour = 23;
    swprintf(out, (size_t)outChars, L"%02d:00", hour);
    out[outChars - 1] = L'\0';
}