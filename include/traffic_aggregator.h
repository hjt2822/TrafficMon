#pragma once
#include "storage.h"
#include <map>
#include <string>
#include <tuple>

struct TrafficDelta {
    std::string identity;
    int64_t utcSeconds = 0;
    int64_t up = 0, down = 0;
    bool lanSplit = false;
    int64_t upLan = 0, downLan = 0;
};
class TrafficAggregator {
public:
    void Add(bool application, const TrafficDelta& delta);
    void Flush(Storage& storage);
    size_t Pending() const { return pending_.size(); }
private:
    std::map<std::tuple<bool, int64_t, int32_t, std::string>, TrafficRecord> pending_;
};