#include "nic_collector.h"

#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>

namespace {
uint64_t Delta(uint64_t cur, uint64_t prev) { return cur >= prev ? cur - prev : 0; }
}

std::vector<NicSample> NicCollector::Poll() {
    std::vector<NicSample> out;
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table) return out;

    std::unordered_map<uint64_t, Counters> next;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& r = table->Table[i];
        if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (r.OperStatus != IfOperStatusUp) continue;
        if (r.InterfaceAndOperStatusFlags.FilterInterface) continue;

        uint64_t luid = r.InterfaceLuid.Value;
        Counters cur{r.InOctets, r.OutOctets};
        next[luid] = cur;

        auto it = prev_.find(luid);
        if (it == prev_.end()) continue;

        NicSample s;
        s.luid = luid;
        s.alias = r.Alias;
        const auto* rawGuid = reinterpret_cast<const unsigned char*>(&r.InterfaceGuid);
        const char* digits = "0123456789abcdef";
        for (size_t j = 0; j < sizeof(r.InterfaceGuid); ++j) {
            s.guid += digits[rawGuid[j] >> 4]; s.guid += digits[rawGuid[j] & 15];
        }
        s.downBytes = Delta(cur.in, it->second.in);
        s.upBytes = Delta(cur.out, it->second.out);
        out.push_back(std::move(s));
    }
    FreeMibTable(table);
    prev_.swap(next);
    return out;
}
