#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "connection_collector.h"

namespace {
std::string Address(const void* data, bool v6, DWORD scope = 0) {
    char text[INET6_ADDRSTRLEN]{};
    if (!InetNtopA(v6 ? AF_INET6 : AF_INET, data, text, sizeof(text))) return "?";
    return std::string(text) + (scope ? "%" + std::to_string(scope) : "");
}
template<class Table, class Decode>
void Collect(ConnectionSnapshot& result, bool udp, bool v6, Decode decode) {
    DWORD size = 0, error = ERROR_INSUFFICIENT_BUFFER;
    std::vector<unsigned char> buffer;
    for (int attempt = 0; attempt < 5; ++attempt) {
        void* data = buffer.empty() ? nullptr : buffer.data();
        error = udp ? GetExtendedUdpTable(data, &size, FALSE, v6 ? AF_INET6 : AF_INET,
                                         UDP_TABLE_OWNER_PID, 0)
                    : GetExtendedTcpTable(data, &size, FALSE, v6 ? AF_INET6 : AF_INET,
                                         TCP_TABLE_OWNER_PID_ALL, 0);
        if (error == NO_ERROR) {
            if (buffer.empty()) break;
            const auto* table = reinterpret_cast<const Table*>(buffer.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                auto row = decode(table->table[i]);
                row.udp = udp;
                row.ipv6 = v6;
                result.rows.push_back(std::move(row));
            }
            return;
        }
        if (error != ERROR_INSUFFICIENT_BUFFER) break;
        buffer.resize(size);
    }
    result.errors.push_back(std::string(udp ? "UDP" : "TCP") + (v6 ? "6" : "4") +
                            " table unavailable: " + std::to_string(error));
}
}

ConnectionSnapshot ConnectionCollector::Poll() const {
    ConnectionSnapshot result;
    Collect<MIB_TCPTABLE_OWNER_PID>(result, false, false, [](const auto& r) {
        return Connection{r.dwOwningPid, r.dwState, false, false,
            Address(&r.dwLocalAddr, false), Address(&r.dwRemoteAddr, false),
            ntohs(static_cast<u_short>(r.dwLocalPort)), ntohs(static_cast<u_short>(r.dwRemotePort))};
    });
    Collect<MIB_TCP6TABLE_OWNER_PID>(result, false, true, [](const auto& r) {
        return Connection{r.dwOwningPid, r.dwState, true, false,
            Address(r.ucLocalAddr, true, r.dwLocalScopeId), Address(r.ucRemoteAddr, true, r.dwRemoteScopeId),
            ntohs(static_cast<u_short>(r.dwLocalPort)), ntohs(static_cast<u_short>(r.dwRemotePort))};
    });
    Collect<MIB_UDPTABLE_OWNER_PID>(result, true, false, [](const auto& r) {
        return Connection{r.dwOwningPid, 0, false, true, Address(&r.dwLocalAddr, false), "",
            ntohs(static_cast<u_short>(r.dwLocalPort)), 0};
    });
    Collect<MIB_UDP6TABLE_OWNER_PID>(result, true, true, [](const auto& r) {
        return Connection{r.dwOwningPid, 0, true, true, Address(r.ucLocalAddr, true, r.dwLocalScopeId), "",
            ntohs(static_cast<u_short>(r.dwLocalPort)), 0};
    });
    return result;
}
