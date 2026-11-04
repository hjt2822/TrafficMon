#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <cassert>
#include <cstdio>
#include "connection_collector.h"

int main() {
    WSADATA data{};
    assert(WSAStartup(MAKEWORD(2,2), &data) == 0);
    for (bool v6 : {false, true}) for (bool udp : {false, true}) {
        SOCKET server = socket(v6 ? AF_INET6 : AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
        assert(server != INVALID_SOCKET);
        sockaddr_storage storage{};
        int length = v6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
        if (v6) {
            auto* a = reinterpret_cast<sockaddr_in6*>(&storage);
            a->sin6_family = AF_INET6; a->sin6_addr = in6addr_loopback;
        } else {
            auto* a = reinterpret_cast<sockaddr_in*>(&storage);
            a->sin_family = AF_INET; a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }
        assert(bind(server, reinterpret_cast<sockaddr*>(&storage), length) == 0);
        assert(getsockname(server, reinterpret_cast<sockaddr*>(&storage), &length) == 0);
        unsigned port = ntohs(v6 ? reinterpret_cast<sockaddr_in6*>(&storage)->sin6_port
                                  : reinterpret_cast<sockaddr_in*>(&storage)->sin_port);
        SOCKET client = INVALID_SOCKET, peer = INVALID_SOCKET;
        if (!udp) {
            assert(listen(server, 1) == 0);
            client = socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
            assert(connect(client, reinterpret_cast<sockaddr*>(&storage), length) == 0);
            peer = accept(server, nullptr, nullptr);
            assert(peer != INVALID_SOCKET);
        }
        auto snapshot = ConnectionCollector().Poll();
        assert(snapshot.errors.empty());
        bool found = false;
        for (const auto& r : snapshot.rows) if (r.pid == GetCurrentProcessId() &&
            r.localPort == port && r.ipv6 == v6 && r.udp == udp) {
            assert(r.local == (v6 ? "::1" : "127.0.0.1"));
            if (udp) assert(r.remote.empty() && r.remotePort == 0);
            if (udp || r.state == MIB_TCP_STATE_ESTAB) found = true;
        }
        assert(found);
        if (peer != INVALID_SOCKET) closesocket(peer);
        if (client != INVALID_SOCKET) closesocket(client);
        closesocket(server);
        snapshot = ConnectionCollector().Poll();
        assert(snapshot.errors.empty());
        for (const auto& r : snapshot.rows)
            assert(!(r.pid == GetCurrentProcessId() && r.localPort == port && r.ipv6 == v6 && r.udp == udp));
    }
    WSACleanup();
    std::puts("connection IPv4/IPv6 TCP/UDP tests passed");
}
