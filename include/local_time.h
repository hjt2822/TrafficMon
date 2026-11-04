#pragma once
#include <cstdint>

// Local calendar label YYYYMMDD for a UTC instant, using the machine timezone (DST aware).
int32_t LocalDayForUtc(int64_t utcSeconds);
// Label arithmetic; ranges are half-open on labels: [day, NextDay(day)).
int32_t NextDay(int32_t day);
int32_t MonthStartOf(int32_t day);
int32_t NextMonthStart(int32_t day);
bool ValidMonth(int32_t yyyymm);
bool ValidDay(int32_t yyyymmdd);