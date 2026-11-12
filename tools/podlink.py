#!/usr/bin/env python3
"""PodLink v1: RAM-only framed CDC messages and a localhost WebSocket bridge.

Dependencies: pyserial; 'serve' also requires websockets >= 14.
Open System > PodLink while unplugged, then connect USB.
"""
import argparse
import asyncio
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
import json
import math
import select
import struct
import threading
import time
import zlib

VID, PID = 0x05AC, 0x1261
MAGIC = b"PLNK"
HEADER = struct.Struct("<4sBBHII")
OPEN, TEXT, BINARY, PING, PONG, CLOSE, HELLO = range(1, 8)
MAX_MESSAGE = 4096
HELLO_DATA = struct.pack("<HI", 1, MAX_MESSAGE)


@dataclass(frozen=True)
class Frame:
    type: int
    seq: int
    payload: bytes


class LinkClosed(RuntimeError):
    def __init__(self, code, reason):
        self.code, self.reason = code, reason
        super().__init__(f"link closed ({code}): {reason}")


def encode(kind, seq, payload=b""):
    if kind not in range(1, 8) or len(payload) > MAX_MESSAGE:
        raise ValueError("invalid type or oversized message")
    header = HEADER.pack(MAGIC, kind, 0, 0, seq, len(payload))
    return header + payload + struct.pack("<I", zlib.crc32(header[4:] + payload))


class Decoder:
    def __init__(self):
        self.buffer = bytearray()
        self.crc_errors = 0

    def feed(self, data):
        # Bound incoming chunks as well as retained state.
        if len(data) > MAX_MESSAGE + 20:
            raise ValueError("decoder chunk too large")
        self.buffer.extend(data)
        frames = []
        while len(self.buffer) >= 4:
            if self.buffer[:4] != MAGIC:
                del self.buffer[0]
                continue
            if len(self.buffer) < HEADER.size:
                break
            _, kind, flags, reserved, seq, length = HEADER.unpack_from(self.buffer)
            if kind not in range(1, 8) or flags or reserved:
                del self.buffer[0]
                continue
            if length > MAX_MESSAGE:
                del self.buffer[0]
                raise LinkClosed(1009, "oversized frame")
            total = HEADER.size + length + 4
            if len(self.buffer) < total:
                break
            crc = struct.unpack_from("<I", self.buffer, total - 4)[0]
            if crc != zlib.crc32(self.buffer[4:total - 4]):
                self.crc_errors += 1
                del self.buffer[0]
                continue
            payload = bytes(self.buffer[HEADER.size:total - 4])
            del self.buffer[:total]
            frames.append(Frame(kind, seq, payload))
        return frames


def find_port(ports):
    found = [p.device for p in ports if p.vid == VID and p.pid == PID]
    if len(found) != 1:
        raise RuntimeError(f"expected one PodLink tty ({VID:04x}:{PID:04x}), found {found}; use --port")
    return found[0]


def pattern(offset, count):
    return bytes((offset + i) % 251 for i in range(count))


class Link:
    def __init__(self, port, timeout=10):
        self.port, self.timeout = port, timeout
        self.decoder, self.frames = Decoder(), deque()
        self.seq, self.max_message = 1, MAX_MESSAGE
        self.opened = False
        self.last_rx = time.monotonic()
        self.keepalive = None
        self.write_lock = threading.Lock()

    def send(self, kind, payload=b"", seq=None):
        if isinstance(payload, str):
            payload = payload.encode("utf-8")
        if len(payload) > self.max_message:
            raise ValueError("message exceeds negotiated maximum")
        with self.write_lock:
            if seq is None:
                seq = self.seq
                self.seq = (self.seq + 1) & 0xffffffff
            data = encode(kind, seq, payload)
            done = 0
            deadline = time.monotonic() + self.timeout
            while done < len(data):
                n = self.port.write(data[done:])
                if not n or time.monotonic() > deadline:
                    raise TimeoutError("serial write timeout")
                done += n
        return seq

    def poll(self):
        data = self.port.read(MAX_MESSAGE)
        if data:
            try:
                incoming = self.decoder.feed(data)
            except LinkClosed as error:
                self.send(CLOSE, struct.pack("<H", error.code))
                raise
            for frame in incoming:
                self.last_rx = time.monotonic()
                if frame.type == PING:
                    self.send(PONG, frame.payload, seq=frame.seq)
                    continue
                if frame.type == PONG and self.keepalive and frame.seq == self.keepalive[0]:
                    self.keepalive = None
                    continue
                if frame.type == CLOSE:
                    code = struct.unpack_from("<H", frame.payload)[0] if len(frame.payload) >= 2 else 1002
                    reason = frame.payload[2:].decode("utf-8", errors="replace")
                    if self.opened:
                        self.send(CLOSE, frame.payload, seq=frame.seq)
                    self.opened = False
                    raise LinkClosed(code, reason)
                if len(self.frames) >= 64:
                    raise LinkClosed(1009, "receive queue full")
                self.frames.append(frame)
        now = time.monotonic()
        if self.opened:
            if self.keepalive and now - self.keepalive[1] > 15:
                self.send(CLOSE, struct.pack("<H", 1001) + b"keepalive timeout")
                self.opened = False
                raise LinkClosed(1001, "keepalive timeout")
            if not self.keepalive and now - self.last_rx > 5:
                self.keepalive = self.send(PING), now

    def receive(self, timeout=None):
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while True:
            if self.frames:
                return self.frames.popleft()
            self.poll()
            if time.monotonic() > deadline:
                raise TimeoutError("message receive timeout")
            if not self.frames:
                try:
                    select.select([self.port.fileno()], [], [],
                                  min(.05, max(0, deadline - time.monotonic())))
                except (AttributeError, OSError):
                    time.sleep(.001)

    def open(self):
        seq = self.send(OPEN, HELLO_DATA)
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            frame = self.receive(max(0, deadline - time.monotonic()))
            if frame.type not in (HELLO, OPEN):
                continue
            if len(frame.payload) != 6:
                raise RuntimeError("bad handshake length")
            version, maximum = struct.unpack("<HI", frame.payload)
            if version != 1 or not 1 <= maximum <= MAX_MESSAGE:
                raise RuntimeError("unsupported handshake")
            self.max_message = maximum
            if frame.type == OPEN and frame.seq == seq:
                self.opened = True
                return
        raise TimeoutError("OPEN handshake timeout")

    def close(self, code=1000, reason=""):
        if not self.opened:
            return
        self.send(CLOSE, struct.pack("<H", code) + reason.encode("utf-8")[:120])
        self.opened = False  # receiving the ACK must not send another CLOSE
        deadline = time.monotonic() + min(self.timeout, 1)
        try:
            while time.monotonic() < deadline:
                self.receive(deadline - time.monotonic())
        except LinkClosed as error:
            if error.code != code:
                raise
            return
        raise TimeoutError("CLOSE handshake timeout")

    def expected(self, kind, payload=None):
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            frame = self.receive(deadline - time.monotonic())
            if frame.type == HELLO:
                continue
            if frame.type != kind or (payload is not None and frame.payload != payload):
                raise RuntimeError(f"unexpected message: {frame}")
            return frame
        raise TimeoutError("response timeout")

    def ping(self):
        payload = struct.pack("<Q", time.monotonic_ns())
        seq = self.send(PING, payload)
        frame = self.expected(PONG, payload)
        if frame.seq != seq:
            raise RuntimeError("PONG sequence mismatch")
        return "PONG"

    def info(self):
        self.send(TEXT, b"INFO")
        return json.loads(self.expected(TEXT).payload)

    def echo(self, text):
        self.send(TEXT, text)
        self.expected(TEXT, text.encode("utf-8"))
        return text

    def bench(self, count):
        if not 1 <= count <= 16 * 1024 * 1024:
            raise ValueError("bench size must be 1..16777216")
        results = {}
        self.send(TEXT, f"BENCH TX {count}")
        self.expected(TEXT, f"READY TX {count}".encode())
        start = time.monotonic()
        for offset in range(0, count, MAX_MESSAGE):
            self.send(BINARY, pattern(offset, min(MAX_MESSAGE, count - offset)))
        self.expected(TEXT, f"OK TX {count} errors=0".encode())
        seconds = time.monotonic() - start
        results["host_to_ipod"] = dict(bytes=count, seconds=seconds,
            bytes_per_second=count / seconds, integrity="PASS", message_bytes=MAX_MESSAGE)
        self.send(TEXT, f"BENCH RX {count}")
        self.expected(TEXT, f"READY RX {count}".encode())
        start = time.monotonic()
        offset = 0
        while offset < count:
            frame = self.expected(BINARY)
            if not frame.payload or len(frame.payload) > min(MAX_MESSAGE, count - offset):
                raise RuntimeError("invalid benchmark frame size")
            if frame.payload != pattern(offset, len(frame.payload)):
                raise RuntimeError(f"download integrity failed at {offset}")
            offset += len(frame.payload)
        self.expected(TEXT, f"OK RX {count}".encode())
        seconds = time.monotonic() - start
        results["ipod_to_host"] = dict(bytes=count, seconds=seconds,
            bytes_per_second=count / seconds, integrity="PASS", message_bytes=MAX_MESSAGE)
        return results

    def latency(self, samples):
        if not 1 <= samples <= 10000:
            raise ValueError("samples must be 1..10000")
        results = {}
        for name in ("ping_pong", "echo_64B"):
            times = []
            for _ in range(samples):
                start = time.perf_counter()
                if name == "ping_pong": self.ping()
                else:
                    self.send(BINARY, pattern(0, 64))
                    self.expected(BINARY, pattern(0, 64))
                times.append((time.perf_counter() - start) * 1000)
            times.sort()
            results[name] = dict(samples=samples, p50_ms=times[math.ceil(samples * .5) - 1],
                                 p95_ms=times[math.ceil(samples * .95) - 1])
        return results


def serial_port(device, timeout):
    import serial
    from serial.tools import list_ports
    return serial.Serial(device or find_port(list_ports.comports()), 115200,
                         timeout=0, write_timeout=timeout, exclusive=True)


async def serial_call(executor, function, *args):
    """Keep cancelled callers alive until their serial operation stops."""
    future = asyncio.get_running_loop().run_in_executor(executor, function, *args)
    try:
        return await asyncio.shield(future)
    except asyncio.CancelledError:
        try:
            await future
        except Exception:
            pass
        raise


async def bridge_session(ws, link, executor=None):
    """One serial owner; bounded websocket and serial buffers."""
    owned = executor is None
    if owned:
        executor = ThreadPoolExecutor(max_workers=1)

    async def from_ws():
        async for data in ws:
            await serial_call(executor, link.send,
                              TEXT if isinstance(data, str) else BINARY, data)

    async def from_serial():
        while True:
            await serial_call(executor, link.poll)
            while link.frames:
                frame = link.frames.popleft()
                if frame.type == TEXT:
                    await ws.send(frame.payload.decode("utf-8"))
                elif frame.type == BINARY:
                    await ws.send(frame.payload)
            await asyncio.sleep(.002)

    tasks = [asyncio.create_task(from_ws()), asyncio.create_task(from_serial())]
    try:
        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            await task
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        if owned:
            executor.shutdown(wait=True)


def bridge_handler(device, timeout):
    busy = asyncio.Lock()

    async def handler(ws):
        if ws.request.path != "/":
            await ws.close(1008, "use /")
            return
        if busy.locked():
            await ws.close(1013, "one client at a time")
            return
        async with busy:
            # Resolve tty again for every session; a replug may change its name.
            port = link = None
            executor = ThreadPoolExecutor(max_workers=1)

            def open_link():
                nonlocal port, link
                port = serial_port(device, timeout)
                link = Link(port, timeout)
                link.open()

            try:
                await serial_call(executor, open_link)
                await bridge_session(ws, link, executor)
            except LinkClosed as error:
                code = error.code if error.code in (1000, 1001, 1002, 1007, 1008, 1009, 1011) else 1001
                await ws.close(code, error.reason[:80])
            except UnicodeError:
                await ws.close(1007, "invalid UTF-8 TEXT")
            except (OSError, RuntimeError, TimeoutError, ValueError) as error:
                await ws.close(1011, str(error)[:80])
            finally:
                try:
                    if link and link.opened:
                        try:
                            code = ws.close_code if ws.close_code in (1000, 1001) else 1000
                            await serial_call(executor, link.close, code, "WebSocket closed")
                        except (OSError, RuntimeError, TimeoutError):
                            pass
                finally:
                    try:
                        if port:
                            await serial_call(executor, port.close)
                    finally:
                        executor.shutdown(wait=True)

    return handler


async def serve(device, port_number, timeout):
    from websockets.asyncio.server import serve as websocket_serve
    async with websocket_serve(bridge_handler(device, timeout), "127.0.0.1", port_number,
                               max_size=MAX_MESSAGE, max_queue=4, write_limit=8192,
                               compression=None):
        print(f"PodLink listening on ws://127.0.0.1:{port_number}/", flush=True)
        await asyncio.Future()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="override tty discovery (VID 05ac, PID 1261)")
    parser.add_argument("--timeout", type=float, default=10)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("ping", "info"):
        sub.add_parser(name)
    sub.add_parser("echo").add_argument("text")
    sub.add_parser("bench").add_argument("--bytes", type=int, default=1024 * 1024)
    sub.add_parser("latency").add_argument("--samples", type=int, default=100)
    sub.add_parser("serve").add_argument("--listen-port", type=int, default=8765)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    try:
        if args.command == "serve":
            asyncio.run(serve(args.port, args.listen_port, args.timeout))
            return
        with serial_port(args.port, args.timeout) as port:
            link = Link(port, args.timeout)
            link.open()
            try:
                if args.command == "echo": result = link.echo(args.text)
                elif args.command == "bench": result = link.bench(args.bytes)
                elif args.command == "latency": result = link.latency(args.samples)
                else: result = getattr(link, args.command)()
            finally:
                link.close()
            print(json.dumps(dict(port=port.port, result=result), ensure_ascii=False, indent=2))
    except (ImportError, OSError, RuntimeError, ValueError, TimeoutError) as error:
        parser.exit(1, f"podlink: {error}\n")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
