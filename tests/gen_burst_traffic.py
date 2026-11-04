"""Send one loopback UDP datagram and exit immediately (short-lived process)."""
import os
import socket

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
payload = b'burst-traffic' * 4
sock.sendto(payload, ('127.0.0.1', 9))  # no listener needed; send event still fires
sock.close()
print(f'{os.getpid()} {len(payload)}', flush=True)