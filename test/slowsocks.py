"""Minimal SOCKS5 CONNECT server that delays every CONNECT reply (a slow proxy).

usage: slowsocks.py PORT DELAY_SECONDS [FAIL_EVERY]
Every FAIL_EVERY-th request gets "host unreachable" (0 = never). After a
successful reply it writes b"hello\n" and closes.
"""
import asyncio
import sys

PORT = int(sys.argv[1])
DELAY = float(sys.argv[2])
FAIL_EVERY = int(sys.argv[3]) if len(sys.argv) > 3 else 0
count = 0
active = 0
peak = 0


async def handle(reader, writer):
    global count, active, peak
    active += 1
    peak = max(peak, active)
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
        count += 1
        n = count
        await asyncio.sleep(DELAY)
        rep = 4 if FAIL_EVERY and n % FAIL_EVERY == 0 else 0
        writer.write(bytes([5, rep, 0, 1, 0, 0, 0, 0, 0, 0]))
        if rep == 0:
            writer.write(b"hello\n")
        await writer.drain()
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        active -= 1
        writer.close()


async def report():
    while True:
        await asyncio.sleep(1)
        print(f"socks: requests={count} active={active} peak={peak}", flush=True)


async def main():
    server = await asyncio.start_server(handle, "127.0.0.1", PORT, backlog=1024)
    print("socks: ready", flush=True)
    asyncio.get_running_loop().create_task(report())
    async with server:
        await server.serve_forever()


asyncio.run(main())
