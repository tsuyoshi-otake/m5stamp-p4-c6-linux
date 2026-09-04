#!/usr/bin/env python3
"""Capture P1's raw P4 console bytes without text conversion."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import serial


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=300.0)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--reset",
        action="store_true",
        help="pulse the USB-serial reset lines after opening the port",
    )
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()

    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    if args.output.exists():
        raise SystemExit(f"refusing to overwrite capture: {args.output}")

    started = datetime.now(timezone.utc)
    digest = hashlib.sha256()
    byte_count = 0
    end = time.monotonic() + args.seconds
    try:
        with serial.Serial(
            port=args.port,
            baudrate=args.baud,
            timeout=0.1,
            write_timeout=0.1,
            dsrdtr=False,
            rtscts=False,
        ) as port, args.output.open("xb") as capture:
            print(
                f"Capturing raw UART from {args.port} for {args.seconds:g}s; "
                "press Ctrl-C to stop"
            )
            if args.reset:
                # Match esptool's USB-serial hard-reset sequence. The
                # capture remains open before the pulse, so boot output is
                # not lost and no UART command bytes are transmitted.
                port.dtr = False
                port.rts = True
                time.sleep(0.1)
                port.dtr = True
                port.rts = False
                time.sleep(0.1)
            while time.monotonic() < end:
                chunk = port.read(4096)
                if not chunk:
                    continue
                capture.write(chunk)
                capture.flush()
                digest.update(chunk)
                byte_count += len(chunk)
    except KeyboardInterrupt:
        print("Capture stopped by operator")
    except serial.SerialException as exc:
        raise SystemExit(f"UART capture failed: {exc}") from exc

    finished = datetime.now(timezone.utc)
    report = {
        "schema": 1,
        "captured_utc_start": started.isoformat(),
        "captured_utc_end": finished.isoformat(),
        "port": args.port,
        "baud": args.baud,
        "capture_seconds_requested": args.seconds,
        "reset_requested": args.reset,
        "bytes": byte_count,
        "sha256": digest.hexdigest(),
        "file": str(args.output),
        "operation": "raw byte capture; no UART commands transmitted",
    }
    if args.report:
        if args.report.exists():
            raise SystemExit(f"refusing to overwrite capture report: {args.report}")
        args.report.write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
    print(json.dumps(report, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
