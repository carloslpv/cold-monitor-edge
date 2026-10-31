"""End-to-end test of the firmware running in QEMU.

The emulated ESP32 runs the real firmware with simulated probes and emulated Ethernet. This script plays the rest of
the world, with nothing but the Python standard library:

- an SNTP server on UDP 123, so the device can set its clock without internet;
- a TCP proxy on port 1883 in front of the real Mosquitto, so the test can cut the device's connection;
- an MQTT subscriber that records everything the device publishes.

It checks that readings arrive with sane values and timestamps, that the broker announces "offline" (the device's
last will) when the connection drops, and that every reading taken during the outage is delivered afterwards: the
seq numbers of the boot have no gaps.

    python3 e2e.py --broker-port 1884 --username admin --password <password> -- idf.py -B build-qemu qemu
"""

import argparse
import json
import queue
import select
import socket
import struct
import subprocess
import sys
import threading
import time

DEVICE = "cm-qemu"
PROBES = 2

# ---- SNTP --------------------------------------------------------------------------------------------------------

NTP_EPOCH_OFFSET = 2208988800


def ntp_timestamp(now: float) -> bytes:
    return struct.pack("!II", int(now) + NTP_EPOCH_OFFSET, int((now % 1) * (1 << 32)))


def serve_ntp(port: int) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))

    def loop() -> None:
        while True:
            request, client = sock.recvfrom(512)
            if len(request) < 48:
                continue
            now = time.time()
            # LI 0, version 4, mode 4 (server), stratum 1; the request's transmit time goes back as originate time
            header = struct.pack("!BBbb", 0x24, 1, 6, -20) + b"\0" * 8 + b"LOCL"
            sock.sendto(header + ntp_timestamp(now) + request[40:48] + ntp_timestamp(now) + ntp_timestamp(now), client)

    threading.Thread(target=loop, daemon=True, name="ntp").start()


# ---- TCP proxy that can cut the line -----------------------------------------------------------------------------


class Proxy:
    def __init__(self, listen_port: int, target_port: int) -> None:
        self.target_port = target_port
        self.server = socket.create_server(("0.0.0.0", listen_port), reuse_port=False)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.lock = threading.Lock()
        self.connections: list[socket.socket] = []
        self.down = False
        threading.Thread(target=self.accept_loop, daemon=True, name="proxy").start()

    def accept_loop(self) -> None:
        while True:
            client, _ = self.server.accept()
            with self.lock:
                if self.down:
                    client.close()
                    continue
                upstream = socket.create_connection(("127.0.0.1", self.target_port))
                self.connections += [client, upstream]
            threading.Thread(target=self.pipe, args=(client, upstream), daemon=True).start()

    def pipe(self, a: socket.socket, b: socket.socket) -> None:
        try:
            while True:
                ready, _, _ = select.select([a, b], [], [])
                for src in ready:
                    data = src.recv(65536)
                    if not data:
                        raise ConnectionError
                    (b if src is a else a).sendall(data)
        except (OSError, ConnectionError, ValueError):
            pass
        finally:
            self.close(a, b)

    def close(self, *socks: socket.socket) -> None:
        for s in socks:
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            s.close()

    def cut(self) -> None:
        with self.lock:
            self.down = True
            self.close(*self.connections)
            self.connections = []

    def restore(self) -> None:
        with self.lock:
            self.down = False


# ---- minimal MQTT 3.1.1 subscriber -------------------------------------------------------------------------------


def encode_length(n: int) -> bytes:
    out = bytearray()
    while True:
        byte, n = n % 128, n // 128
        out.append(byte | (0x80 if n else 0))
        if not n:
            return bytes(out)


def mqtt_string(s: str) -> bytes:
    data = s.encode()
    return struct.pack("!H", len(data)) + data


class Subscriber:
    def __init__(self, port: int, username: str, password: str, topic: str) -> None:
        self.messages: "queue.Queue[tuple[float, str, str, bool]]" = queue.Queue()
        self.sock = socket.create_connection(("127.0.0.1", port))
        payload = mqtt_string("e2e-test") + mqtt_string(username) + mqtt_string(password)
        variable = mqtt_string("MQTT") + bytes([4, 0xC2]) + struct.pack("!H", 60)  # user, password, clean session
        self.send(0x10, variable + payload)
        packet_type, body = self.read_packet()
        if packet_type != 0x20 or body[1] != 0:
            raise RuntimeError(f"broker refused the test subscriber (CONNACK {body.hex()})")
        self.send(0x82, struct.pack("!H", 1) + mqtt_string(topic) + bytes([1]))
        threading.Thread(target=self.read_loop, daemon=True, name="mqtt").start()
        threading.Thread(target=self.ping_loop, daemon=True, name="ping").start()

    def send(self, header: int, body: bytes) -> None:
        self.sock.sendall(bytes([header]) + encode_length(len(body)) + body)

    def read_exact(self, n: int) -> bytes:
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise ConnectionError("broker closed the test subscriber")
            data += chunk
        return data

    def read_packet(self) -> tuple[int, bytes]:
        header = self.read_exact(1)[0]
        length, shift = 0, 0
        while True:
            byte = self.read_exact(1)[0]
            length |= (byte & 0x7F) << shift
            shift += 7
            if not byte & 0x80:
                break
        return header, self.read_exact(length)

    def read_loop(self) -> None:
        while True:
            header, body = self.read_packet()
            if header >> 4 != 3:  # PUBLISH
                continue
            qos = (header >> 1) & 3
            topic_len = struct.unpack("!H", body[:2])[0]
            topic = body[2 : 2 + topic_len].decode()
            rest = body[2 + topic_len :]
            if qos:
                packet_id, rest = rest[:2], rest[2:]
                self.send(0x40, packet_id)  # PUBACK
            self.messages.put((time.time(), topic, rest.decode(), bool(header & 1)))

    def ping_loop(self) -> None:
        while True:
            time.sleep(30)
            self.send(0xC0, b"")


# ---- the test ----------------------------------------------------------------------------------------------------


class Failure(Exception):
    pass


class Run:
    def __init__(self, subscriber: Subscriber) -> None:
        self.subscriber = subscriber
        self.readings: list[dict] = []
        self.statuses: list[tuple[float, str]] = []

    def pump(self, timeout: float) -> None:
        try:
            received, topic, payload, retained = self.subscriber.messages.get(timeout=timeout)
        except queue.Empty:
            return
        if retained:
            return  # left over from an earlier run
        if topic.endswith("/readings"):
            reading = json.loads(payload)
            reading["received"] = received
            self.readings.append(reading)
        elif topic.endswith("/status"):
            self.statuses.append((received, payload))
            print(f"  status: {payload}")

    def wait_for(self, what: str, condition, timeout: float) -> None:
        deadline = time.time() + timeout
        while not condition():
            if time.time() > deadline:
                raise Failure(f"timed out after {timeout:.0f} s waiting for {what}")
            self.pump(0.5)

    def status_since(self, since: float, value: str) -> bool:
        return any(t >= since and s == value for t, s in self.statuses)

    def readings_per_probe(self) -> dict[str, int]:
        counts: dict[str, int] = {}
        for r in self.readings:
            counts[r["probe"]] = counts.get(r["probe"], 0) + 1
        return counts


def check_reading(r: dict) -> None:
    if r["device"] != DEVICE:
        raise Failure(f"unexpected device in {r}")
    if not (len(r["probe"]) == 16 and r["probe"].startswith("28")):
        raise Failure(f"probe {r['probe']} is not a DS18B20 ROM code")
    if not (3.0 <= r["t"] <= 7.0 or -27.0 <= r["t"] <= -23.0):
        raise Failure(f"temperature {r['t']} out of the simulated ranges")
    if "ts" not in r:
        raise Failure(f"reading without a timestamp: {r}")
    if not (r["received"] - 600 <= r["ts"] <= r["received"] + 5):
        raise Failure(f"timestamp {r['ts']} is not close to the arrival time {r['received']:.0f}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--broker-port", type=int, default=1884, help="where the real Mosquitto listens")
    parser.add_argument("--proxy-port", type=int, default=1883, help="where the device connects")
    parser.add_argument("--ntp-port", type=int, default=123)
    parser.add_argument("--username", default="admin")
    parser.add_argument("--password", required=True)
    parser.add_argument("--outage", type=float, default=30, help="seconds the connection stays cut")
    parser.add_argument("--log", default="qemu.log", help="file for the device's serial output")
    parser.add_argument("qemu", nargs=argparse.REMAINDER, help="command that starts QEMU, after --")
    args = parser.parse_args()
    command = args.qemu[1:] if args.qemu[:1] == ["--"] else args.qemu
    if not command:
        parser.error("give the QEMU command after --")

    serve_ntp(args.ntp_port)
    proxy = Proxy(args.proxy_port, args.broker_port)
    run = Run(Subscriber(args.broker_port, args.username, args.password, f"coldmonitor/{DEVICE}/#"))

    with open(args.log, "w") as log:
        device = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        try:
            started = time.time()
            print("1. device boots, sets its clock and publishes")
            run.wait_for('"online"', lambda: run.status_since(started, "online"), 180)
            run.wait_for("3 readings of each probe",
                         lambda: len(run.readings_per_probe()) == PROBES
                         and min(run.readings_per_probe().values()) >= 3, 120)
            for r in run.readings:
                check_reading(r)
            print(f"  {len(run.readings)} readings, e.g. {json.dumps({k: v for k, v in run.readings[-1].items() if k != 'received'})}")

            print(f"2. connection cut for {args.outage:.0f} s")
            cut_at = time.time()
            proxy.cut()
            run.wait_for('"offline" from the broker (last will)', lambda: run.status_since(cut_at, "offline"), 20)
            time.sleep(max(0.0, args.outage - (time.time() - cut_at)))
            last_seq_before = max(r["seq"] for r in run.readings)

            print("3. connection restored")
            restored_at = time.time()
            proxy.restore()
            run.wait_for('"online" again', lambda: run.status_since(restored_at, "online"), 60)
            expected_backlog = int(args.outage / 5) * PROBES
            run.wait_for(f"the backlog (about {expected_backlog} readings) and fresh readings",
                         lambda: max(r["seq"] for r in run.readings) >= last_seq_before + expected_backlog + 2 * PROBES,
                         90)
            # let any reading still in flight arrive before counting
            end = time.time() + 6
            while time.time() < end:
                run.pump(0.5)
        finally:
            device.terminate()
            try:
                device.wait(timeout=10)
            except subprocess.TimeoutExpired:
                device.kill()

    for r in run.readings:
        check_reading(r)
    boots = {r["boot"] for r in run.readings}
    if len(boots) != 1:
        raise Failure(f"the device rebooted during the test (boot ids {boots}); see {args.log}")
    seqs = sorted({r["seq"] for r in run.readings})
    missing = sorted(set(range(seqs[0], seqs[-1] + 1)) - set(seqs))
    if missing:
        raise Failure(f"readings lost: seq {missing}")
    during_outage = [r for r in run.readings if cut_at <= r["ts"] <= restored_at]
    if len(during_outage) < expected_backlog - PROBES:
        raise Failure(f"only {len(during_outage)} readings are dated inside the outage, expected about {expected_backlog}")
    duplicates = len(run.readings) - len(seqs)

    print(f"4. ok: seq {seqs[0]}..{seqs[-1]} with no gaps, {len(during_outage)} readings taken during the outage "
          f"delivered afterwards with their own timestamps, {duplicates} duplicate(s)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Failure as failure:
        print(f"FAIL: {failure}", file=sys.stderr)
        sys.exit(1)
