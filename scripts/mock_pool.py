#!/usr/bin/env python3
"""Local mock Stratum V1 pool for end-to-end development testing.

This is a DEVELOPMENT TOOL, not part of the miner and not part of `ctest`
(unit tests never open a socket). Its purpose is to exercise the real network
path -- subscribe, authorize, notify, set_difficulty, submit -- and to verify
submitted shares INDEPENDENTLY of the C++ code, using Python's own hashlib.

A share is accepted only if this script can reproduce the miner's work from the
submitted parameters and confirm the resulting hash really meets the target. It
never accepts a share just because one arrived.

Usage:
    python3 scripts/mock_pool.py --port 3333 [--difficulty 0.001]

    # in another terminal
    ./build/bin/azd-miner --config config/config.json --start

Nothing here mines, pays out, or connects to the Bitcoin network.
"""

import argparse
import binascii
import hashlib
import json
import socket
import socketserver
import struct
import sys
import threading
import time

DIFF1_TARGET = 0x00000000FFFF0000000000000000000000000000000000000000000000000000


def sha256d(data: bytes) -> bytes:
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()


def swap_words(data: bytes) -> bytes:
    """Reverse the bytes within each 4-byte word, keeping word positions."""
    if len(data) % 4 != 0:
        raise ValueError("length must be a multiple of 4")
    return b"".join(data[i:i + 4][::-1] for i in range(0, len(data), 4))


def target_for_difficulty(difficulty: float) -> int:
    return int(DIFF1_TARGET / difficulty)


class Job:
    """A synthetic job. The coinbase is arbitrary bytes -- this is a protocol
    exerciser, not a block template builder."""

    def __init__(self, job_id: str):
        self.job_id = job_id
        # Previous block: the real genesis hash, so the wire encoding matches
        # what a real pool would send for a chain tip.
        genesis_display = bytes.fromhex(
            "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f")
        self.prev_internal = genesis_display[::-1]
        self.prev_stratum = swap_words(self.prev_internal)

        self.coinb1 = bytes.fromhex(
            "01000000010000000000000000000000000000000000000000000000000000"
            "000000000000ffffffff20020862")
        self.coinb2 = bytes.fromhex(
            "072f415a442f000000000100f2052a010000001976a914"
            "000000000000000000000000000000000000000088ac00000000")
        self.merkle_branches = [
            bytes.fromhex(
                "58bd1a9d4d5b2ca2a1a4e4c04e3a0a2b6b9f7d0a3e4b5c6d7e8f90a1b2c3d4e5"),
        ]
        self.version = 0x20000000
        self.nbits = 0x1D00FFFF
        self.ntime = int(time.time())

    def notify_params(self, clean_jobs: bool):
        return [
            self.job_id,
            self.prev_stratum.hex(),
            self.coinb1.hex(),
            self.coinb2.hex(),
            [branch.hex() for branch in self.merkle_branches],
            "%08x" % self.version,
            "%08x" % self.nbits,
            "%08x" % self.ntime,
            clean_jobs,
        ]

    def build_header(self, extranonce1: bytes, extranonce2: bytes, ntime: int,
                     nonce: int) -> bytes:
        coinbase = self.coinb1 + extranonce1 + extranonce2 + self.coinb2
        merkle_root = sha256d(coinbase)
        for branch in self.merkle_branches:
            merkle_root = sha256d(merkle_root + branch)

        return (struct.pack("<I", self.version)
                + self.prev_internal
                + merkle_root
                + struct.pack("<I", ntime)
                + struct.pack("<I", self.nbits)
                + struct.pack("<I", nonce))


class StratumHandler(socketserver.BaseRequestHandler):

    def setup(self):
        self.buffer = b""
        self.extranonce1 = bytes.fromhex("01020304")
        self.extranonce2_size = 4
        self.authorized = False
        self.jobs = {}
        self.job_counter = 0
        self.accepted = 0
        self.rejected = 0
        self.running = True

    def log(self, message):
        print("[pool] %s" % message, flush=True)

    def send(self, obj):
        line = json.dumps(obj) + "\n"
        self.request.sendall(line.encode())

    def new_job(self, clean_jobs):
        self.job_counter += 1
        job = Job("job%d" % self.job_counter)
        self.jobs[job.job_id] = job
        self.send({"id": None, "method": "mining.notify",
                   "params": job.notify_params(clean_jobs)})
        self.log("sent job %s (clean=%s)" % (job.job_id, clean_jobs))
        return job

    def handle(self):
        self.log("miner connected from %s" % (self.client_address,))
        self.request.settimeout(1.0)

        job_thread = None
        try:
            while self.running:
                try:
                    chunk = self.request.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    break

                self.buffer += chunk
                while b"\n" in self.buffer:
                    line, self.buffer = self.buffer.split(b"\n", 1)
                    if line.strip():
                        self.handle_line(line.decode().strip())

                if self.authorized and job_thread is None:
                    job_thread = threading.Thread(target=self.job_loop, daemon=True)
                    job_thread.start()
        except ConnectionError:
            pass
        finally:
            self.running = False
            self.log("miner disconnected (accepted=%d rejected=%d)"
                     % (self.accepted, self.rejected))

    def job_loop(self):
        """Send a fresh job periodically, exactly like a real pool."""
        while self.running:
            time.sleep(20)
            if not self.running:
                break
            try:
                self.new_job(clean_jobs=False)
            except OSError:
                break

    def handle_line(self, line):
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            self.log("dropping malformed line: %r" % line[:120])
            return

        method = message.get("method")
        message_id = message.get("id")
        params = message.get("params") or []

        if method == "mining.subscribe":
            self.log("mining.subscribe %s" % (params[0] if params else ""))
            self.send({
                "id": message_id,
                "result": [
                    [["mining.set_difficulty", "deadbeef"],
                     ["mining.notify", "cafebabe"]],
                    self.extranonce1.hex(),
                    self.extranonce2_size,
                ],
                "error": None,
            })

        elif method == "mining.authorize":
            worker = params[0] if params else "?"
            # The password is params[1]; deliberately never logged.
            self.log("mining.authorize worker=%s (password not logged)" % worker)
            self.send({"id": message_id, "result": True, "error": None})
            self.authorized = True
            self.send({"id": None, "method": "mining.set_difficulty",
                       "params": [self.server.difficulty]})
            self.log("set_difficulty %s" % self.server.difficulty)
            self.new_job(clean_jobs=True)

        elif method == "mining.submit":
            self.handle_submit(message_id, params)

        else:
            self.log("unhandled method %r" % method)
            self.send({"id": message_id, "result": None,
                       "error": [20, "Unknown method", None]})

    def handle_submit(self, message_id, params):
        if len(params) < 5:
            self.reject(message_id, 20, "Malformed submit")
            return

        worker, job_id, extranonce2_hex, ntime_hex, nonce_hex = params[:5]
        job = self.jobs.get(job_id)
        if job is None:
            self.reject(message_id, 21, "Job not found")
            return

        try:
            extranonce2 = bytes.fromhex(extranonce2_hex)
            ntime = int(ntime_hex, 16)
            nonce = int(nonce_hex, 16)
        except (ValueError, binascii.Error):
            self.reject(message_id, 20, "Malformed submit")
            return

        if len(extranonce2) != self.extranonce2_size:
            self.reject(message_id, 20, "Bad extranonce2 size")
            return

        # Independent verification with Python's hashlib.
        header = job.build_header(self.extranonce1, extranonce2, ntime, nonce)
        hash_internal = sha256d(header)
        hash_value = int.from_bytes(hash_internal, "little")
        target = target_for_difficulty(self.server.difficulty)

        display = hash_internal[::-1].hex()

        if hash_value <= target:
            self.accepted += 1
            achieved = DIFF1_TARGET // hash_value if hash_value else 0
            self.log("ACCEPT  job=%s nonce=%s hash=%s difficulty=%d"
                     % (job_id, nonce_hex, display, achieved))
            self.send({"id": message_id, "result": True, "error": None})
        else:
            self.rejected += 1
            self.log("REJECT  job=%s nonce=%s hash=%s (above target)"
                     % (job_id, nonce_hex, display))
            self.reject(message_id, 23, "Low difficulty share")

    def reject(self, message_id, code, text):
        self.send({"id": message_id, "result": False, "error": [code, text, None]})


class ThreadedServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    parser = argparse.ArgumentParser(description="Mock Stratum V1 pool (development tool)")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=3333)
    parser.add_argument("--difficulty", type=float, default=0.001,
                        help="share difficulty to hand out (low = shares arrive quickly)")
    parser.add_argument("--duration", type=float, default=0,
                        help="exit after N seconds (0 = run forever)")
    args = parser.parse_args()

    server = ThreadedServer((args.host, args.port), StratumHandler)
    server.difficulty = args.difficulty

    print("[pool] mock Stratum V1 pool on %s:%d (difficulty %s)"
          % (args.host, args.port, args.difficulty), flush=True)
    print("[pool] shares are verified independently with hashlib", flush=True)

    if args.duration > 0:
        threading.Timer(args.duration, server.shutdown).start()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()
        print("[pool] stopped", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
