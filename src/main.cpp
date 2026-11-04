#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nic_collector.h"
#include "proc_collector.h"
#include "connection_collector.h"
#include "traffic_aggregator.h"
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <map>
#include "local_time.h"

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI OnCtrl(DWORD) {
    g_stop = true;
    return TRUE;
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Rate(uint64_t bytesPerSec) {
    char b[32];
    double v = (double)bytesPerSec;
    if (v >= 1048576.0) std::snprintf(b, sizeof b, "%.2f MB/s", v / 1048576.0);
    else if (v >= 1024.0) std::snprintf(b, sizeof b, "%.1f KB/s", v / 1024.0);
    else std::snprintf(b, sizeof b, "%llu B/s", (unsigned long long)bytesPerSec);
    return b;
}

std::string Total(uint64_t bytes) {
    char b[32];
    double v = (double)bytes;
    if (v >= 1073741824.0) std::snprintf(b, sizeof b, "%.2f GB", v / 1073741824.0);
    else if (v >= 1048576.0) std::snprintf(b, sizeof b, "%.2f MB", v / 1048576.0);
    else std::snprintf(b, sizeof b, "%.1f KB", v / 1024.0);
    return b;
}

std::wstring ProcName(uint32_t pid, std::unordered_map<uint32_t, std::wstring>& cache) {
    cache.erase(pid);
    std::wstring name;
    if (pid == 0 || pid == 4) {
        name = L"System";
    } else if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH * 2];
        DWORD len = (DWORD)(sizeof path / sizeof path[0]);
        if (QueryFullProcessImageNameW(h, 0, path, &len)) {
            name.assign(path, len);
            size_t slash = name.find_last_of(L'\\');
            if (slash != std::wstring::npos) name = name.substr(slash + 1);
        }
        CloseHandle(h);
    }
    if (name.empty()) name = L"(exited/unknown)";
    // Only cache resolved names; unresolved PIDs may be retried (process may still be alive).
    if (name != L"(exited/unknown)") cache[pid] = name;
    return name;
}

int64_t NowUtc() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
std::pair<int32_t,int32_t> Period(const std::string& text, bool month) {
    if (text.size() != (month ? 7u : 10u) || text[4] != '-' || (!month && text[7] != '-'))
        throw std::invalid_argument("Expected YYYY-MM or YYYY-MM-DD");
    auto number = [&](size_t at, size_t length) {
        int value = 0;
        for (size_t i = at; i < at + length; ++i) {
            if (text[i] < '0' || text[i] > '9') throw std::invalid_argument("Invalid calendar date");
            value = value * 10 + (text[i] - '0');
        }
        return value;
    };
    const int y = number(0,4), m = number(5,2), d = month ? 1 : number(8,2);
    if (!ValidMonth(y * 100 + m) || !ValidDay(y * 10000 + m * 100 + d))
        throw std::invalid_argument("Invalid calendar date");
    const int32_t begin = y * 10000 + m * 100 + d;
    return {begin, month ? NextMonthStart(begin) : NextDay(begin)};
}
int PrintHistory(Storage& storage, const std::string& period, bool month) {
    const auto [begin,end] = Period(period,month);
    std::printf("History %s local calendar, half-open [%d,%d)\n",period.c_str(),begin,end);
    for (bool application : {false,true}) {
        struct Sum { int64_t up=0,down=0,upLan=0,downLan=0; bool split=true; };
        std::map<std::string,Sum> totals;
        for (const auto& r : storage.Query(application,begin,end)) {
            auto& s = totals[r.identity];
            s.up += r.up; s.down += r.down;
            if (r.lanSplit) { s.upLan += r.upLan; s.downLan += r.downLan; }
            else s.split = false;
        }
        std::printf("%s (bytes; do not add NIC and application totals):\n", application ? "Applications" : "NICs");
        for (const auto& [id, s] : totals) {
            std::printf("  %s up=%lld down=%lld", id.c_str(),(long long)s.up,(long long)s.down);
            if (s.split) std::printf(" lan_up=%lld lan_down=%lld",(long long)s.upLan,(long long)s.downLan);
            else std::printf(" lan=unclassified");
            std::puts("");
        }
        if (totals.empty()) std::puts("  No recorded rows (not proof of zero traffic)");
    }
    // Coverage rows are epoch-based; widen by two days to cover any local offset of the label range.
    const int64_t day = 86400;
    const int64_t approxStart = (static_cast<int64_t>(begin / 10000) - 1970) * 365 * day - 2 * day;
    for (const auto& s : storage.Sessions(approxStart, NowUtc() + 2 * day))
        std::printf("Session %lld mode=%s started=%lld last_commit=%lld status=%s\n",
            (long long)s.id,s.mode.c_str(),(long long)s.started,(long long)s.committed,
            s.closed ? "closed" : "open-or-interrupted");
    std::puts("Local day labels use the current timezone; rows migrated from v1/v2 are approximated.");
    std::puts("Coverage is collector mode/commit history, not proof of lossless ETW capture.");
    return 0;
}

}  // namespace

int Run(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(OnCtrl, TRUE);

    int durationSec = 0;
    bool noEtw = false;
    std::string dbPath, historyPeriod;
    bool historyMonth = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--no-etw") noEtw = true;
        else if (std::string(argv[i]) == "--db" && i + 1 < argc) dbPath = argv[++i];
        else if ((std::string(argv[i]) == "--day" || std::string(argv[i]) == "--month") && i + 1 < argc) {
            if (!historyPeriod.empty()) throw std::invalid_argument("Choose only one history period");
            historyMonth = std::string(argv[i]) == "--month";
            historyPeriod = argv[++i];
            Period(historyPeriod, historyMonth);
        }
        else {
            char* end = nullptr;
            long seconds = std::strtol(argv[i], &end, 10);
            if (!*argv[i] || *end || seconds < 1 || seconds > 86400) {
                std::fprintf(stderr, "Usage: trafficmon_m1 [1..86400 seconds] [--no-etw]\n");
                return 2;
            }
            durationSec = static_cast<int>(seconds);
        }
    }

    if (dbPath.empty()) {
        wchar_t local[32768];
        DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
        if (!length || length >= 32768) throw std::runtime_error("LOCALAPPDATA unavailable; use --db");
        auto dir = std::filesystem::path(local) / L"TrafficMon";
        std::filesystem::create_directories(dir);
        dbPath = Utf8((dir / L"traffic.sqlite3").wstring());
    }
    if (!historyPeriod.empty() && !std::filesystem::exists(std::filesystem::u8path(dbPath)))
        throw std::runtime_error("History database does not exist");
    Storage storage(dbPath);
    if (!historyPeriod.empty()) return PrintHistory(storage, historyPeriod, historyMonth);
    NicCollector nic;
    ConnectionCollector connections;
    ProcNetCollector proc;
    std::wstring err;
    bool procOk = !noEtw && proc.Start(err);
    if (!procOk) {
        std::printf("[WARN] application bytes N/A; connection-table mode: %s\n", noEtw ? "--no-etw" : Utf8(err).c_str());
    } else std::puts("[OK] ETW session started");
    storage.BeginSession(procOk, NowUtc());
    TrafficAggregator aggregate;
    auto drainHistory = [&]() {
        for (const auto& delta : proc.DrainHistory()) aggregate.Add(true, delta);
    };
    std::printf("[DB] %s (UTC hourly buckets; NIC/application totals separate)\n", dbPath.c_str());
    nic.Poll();  // prime baselines
    std::unordered_map<uint32_t, std::wstring> nameCache;
    std::unordered_map<uint32_t, ProcTraffic> totals;
    uint64_t nicUpTotal = 0, nicDownTotal = 0, procUpTotal = 0, procDownTotal = 0;

    auto start = std::chrono::steady_clock::now();
    auto next = start;
    int tick = 0;
    while (!g_stop) {
        next += std::chrono::seconds(1);
        std::this_thread::sleep_until(next);
        ++tick;

        std::printf("\n--- t=%ds ---\n", tick);
        for (const NicSample& s : nic.Poll()) {
            nicUpTotal += s.upBytes;
            nicDownTotal += s.downBytes;
            auto nowUtc = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            aggregate.Add(false, TrafficDelta{s.guid, nowUtc, (int64_t)s.upBytes, (int64_t)s.downBytes});
            if (s.upBytes == 0 && s.downBytes == 0) continue;
            std::printf("NIC %-24s up %-12s down %s\n", Utf8(s.alias).c_str(),
                        Rate(s.upBytes).c_str(), Rate(s.downBytes).c_str());
        }

        if (procOk) {
            auto delta = proc.Drain();
            std::vector<std::pair<uint32_t, ProcTraffic>> rows(delta.begin(), delta.end());
            for (auto& [pid, t] : delta) {
                ProcTraffic& tt = totals[pid];
                tt.upLan += t.upLan; tt.upWan += t.upWan;
                tt.downLan += t.downLan; tt.downWan += t.downWan;
                procUpTotal += t.Up();
                procDownTotal += t.Down();
            }
            std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) {
                return a.second.Up() + a.second.Down() > b.second.Up() + b.second.Down();
            });
            if (rows.size() > 5) rows.resize(5);
            for (auto& [pid, t] : rows) {
                std::printf("  PID %-6u %-24s up %-12s down %-12s (LAN up/down %s/%s)\n", pid,
                            Utf8(ProcName(pid, nameCache)).c_str(), Rate(t.Up()).c_str(),
                            Rate(t.Down()).c_str(), Rate(t.upLan).c_str(), Rate(t.downLan).c_str());
            }
        }

        if (!procOk) {
            auto snapshot = connections.Poll();
            for (const auto& error : snapshot.errors) std::printf("[WARN] %s\n", error.c_str());
            std::unordered_map<uint32_t, std::pair<unsigned, unsigned>> counts;
            for (const auto& row : snapshot.rows) {
                auto& count = counts[row.pid];
                if (row.udp) ++count.second; else ++count.first;
            }
            for (const auto& [pid, count] : counts)
                std::printf("  PID %-6u %-24s TCP=%u UDP-endpoints=%u up=N/A down=N/A\n", pid,
                    Utf8(ProcName(pid, nameCache)).c_str(), count.first, count.second);
        }
        drainHistory();
        if (tick % 60 == 0) aggregate.Flush(storage);
        if (durationSec > 0 && tick >= durationSec) break;
    }

    proc.Stop();
    drainHistory();
    for (const auto& [pid, t] : proc.Drain()) {
        auto& tt = totals[pid];
        tt.upLan += t.upLan; tt.upWan += t.upWan;
        tt.downLan += t.downLan; tt.downWan += t.downWan;
        procUpTotal += t.Up(); procDownTotal += t.Down();
    }
    aggregate.Flush(storage);
    storage.CloseSession();

    std::printf("\n=== Summary over %d s ===\n", tick);
    std::printf("NIC total      up %s, down %s\n", Total(nicUpTotal).c_str(), Total(nicDownTotal).c_str());
    if (procOk) {
        std::printf("Process total  up %s, down %s (ETW events: %llu, process events: %llu, provider: %s)\n",
                    Total(procUpTotal).c_str(), Total(procDownTotal).c_str(),
                    (unsigned long long)proc.EventCount(), (unsigned long long)proc.ProcessEventCount(),
                    proc.ProcessProviderEnabled() ? "enabled" : "unavailable");
        std::vector<std::pair<uint32_t, ProcTraffic>> rows(totals.begin(), totals.end());
        std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) {
            return a.second.Up() + a.second.Down() > b.second.Up() + b.second.Down();
        });
        if (rows.size() > 10) rows.resize(10);
        for (auto& [pid, t] : rows) {
            std::printf("  PID %-6u %-24s up %-10s down %s\n", pid,
                        Utf8(ProcName(pid, nameCache)).c_str(), Total(t.Up()).c_str(),
                        Total(t.Down()).c_str());
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    try { return Run(argc, argv); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "[ERROR] %s; uncommitted samples may be lost\n", e.what());
        return 1;
    }
}
