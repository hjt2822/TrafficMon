"""Generates loopback TCP/UDP traffic over IPv4 and IPv6 with known ports and payload sizes."""
import socket
import threading
import time

PAYLOAD = {"tcp4": 300, "tcp6": 310, "udp4": 200, "udp6": 210}
PORT = {"tcp4": 40125, "tcp6": 40126, "udp4": 40124, "udp6": 40123}


def tcp(fam, addr, key):
    srv = socket.socket(fam, socket.SOCK_STREAM)
    srv.bind((addr, PORT[key]))
    srv.listen(1)

    def serve():
        c, _ = srv.accept()
        n = 0
        while n < PAYLOAD[key]:
            data = c.recv(4096)
            if not data:
                break
            n += len(data)
        c.close()

    t = threading.Thread(target=serve)
    t.start()
    cli = socket.socket(fam, socket.SOCK_STREAM)
    cli.connect((addr, PORT[key]))
    cli.sendall(b"x" * PAYLOAD[key])
    time.sleep(0.3)
    cli.close()
    t.join()
    srv.close()


def udp(fam, addr, key):
    rx = socket.socket(fam, socket.SOCK_DGRAM)
    rx.bind((addr, PORT[key]))
    tx = socket.socket(fam, socket.SOCK_DGRAM)
    tx.sendto(b"y" * PAYLOAD[key], (addr, PORT[key]))
    rx.settimeout(2)
    rx.recvfrom(4096)
    rx.close()
    tx.close()


time.sleep(3)
tcp(socket.AF_INET, "127.0.0.1", "tcp4")
tcp(socket.AF_INET6, "::1", "tcp6")
udp(socket.AF_INET, "127.0.0.1", "udp4")
udp(socket.AF_INET6, "::1", "udp6")
print("done")
