#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Connection {
    uint32_t pid = 0, state = 0;
    bool ipv6 = false, udp = false;
    std::string local, remote;
    uint16_t localPort = 0, remotePort = 0;
};
struct ConnectionSnapshot {
    std::vector<Connection> rows;
    std::vector<std::string> errors;
};
class ConnectionCollector {
public:
    ConnectionSnapshot Poll() const;
};
