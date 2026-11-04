#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;
struct TrafficRecord {
    int64_t utcHour = 0;
    int32_t localDay = 0;   // YYYYMMDD local calendar label
    std::string identity;
    int64_t up = 0, down = 0;
    bool lanSplit = false;  // false when LAN/WAN classification is unavailable
    int64_t upLan = 0, downLan = 0;
};
struct TrafficBatch {
    std::vector<TrafficRecord> nic, application;
};
struct Coverage {
    int64_t id = 0, started = 0, committed = 0;
    std::string mode;
    bool closed = false;
};
class Storage {
public:
    explicit Storage(const std::string& path);
    // Preferred on Windows: opens non-ASCII paths correctly (sqlite3_open16).
    explicit Storage(const std::wstring& path);
    ~Storage();
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;
    void Write(const TrafficBatch& batch);
    // Half-open local-day label range.
    std::vector<TrafficRecord> Query(bool application, int32_t beginDay, int32_t endDay,
                                     const std::string& identity = "") const;
    void BeginSession(bool etw, int64_t started);
    void CloseSession();
    std::vector<Coverage> Sessions(int64_t begin, int64_t end) const;
private:
    void Open();
    sqlite3* db_ = nullptr;
    int64_t session_ = 0;
};