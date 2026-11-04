#include "bridge.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "local_time.h"
#include "nic_collector.h"
#include "proc_collector.h"
#include "storage.h"
#include "traffic_aggregator.h"

namespace {

constexpr int kFlushEveryTicks = 5;
constexpr int kTodayCacheTicks = 5;
constexpr int kMonthCacheTicks = 5;  /* keep month >= today at every flush */
constexpr int kMaxSignalPoints = TM_SIGNAL_POINTS;

int64_t NowUtc() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::wstring WideFromUtf8(const std::string& text) {
    if (text.empty()) return {};
    int chars = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                    nullptr, 0);
    if (chars <= 0) return {};
    std::wstring out(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), chars);
    return out;
}

std::string Utf8FromWide(const std::wstring& text) {
    if (text.empty()) return {};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                    nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), bytes,
                        nullptr, nullptr);
    return out;
}

void CopyW(wchar_t* dst, size_t cch, const std::wstring& src) {
    if (!dst || !cch) return;
    const size_t n = std::min(src.size(), cch - 1);
    if (n) std::wmemcpy(dst, src.data(), n);
    dst[n] = L'\0';
}

void CopyW(wchar_t* dst, size_t cch, const wchar_t* src) {
    if (!dst || !cch) return;
    if (!src) { dst[0] = L'\0'; return; }
    const size_t n = std::min(std::wcslen(src), cch - 1);
    if (n) std::wmemcpy(dst, src, n);
    dst[n] = L'\0';
}

void SetError(wchar_t* err, int errChars, const std::wstring& text) {
    if (err && errChars > 0) CopyW(err, static_cast<size_t>(errChars), text);
}

std::wstring BaseName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring DisplayName(const std::string& identity) {
    if (identity == "System") return L"System";
    if (identity.rfind("unknown:", 0) == 0) return L"未知进程";
    const std::wstring wide = WideFromUtf8(identity);
    const std::wstring base = BaseName(wide);
    return base.empty() ? L"未知进程" : base;
}

std::wstring DisplayPath(const std::string& identity) {
    if (identity.rfind("unknown:", 0) == 0) return {};
    return WideFromUtf8(identity);
}

// Local wall-clock hour of a UTC hour bucket. SystemTimeToTzSpecificLocalTime uses the
// current time-zone rules, matching local_time.cpp's documented approximation.
int LocalHourForUtc(int64_t utcSeconds) {
    const int64_t fileTime = (utcSeconds + 11644473600LL) * 10000000LL;
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(fileTime & 0xffffffff);
    ft.dwHighDateTime = static_cast<DWORD>(static_cast<uint64_t>(fileTime) >> 32);
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (!FileTimeToSystemTime(&ft, &utc)) return 0;
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return utc.wHour;
    return local.wHour;
}

bool IsLeap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DaysInMonth(int32_t day) {
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int year = day / 10000;
    const int month = (day / 100) % 100;
    if (month < 1 || month > 12) return 31;
    return kDays[month - 1] + (month == 2 && IsLeap(year) ? 1 : 0);
}

struct AppAgg {
    int64_t up = 0;
    int64_t down = 0;
    int64_t upLan = 0;
    int64_t downLan = 0;
    bool lanKnown = true;
    bool seen = false;
};

void Accumulate(std::map<std::string, AppAgg>& totals, const TrafficRecord& record) {
    AppAgg& a = totals[record.identity];
    a.seen = true;
    a.up += record.up;
    a.down += record.down;
    if (record.lanSplit) {
        a.upLan += record.upLan;
        a.downLan += record.downLan;
    } else {
        a.lanKnown = false;
    }
}

std::vector<TmAppRow> BuildAppRows(const std::map<std::string, AppAgg>& totals,
                                   int64_t* totalUp, int64_t* totalDown) {
    std::vector<TmAppRow> rows;
    rows.reserve(totals.size());
    int64_t upSum = 0;
    int64_t downSum = 0;
    for (const auto& [identity, a] : totals) {
        upSum += a.up;
        downSum += a.down;
        TmAppRow row{};
        CopyW(row.name, TM_NAME_CHARS, DisplayName(identity));
        CopyW(row.path, TM_PATH_CHARS, DisplayPath(identity));
        row.up = a.up;
        row.down = a.down;
        row.upLan = a.upLan;
        row.downLan = a.downLan;
        row.lanKnown = a.lanKnown && a.seen ? 1 : 0;
        rows.push_back(row);
    }
    if (totalUp) *totalUp = upSum;
    if (totalDown) *totalDown = downSum;
    std::sort(rows.begin(), rows.end(), [](const TmAppRow& a, const TmAppRow& b) {
        const int64_t ta = a.up + a.down;
        const int64_t tb = b.up + b.down;
        if (ta != tb) return ta > tb;
        return std::wcscmp(a.name, b.name) < 0;
    });
    if (rows.size() > TM_MAX_APPS) rows.resize(TM_MAX_APPS);
    return rows;
}

struct LiveNic {
    std::wstring alias;
    std::string guid;
    uint64_t upPerSec = 0;
    uint64_t downPerSec = 0;
};

}  // namespace

struct TmEngine {
    std::unique_ptr<Storage> storage;
    NicCollector nic;
    ProcNetCollector proc;
    TrafficAggregator aggregate;

    std::wstring dbPath;
    std::wstring status;
    std::wstring startError;
    std::wstring lastError;

    bool noEtw = false;
    bool started = false;
    bool stopped = false;
    bool etw = false;

    int tick = 0;
    std::chrono::steady_clock::time_point lastPoll;
    double lastElapsed = 1.0;
    uint64_t lastUpRate = 0;
    uint64_t lastDownRate = 0;

    std::vector<LiveNic> liveNics;
    uint64_t ringUp[kMaxSignalPoints]{};
    uint64_t ringDown[kMaxSignalPoints]{};
    int ringPos = 0;
    int ringCount = 0;

    int64_t sessionStart = 0;
    int64_t todayUp = 0;
    int64_t todayDown = 0;
    int64_t monthUp = 0;
    int64_t monthDown = 0;
    int64_t appTodayUp = 0;
    int64_t appTodayDown = 0;
    int cacheTickToday = -1000;
    int cacheTickMonth = -1000;
    std::map<std::string, std::pair<int64_t, int64_t>> nicToday;  // guid -> (up, down)
    std::vector<TmAppRow> appToday;
    std::vector<std::string> watchIds;
    std::vector<std::pair<uint64_t, uint64_t>> watchRates;  // per tick, parallel to watchIds
};

namespace {

void PushSignal(TmEngine& e, uint64_t up, uint64_t down) {
    e.ringUp[e.ringPos] = up;
    e.ringDown[e.ringPos] = down;
    e.ringPos = (e.ringPos + 1) % kMaxSignalPoints;
    if (e.ringCount < kMaxSignalPoints) ++e.ringCount;
}

void RefreshToday(TmEngine& e) {
    const int32_t day = LocalDayForUtc(NowUtc());
    const int32_t end = NextDay(day);

    e.nicToday.clear();
    e.todayUp = 0;
    e.todayDown = 0;
    for (const TrafficRecord& r : e.storage->Query(false, day, end)) {
        auto& sum = e.nicToday[r.identity];
        sum.first += r.up;
        sum.second += r.down;
        e.todayUp += r.up;
        e.todayDown += r.down;
    }

    std::map<std::string, AppAgg> apps;
    for (const TrafficRecord& r : e.storage->Query(true, day, end)) Accumulate(apps, r);
    e.appToday = BuildAppRows(apps, &e.appTodayUp, &e.appTodayDown);
    e.cacheTickToday = e.tick;
}

void RefreshMonth(TmEngine& e) {
    const int32_t day = LocalDayForUtc(NowUtc());
    const int32_t begin = MonthStartOf(day);
    const int32_t end = NextMonthStart(day);
    e.monthUp = 0;
    e.monthDown = 0;
    for (const TrafficRecord& r : e.storage->Query(false, begin, end)) {
        e.monthUp += r.up;
        e.monthDown += r.down;
    }
    e.cacheTickMonth = e.tick;
}

void FillSnapshot(TmEngine& e, TmSnapshot* out) {
    std::memset(out, 0, sizeof(*out));
    out->running = e.started ? 1 : 0;
    out->etw = e.etw ? 1 : 0;
    out->appBytesAvailable = e.etw ? 1 : 0;
    out->upPerSec = e.lastUpRate;
    out->downPerSec = e.lastDownRate;
    out->todayUp = e.todayUp;
    out->todayDown = e.todayDown;
    out->monthUp = e.monthUp;
    out->monthDown = e.monthDown;
    out->appTotalUp = e.appTodayUp;
    out->appTotalDown = e.appTodayDown;
    out->sessionStartUtc = e.sessionStart;

    if (!e.started) {
        out->status[0] = L'\0';
        CopyW(out->status, TM_STATUS_CHARS,
              !e.startError.empty() ? std::wstring(L"启动失败") : std::wstring(L"未启动"));
    } else {
        CopyW(out->status, TM_STATUS_CHARS,
              e.etw ? std::wstring(L"实时监控 · 管理员模式")
                    : std::wstring(L"连接表模式 · 应用统计不可用"));
    }
    if (!e.lastError.empty()) {
        CopyW(out->error, TM_ERROR_CHARS, e.lastError);
    } else if (!e.etw && !e.startError.empty()) {
        CopyW(out->error, TM_ERROR_CHARS, L"ETW 启动失败：" + e.startError);
    }
    CopyW(out->dbPath, TM_PATH_CHARS, e.dbPath);

    const size_t nicCount = std::min<size_t>(e.liveNics.size(), TM_MAX_NICS);
    out->nicCount = static_cast<int>(nicCount);
    for (size_t i = 0; i < nicCount; ++i) {
        const LiveNic& src = e.liveNics[i];
        TmNicRow& dst = out->nics[i];
        CopyW(dst.alias, TM_ALIAS_CHARS,
              src.alias.empty() ? WideFromUtf8(src.guid) : src.alias);
        CopyW(dst.guid, TM_GUID_CHARS, WideFromUtf8(src.guid));
        dst.upPerSec = src.upPerSec;
        dst.downPerSec = src.downPerSec;
        const auto today = e.nicToday.find(src.guid);
        if (today != e.nicToday.end()) {
            dst.todayUp = today->second.first;
            dst.todayDown = today->second.second;
        }
    }

    const size_t appCount = e.appToday.size();
    out->appCount = static_cast<int>(appCount);
    for (size_t i = 0; i < appCount; ++i) out->apps[i] = e.appToday[i];

    out->watchCount = static_cast<int>(e.watchIds.size());
    for (size_t i = 0; i < e.watchIds.size(); ++i) {
        TmWatchRow& dst = out->watch[i];
        CopyW(dst.name, TM_NAME_CHARS, DisplayName(e.watchIds[i]));
        CopyW(dst.path, TM_PATH_CHARS, DisplayPath(e.watchIds[i]));
        if (i < e.watchRates.size()) {
            dst.upPerSec = e.watchRates[i].first;
            dst.downPerSec = e.watchRates[i].second;
        }
    }

    out->signalCount = e.ringCount;
    for (int i = 0; i < e.ringCount; ++i) {
        const int index = (e.ringPos - e.ringCount + i + kMaxSignalPoints) % kMaxSignalPoints;
        out->signalUp[i] = e.ringUp[index];
        out->signalDown[i] = e.ringDown[index];
    }
}

}  // namespace

extern "C" int tm_default_db_path(wchar_t* out, int outChars, wchar_t* err, int errChars) {
    if (!out || outChars <= 0) return -1;
    wchar_t module[32768];
    const DWORD length = GetModuleFileNameW(nullptr, module, 32768);
    if (!length || length >= 32768) {
        SetError(err, errChars, L"无法确定程序所在目录");
        return -1;
    }
    const std::filesystem::path dir = std::filesystem::path(module).parent_path();
    CopyW(out, static_cast<size_t>(outChars), (dir / L"traffic.sqlite3").wstring());
    return 0;
}

extern "C" TmEngine* tm_create(const wchar_t* dbPath, int noEtw, wchar_t* err, int errChars) {
    try {
        auto engine = std::make_unique<TmEngine>();
        engine->noEtw = noEtw != 0;
        engine->dbPath = dbPath ? dbPath : L"";
        if (engine->dbPath.empty()) throw std::runtime_error("Database path is empty");
        std::error_code ec;
        const std::filesystem::path parent = std::filesystem::path(engine->dbPath).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        engine->storage = std::make_unique<Storage>(engine->dbPath);
        return engine.release();
    } catch (const std::exception& ex) {
        SetError(err, errChars, WideFromUtf8(ex.what()));
        return nullptr;
    } catch (...) {
        SetError(err, errChars, L"未知错误");
        return nullptr;
    }
}

extern "C" int tm_start(TmEngine* engine) {
    if (!engine) return -1;
    if (engine->started) return 0;
    try {
        std::wstring etwError;
        const bool etw = !engine->noEtw && engine->proc.Start(etwError);
        engine->etw = etw;
        engine->startError = etw ? L"" : etwError;
        engine->sessionStart = NowUtc();
        engine->storage->BeginSession(etw, engine->sessionStart);
        engine->nic.Poll();  // prime NIC baselines; the first real sample arrives next tick
        engine->lastPoll = std::chrono::steady_clock::now();
        engine->started = true;
        engine->stopped = false;
        RefreshToday(*engine);
        RefreshMonth(*engine);
        return 0;
    } catch (const std::exception& ex) {
        engine->lastError = WideFromUtf8(ex.what());
        engine->proc.Stop();
        engine->started = false;
        return -1;
    } catch (...) {
        engine->lastError = L"启动采集时发生未知错误";
        engine->proc.Stop();
        engine->started = false;
        return -1;
    }
}

extern "C" void tm_tick(TmEngine* engine, TmSnapshot* out) {
    if (!out) return;
    if (!engine) {
        std::memset(out, 0, sizeof(*out));
        CopyW(out->status, TM_STATUS_CHARS, L"未启动");
        return;
    }
    if (!engine->started) {
        FillSnapshot(*engine, out);
        return;
    }
    try {
        const auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - engine->lastPoll).count();
        engine->lastPoll = now;
        if (elapsed < 0.2) elapsed = 0.2;  // WM_TIMER coalescing / clock granularity guard
        engine->lastElapsed = elapsed;

        const int64_t utc = NowUtc();
        uint64_t upBytes = 0;
        uint64_t downBytes = 0;
        engine->liveNics.clear();
        for (const NicSample& s : engine->nic.Poll()) {
            upBytes += s.upBytes;
            downBytes += s.downBytes;
            engine->aggregate.Add(false, TrafficDelta{s.guid, utc,
                static_cast<int64_t>(s.upBytes), static_cast<int64_t>(s.downBytes)});
            LiveNic live;
            live.alias = s.alias;
            live.guid = s.guid;
            live.upPerSec = static_cast<uint64_t>(static_cast<double>(s.upBytes) / elapsed);
            live.downPerSec = static_cast<uint64_t>(static_cast<double>(s.downBytes) / elapsed);
            engine->liveNics.push_back(std::move(live));
        }
        engine->lastUpRate = static_cast<uint64_t>(static_cast<double>(upBytes) / elapsed);
        engine->lastDownRate = static_cast<uint64_t>(static_cast<double>(downBytes) / elapsed);
        PushSignal(*engine, engine->lastUpRate, engine->lastDownRate);

        /* Busiest interface first; the UI shows a top-N list plus an aggregate. */
        std::sort(engine->liveNics.begin(), engine->liveNics.end(),
                  [](const LiveNic& a, const LiveNic& b) {
                      return (a.upPerSec + a.downPerSec) > (b.upPerSec + b.downPerSec);
                  });

        if (engine->etw) {
            std::map<std::string, std::pair<uint64_t, uint64_t>> tickApps;
            for (const TrafficDelta& delta : engine->proc.DrainHistory()) {
                engine->aggregate.Add(true, delta);
                auto& sum = tickApps[delta.identity];
                sum.first += static_cast<uint64_t>(delta.up);
                sum.second += static_cast<uint64_t>(delta.down);
            }
            engine->watchRates.clear();
            for (const std::string& id : engine->watchIds) {
                const auto found = tickApps.find(id);
                std::pair<uint64_t, uint64_t> rate{0, 0};
                if (found != tickApps.end()) {
                    rate.first = static_cast<uint64_t>(static_cast<double>(found->second.first) / elapsed);
                    rate.second = static_cast<uint64_t>(static_cast<double>(found->second.second) / elapsed);
                }
                engine->watchRates.push_back(rate);
            }
        }

        ++engine->tick;
        if (engine->tick % kFlushEveryTicks == 0) engine->aggregate.Flush(*engine->storage);
        if (engine->tick - engine->cacheTickToday >= kTodayCacheTicks) RefreshToday(*engine);
        if (engine->tick - engine->cacheTickMonth >= kMonthCacheTicks) RefreshMonth(*engine);
        FillSnapshot(*engine, out);
    } catch (const std::exception& ex) {
        engine->lastError = WideFromUtf8(ex.what());
        FillSnapshot(*engine, out);
    } catch (...) {
        engine->lastError = L"采集时发生未知错误";
        FillSnapshot(*engine, out);
    }
}

extern "C" int tm_history(TmEngine* engine, int monthMode, int32_t anchorDay, TmHistory* out,
                          wchar_t* err, int errChars) {
    if (!engine || !out) {
        SetError(err, errChars, L"无效的参数");
        return -1;
    }
    try {
        std::memset(out, 0, sizeof(*out));
        out->monthMode = monthMode ? 1 : 0;
        const int32_t begin = monthMode ? MonthStartOf(anchorDay) : anchorDay;
        const int32_t end = monthMode ? NextMonthStart(anchorDay) : NextDay(anchorDay);
        out->count = monthMode ? DaysInMonth(anchorDay) : 24;
        if (out->count > TM_MAX_BUCKETS) out->count = TM_MAX_BUCKETS;

        for (int i = 0; i < out->count; ++i) out->label[i] = monthMode ? i + 1 : i;

        for (const TrafficRecord& r : engine->storage->Query(false, begin, end)) {
            int bucket;
            if (monthMode) {
                bucket = r.localDay % 100 - 1;
            } else {
                bucket = LocalHourForUtc(r.utcHour);
            }
            if (bucket >= 0 && bucket < out->count) {
                out->up[bucket] += r.up;
                out->down[bucket] += r.down;
            }
            out->totalUp += r.up;
            out->totalDown += r.down;
        }

        std::map<std::string, AppAgg> apps;
        for (const TrafficRecord& r : engine->storage->Query(true, begin, end)) Accumulate(apps, r);
        const std::vector<TmAppRow> rows =
            BuildAppRows(apps, &out->appTotalUp, &out->appTotalDown);
        out->appCount = static_cast<int>(rows.size());
        for (size_t i = 0; i < rows.size(); ++i) out->apps[i] = rows[i];
        return 0;
    } catch (const std::exception& ex) {
        SetError(err, errChars, WideFromUtf8(ex.what()));
        return -1;
    } catch (...) {
        SetError(err, errChars, L"查询历史时发生未知错误");
        return -1;
    }
}

extern "C" int tm_set_watch(TmEngine* engine, const wchar_t* paths) {
    if (!engine) return -1;
    try {
        engine->watchIds.clear();
        engine->watchRates.clear();
        if (!paths) return 0;
        const std::wstring text(paths);
        size_t start = 0;
        while (start <= text.size() && engine->watchIds.size() < TM_MAX_WATCH) {
            size_t end = text.find(L'\n', start);
            if (end == std::wstring::npos) end = text.size();
            std::wstring line = text.substr(start, end - start);
            while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
            if (!line.empty()) {
                const std::string id = Utf8FromWide(line);
                if (std::find(engine->watchIds.begin(), engine->watchIds.end(), id) ==
                    engine->watchIds.end())
                    engine->watchIds.push_back(id);
            }
            start = end + 1;
        }
        return static_cast<int>(engine->watchIds.size());
    } catch (...) {
        return -1;
    }
}

extern "C" void tm_stop(TmEngine* engine) {
    if (!engine || !engine->started || engine->stopped) return;
    try {
        engine->proc.Stop();  // flushes pending identities into DrainHistory()
        for (const TrafficDelta& delta : engine->proc.DrainHistory())
            engine->aggregate.Add(true, delta);
        engine->proc.Drain();  // per-PID live counters are not persisted separately
        engine->aggregate.Flush(*engine->storage);
        engine->storage->CloseSession();
    } catch (const std::exception& ex) {
        engine->lastError = WideFromUtf8(ex.what());
    } catch (...) {
        engine->lastError = L"停止采集时发生未知错误";
    }
    engine->started = false;
    engine->stopped = true;
    engine->etw = false;
}

extern "C" void tm_destroy(TmEngine* engine) {
    if (!engine) return;
    try { tm_stop(engine); } catch (...) {}
    delete engine;
}

extern "C" int32_t tm_today(void) {
    return LocalDayForUtc(NowUtc());
}

extern "C" int32_t tm_month_start(int32_t day) {
    return MonthStartOf(day);
}

extern "C" int32_t tm_next_month(int32_t day) {
    return NextMonthStart(day);
}

extern "C" int32_t tm_next_day(int32_t day) {
    return NextDay(day);
}

extern "C" int32_t tm_prev_day(int32_t day) {
    const int year = day / 10000;
    const int month = (day / 100) % 100;
    const int date = day % 100;
    if (date > 1) return year * 10000 + month * 100 + (date - 1);
    int prevYear = year;
    int prevMonth = month - 1;
    if (prevMonth < 1) {
        prevMonth = 12;
        --prevYear;
    }
    const int last = DaysInMonth(prevYear * 10000 + prevMonth * 100 + 1);
    return prevYear * 10000 + prevMonth * 100 + last;
}

extern "C" int32_t tm_prev_month(int32_t day) {
    int year = day / 10000;
    int month = (day / 100) % 100 - 1;
    if (month < 1) {
        month = 12;
        --year;
    }
    return year * 10000 + month * 100 + 1;
}