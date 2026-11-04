"""Sends a real UDP DNS query to a public resolver so ETW UDP send/receive events carry an external address."""
import socket
import struct
import time

time.sleep(3)
query = struct.pack(">HHHHHH", 0x1234, 0x0100, 1, 0, 0, 0) + b"\x07example\x03com\x00" + struct.pack(">HH", 1, 1)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(3)
s.sendto(query, ("1.1.1.1", 53))
resp, addr = s.recvfrom(4096)
print("query", len(query), "bytes, response", len(resp), "bytes from", addr, "local", s.getsockname())
s.close()
