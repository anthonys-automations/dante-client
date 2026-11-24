"""Minimal SOCKS5 server that delays every CONNECT reply (a slow proxy).

usage: slowsocks.py PORT DELAY_SECONDS [FAIL_EVERY [HOST [UDP_HOST]]]
Listens on HOST (default 127.0.0.1; e.g. ::1 for an IPv6-only proxy).
Every FAIL_EVERY-th CONNECT gets "host unreachable" (0 = never). After a
successful reply it writes b"hello\n" and closes.

UDP ASSOCIATE relays on UDP_HOST (default HOST) and echoes every datagram
back as if from its destination. A datagram to port 6666 is first answered
from 2001:db8::1 (an IPv6 source, which an AF_INET program can not see),
then echoed as usual.

SLOWSOCKS_UDP_TTL=N (environment) ends every UDP association after N seconds:
the server closes its control connection and the relay, as servers do on
idle timeouts or restarts.
"""
import asyncio
import os
import ipaddress
import struct
import sys

PORT = int(sys.argv[1])
DELAY = float(sys.argv[2])
FAIL_EVERY = int(sys.argv[3]) if len(sys.argv) > 3 else 0
HOST = sys.argv[4] if len(sys.argv) > 4 else "127.0.0.1"
UDP_HOST = sys.argv[5] if len(sys.argv) > 5 else HOST
count = 0
active = 0
peak = 0
udp = 0


def socksaddr(host, port):
    ip = ipaddress.ip_address(host)
    atyp = 1 if ip.version == 4 else 4
    return bytes([atyp]) + ip.packed + struct.pack("!H", port)


def parseaddr(data):
    """returns (header length, host, port) of a SOCKS5 address at data[0]."""
    atyp = data[0]
    if atyp == 1:
        n = 4
        host = str(ipaddress.ip_address(data[1:5]))
    elif atyp == 4:
        n = 16
        host = str(ipaddress.ip_address(data[1:17]))
    else:
        n = 1 + data[1]
        host = data[2 : 1 + n].decode()
    (port,) = struct.unpack("!H", data[1 + n : 3 + n])
    return 3 + n, host, port


class Relay(asyncio.DatagramProtocol):
    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        global udp
        udp += 1
        n, host, port = parseaddr(data[3:])
        payload = data[3 + n :]
        if port == 6666:
            self.transport.sendto(b"\0\0\0" + socksaddr("2001:db8::1", port)
                                  + b"v6:" + payload, addr)
        self.transport.sendto(data[: 3 + n] + payload, addr)


async def handle(reader, writer):
    global count, active, peak
    active += 1
    peak = max(peak, active)
    relay = None
    try:
        ver, nmethods = await reader.readexactly(2)
        await reader.readexactly(nmethods)
        writer.write(b"\x05\x00")
        await writer.drain()
        ver, cmd, _, atyp = await reader.readexactly(4)
        if atyp == 1:
            await reader.readexactly(4)
        elif atyp == 3:
            n = (await reader.readexactly(1))[0]
            await reader.readexactly(n)
        elif atyp == 4:
            await reader.readexactly(16)
        await reader.readexactly(2)
        bnd = writer.get_extra_info("sockname")
        if cmd == 3:  # UDP ASSOCIATE; lasts as long as the control connection.
            relay, _ = await asyncio.get_running_loop().create_datagram_endpoint(
                Relay, local_addr=(UDP_HOST, 0))
            bnd = relay.get_extra_info("sockname")
            writer.write(b"\x05\x00\x00" + socksaddr(bnd[0], bnd[1]))
            await writer.drain()
            ttl = float(os.environ.get("SLOWSOCKS_UDP_TTL", "0"))
            try:
                await asyncio.wait_for(reader.read(), ttl if ttl > 0 else None)
            except asyncio.TimeoutError:
                pass  # finally: closes the relay and the control connection
            return
        count += 1
        n = count
        await asyncio.sleep(DELAY)
        rep = 4 if FAIL_EVERY and n % FAIL_EVERY == 0 else 0
        writer.write(b"\x05" + bytes([rep]) + b"\x00" + socksaddr(bnd[0], bnd[1]))
        if rep == 0:
            writer.write(b"hello\n")
        await writer.drain()
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        active -= 1
        if relay is not None:
            relay.close()
        writer.close()


async def report():
    while True:
        await asyncio.sleep(1)
        print(f"socks: requests={count} active={active} peak={peak} udp={udp}",
              flush=True)


async def main():
    server = await asyncio.start_server(handle, HOST, PORT, backlog=1024)
    print("socks: ready", flush=True)
    asyncio.get_running_loop().create_task(report())
    async with server:
        await server.serve_forever()


asyncio.run(main())
