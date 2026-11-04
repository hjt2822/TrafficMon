#include "traffic_aggregator.h"
#include "local_time.h"
#include <limits>
#include <stdexcept>

namespace {
void CheckAdd(int64_t delta, int64_t& into) {
    if (delta < 0 || delta > std::numeric_limits<int64_t>::max() - into)
        throw std::overflow_error("Traffic counter overflow");
    into += delta;
}
}

void TrafficAggregator::Add(bool application, const TrafficDelta& d) {
    if (d.utcSeconds < 0 || d.identity.empty() || d.up < 0 || d.down < 0 ||
        (d.lanSplit && (d.upLan < 0 || d.downLan < 0 || d.upLan > d.up || d.downLan > d.down)))
        throw std::invalid_argument("Invalid traffic delta");
    const int64_t hour = d.utcSeconds - d.utcSeconds % 3600;
    const int32_t day = LocalDayForUtc(hour);
    if (day <= 0) throw std::invalid_argument("Unrepresentable local day");

    auto key = std::make_tuple(application, hour, day, d.identity);
    auto it = pending_.find(key);
    if (it == pending_.end()) {
        if (pending_.size() >= 100000) throw std::runtime_error("Pending traffic capacity reached; flush required");
        TrafficRecord r;
        r.utcHour = hour; r.localDay = day; r.identity = d.identity;
        r.up = d.up; r.down = d.down; r.lanSplit = d.lanSplit;
        r.upLan = d.upLan; r.downLan = d.downLan;
        pending_.emplace(std::move(key), std::move(r));
        return;
    }
    TrafficRecord& r = it->second;
    CheckAdd(d.up, r.up);
    CheckAdd(d.down, r.down);
    if (!d.lanSplit) {
        r.lanSplit = false;  // unclassified data poisons the split; totals stay exact
        r.upLan = r.downLan = 0;
    } else if (r.lanSplit) {
        CheckAdd(d.upLan, r.upLan);
        CheckAdd(d.downLan, r.downLan);
    }
}

void TrafficAggregator::Flush(Storage& storage) {
    TrafficBatch batch;
    for (const auto& [key, row] : pending_)
        (std::get<0>(key) ? batch.application : batch.nic).push_back(row);
    storage.Write(batch);
    pending_.clear();
}