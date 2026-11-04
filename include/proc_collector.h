#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <map>
#include <tuple>
#include "traffic_aggregator.h"

struct ProcTraffic {
    uint64_t upLan = 0, upWan = 0, downLan = 0, downWan = 0;
    uint64_t Up() const { return upLan + upWan; }
    uint64_t Down() const { return downLan + downWan; }
};

// ETW real-time session on Microsoft-Windows-Kernel-Network. Requires administrator.
class ProcNetCollector {
public:
    ~ProcNetCollector() { Stop(); }
    bool Start(std::wstring& error);
    void Stop();
    // Returns per-PID traffic accumulated since the previous call.
    std::unordered_map<uint32_t, ProcTraffic> Drain();
    uint64_t EventCount() const { return events_.load(); }
    uint64_t ProcessEventCount() const { return processEvents_.load(); }
    bool ProcessProviderEnabled() const { return processProviderEnabled_; }

    void OnData(uint32_t pid, uint32_t size, bool isSend, bool isLan, int64_t fileTime);
    void OnProcessStart(uint32_t pid, int64_t createTime);
    void OnProcessStop(uint32_t pid, int64_t createTime, const std::string& imageName);
    void OnImageLoad(uint32_t pid, const std::string& ntPath);
    std::vector<TrafficDelta> DrainHistory();
    std::atomic<uint64_t> events_{0};
    std::atomic<uint64_t> processEvents_{0};
    bool processProviderEnabled_ = false;

private:
    struct CachedIdentity {
        std::string path;
        int64_t createTime = 0;  // FILETIME; guards against PID reuse
    };
    struct PendingHour {
        uint64_t up = 0, down = 0, upLan = 0, downLan = 0;
    };
    std::string IdentityFor(uint32_t pid, int64_t eventFileTime);
    void SeedIdentities();
    std::string NormalizeImagePath(const std::string& ntPath) const;
    void FlushPendingLocked(uint32_t pid, const std::string& identity);
    void FlushAllPendingLocked();

    std::wstring sessionName_;
    std::map<std::pair<int64_t, std::string>, TrafficDelta> history_;
    std::mutex mu_;
    std::unordered_map<uint32_t, CachedIdentity> identities_;
    std::unordered_map<uint32_t, ProcTraffic> acc_;
    // Traffic for PIDs whose image could not be resolved in time. Attribution waits until
    // ProcessStop (which carries the image basename) or session end.
    std::unordered_map<uint32_t, std::map<int64_t, PendingHour>> pending_;
    std::unordered_map<uint32_t, int64_t> started_;   // PID -> creation FILETIME
    std::unordered_map<uint32_t, std::string> images_;  // first ImageLoad per started PID
    std::map<std::string, std::string> deviceMap_;      // \Device\HarddiskVolumeN -> C:
    std::thread worker_;
    uint64_t sessionHandle_ = 0;
    uint64_t traceHandle_ = 0;
    bool running_ = false;
};
