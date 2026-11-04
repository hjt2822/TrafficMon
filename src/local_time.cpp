#include "local_time.h"

#include <windows.h>

namespace {
bool LeapYear(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
int DaysInMonth(int y, int m) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && LeapYear(y) ? 29 : kDays[m - 1];
}
}

int32_t LocalDayForUtc(int64_t utcSeconds) {
    if (utcSeconds < -11644473600LL) return 0;
    ULARGE_INTEGER ticks;
    ticks.QuadPart = static_cast<ULONGLONG>(utcSeconds + 11644473600LL) * 10000000ULL;
    FILETIME ft{ticks.LowPart, ticks.HighPart};
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&ft, &utc) ||
        !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
        return 0;
    return local.wYear * 10000 + local.wMonth * 100 + local.wDay;
}

int32_t NextDay(int32_t day) {
    int y = day / 10000, m = day / 100 % 100, d = day % 100;
    if (++d > DaysInMonth(y, m)) { d = 1; if (++m > 12) { m = 1; ++y; } }
    return y * 10000 + m * 100 + d;
}

int32_t MonthStartOf(int32_t day) { return day / 100 * 100 + 1; }

int32_t NextMonthStart(int32_t day) {
    int y = day / 10000, m = day / 100 % 100;
    if (++m > 12) { m = 1; ++y; }
    return y * 10000 + m * 100 + 1;
}

bool ValidMonth(int32_t v) {
    int y = v / 100, m = v % 100;
    return y >= 1970 && y <= 9999 && m >= 1 && m <= 12;
}

bool ValidDay(int32_t v) {
    int y = v / 10000, m = v / 100 % 100, d = v % 100;
    return y >= 1970 && y <= 9999 && m >= 1 && m <= 12 && d >= 1 && d <= DaysInMonth(y, m);
}