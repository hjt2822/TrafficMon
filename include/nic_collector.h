#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct NicSample {
    uint64_t luid = 0;
    std::wstring alias;
    std::string guid;
    uint64_t upBytes = 0;
    uint64_t downBytes = 0;
};

class NicCollector {
public:
    // Returns per-interface byte deltas since the previous call. First call primes baselines.
    std::vector<NicSample> Poll();

private:
    struct Counters { uint64_t in = 0, out = 0; };
    std::unordered_map<uint64_t, Counters> prev_;
};
