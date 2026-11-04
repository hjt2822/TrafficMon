/* Display formatting. All lengths are 1024-based; all strings are NUL-terminated. */
#ifndef TM_FORMAT_H
#define TM_FORMAT_H

#include <stdint.h>
#include <wchar.h>

/* "1.24" + "MB/s", "86.0" + "KB/s", "0" + "B/s" */
void tm_split_rate(uint64_t bytesPerSec, wchar_t* value, int valueChars,
                   wchar_t* unit, int unitChars);

/* "1.24" + "GB", "820" + "MB" */
void tm_split_bytes(int64_t bytes, wchar_t* value, int valueChars,
                    wchar_t* unit, int unitChars);

void tm_format_rate(uint64_t bytesPerSec, wchar_t* out, int outChars);
void tm_format_bytes(int64_t bytes, wchar_t* out, int outChars);

/* 0..100 with no trailing noise. */
void tm_format_percent(double percent, wchar_t* out, int outChars);

/* YYYYMMDD -> "2026年11月3日" and "2026年11月". */
void tm_format_date(int32_t day, wchar_t* out, int outChars);
void tm_format_month(int32_t day, wchar_t* out, int outChars);

/* "14:00"; used by the day chart axis. */
void tm_format_hour(int hour, wchar_t* out, int outChars);

#endif /* TM_FORMAT_H */