#!/usr/bin/env python3
"""
Stand SDR — signal classifier service.

Drop-in contract:
  1. Receive binary I/Q frames from Stand (see FRAME FORMAT below).
  2. Classify the modulation type.
  3. Send back a JSON result on the same TCP connection.

To plug in a real model, replace the classify() function.
Dependencies for the stub: none (stdlib only).
Dependencies for a real model: torch, numpy (or tflite, onnxruntime, etc.)

FRAME FORMAT (little-endian):
  [4B uint32  payload_length  — length of everything after these 4 bytes]
  [8B uint64  timestamp       — hardware sample counter from LimeSuite]
  [4B int32   sample_count N  — number of complex samples]
  [8B float64 sample_rate_hz  — samples per second]
  [N*2*2B int16 IQ pairs      — interleaved: I0, Q0, I1, Q1, ...]

RESPONSE FORMAT (newline-terminated JSON):
  {"type": "FM", "confidence": 0.95, "timestamp": 12345}

Supported type strings (extend as needed):
  FM, AM, CW, USB, LSB, NFM, Unknown
"""

import socket
import struct
import json
import random
import sys
import time

HOST = "127.0.0.1"
PORT = 52001

# Header inside the payload (after the 4-byte length prefix)
# Format: timestamp (uint64) + count (int32) + sample_rate (float64)
_HDR = struct.Struct("<QId")   # 8 + 4 + 8 = 20 bytes
_KNOWN_TYPES = ["FM", "AM", "CW", "USB", "LSB", "NFM"]


# ---------------------------------------------------------------------------
# Replace this function with real model inference.
# iq_samples: list of int16, interleaved I/Q  [I0, Q0, I1, Q1, ...]
# sample_rate: float, Hz
# Returns: (type_string, confidence_0_to_1)
# ---------------------------------------------------------------------------
def classify(iq_samples: list, sample_rate: float) -> tuple[str, float]:
    """Stub: returns a random result.  Replace with actual model."""
    return random.choice(_KNOWN_TYPES), round(random.uniform(0.5, 0.99), 3)


# ---------------------------------------------------------------------------
# Protocol helpers
# ---------------------------------------------------------------------------
def _recv_exact(conn: socket.socket, n: int) -> bytes | None:
    """Read exactly n bytes, return None on EOF."""
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def _handle(conn: socket.socket) -> None:
    print("[classifier] Client connected", flush=True)
    while True:
        # 1. Read 4-byte length prefix
        raw_len = _recv_exact(conn, 4)
        if raw_len is None:
            break
        payload_len = struct.unpack("<I", raw_len)[0]

        # 2. Read payload
        payload = _recv_exact(conn, payload_len)
        if payload is None:
            break

        # 3. Parse header
        if len(payload) < _HDR.size:
            print("[classifier] Frame too short, skipping", flush=True)
            continue
        timestamp, count, sample_rate = _HDR.unpack_from(payload, 0)

        # 4. Parse IQ samples
        iq_bytes = payload[_HDR.size:]
        expected = count * 2 * 2   # count complex samples × 2 ints × 2 bytes each
        if len(iq_bytes) < expected:
            print(f"[classifier] IQ data too short ({len(iq_bytes)} < {expected}), skipping",
                  flush=True)
            continue
        iq = list(struct.unpack_from(f"<{count * 2}h", iq_bytes))

        # 5. Classify
        mod_type, confidence = classify(iq, sample_rate)

        # 6. Send result
        result = json.dumps({
            "type":       mod_type,
            "confidence": confidence,
            "timestamp":  timestamp,
        }) + "\n"
        try:
            conn.sendall(result.encode())
        except OSError:
            break

    print("[classifier] Client disconnected", flush=True)


# ---------------------------------------------------------------------------
# Server loop
# ---------------------------------------------------------------------------
def main() -> None:
    print(f"[classifier] Listening on {HOST}:{PORT}", flush=True)
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            srv.bind((HOST, PORT))
        except OSError as e:
            print(f"[classifier] bind failed: {e}", flush=True)
            sys.exit(1)
        srv.listen(1)

        while True:
            conn, addr = srv.accept()
            with conn:
                _handle(conn)
            print("[classifier] Waiting for next connection…", flush=True)


if __name__ == "__main__":
    main()
