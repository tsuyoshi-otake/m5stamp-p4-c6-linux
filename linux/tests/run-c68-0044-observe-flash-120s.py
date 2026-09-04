"""Flash a selected fail-closed C68 observation quartet and capture 120 s."""

import hashlib
import json
import os
import re
import subprocess
import time
from pathlib import Path


PYTHON = r"C:\Users\developer\tmp\easystick-p4-tools-481\Scripts\python.exe"
ESPTOOL = [PYTHON, "-m", "esptool"]
PORT = "COM10"
CAPTURE_PROFILE = os.environ.get(
    "EASYSTICK_CAPTURE_PROFILE", "c68-0044-observe-20260819"
)
CAPTURE_ROOT = Path(
    os.environ.get(
        "EASYSTICK_CAPTURE_ROOT",
        r"C:\Users\developer\tmp\easystick-m25-smp-20260817",
    )
)
CAPTURE_COMMAND = os.environ.get("EASYSTICK_CAPTURE_COMMAND", "").strip()
if "-stress-" in CAPTURE_PROFILE:
    if not CAPTURE_COMMAND:
        # Leave enough of the fixed 120 s capture window for the helper to
        # stop its persistent worker, take the final top snapshot, and emit
        # M5STAMP_STRESS PASS.
        CAPTURE_COMMAND = "/usr/sbin/m5stamp-smp-stress --seconds 90"
    elif CAPTURE_COMMAND.replace("\\", "/").split()[0].rsplit("/", 1)[-1] != (
        "m5stamp-smp-stress"
    ):
        raise SystemExit(
            "stress capture command must target /usr/sbin/m5stamp-smp-stress"
        )
elif "-l3-" in CAPTURE_PROFILE:
    if not CAPTURE_COMMAND:
        CAPTURE_COMMAND = "/usr/sbin/m5stamp-smp-smoke"
    elif CAPTURE_COMMAND.replace("\\", "/").split()[0].rsplit("/", 1)[-1] != (
        "m5stamp-smp-smoke"
    ):
        raise SystemExit(
            "L3 capture command must target /usr/sbin/m5stamp-smp-smoke"
        )
TRIPLE = CAPTURE_ROOT / f"{CAPTURE_PROFILE}-triple"
OUT_BIN = CAPTURE_ROOT / f"{CAPTURE_PROFILE}-120s.bin"
OUT_TXT = OUT_BIN.with_suffix(".txt")
OUT_JSON = OUT_BIN.with_suffix(".json")
WINDOW_SECONDS = 120.0
SERIAL_START_DELAY_SECONDS = float(
    os.environ.get("EASYSTICK_CAPTURE_SERIAL_START_DELAY", "0.05")
)

EXPECTED_BY_PROFILE = {
    "c68-0044-observe-20260819": {
        "boot-shim.bin": "c7ec1d122f6d067945402e006188996c88ca9f14fa550061e619b59f695110d7",
        "Image": "c6605d41c6b129a79a0654f1aa38798bc4d2fc5b9c064f7a6c93cedea925d7f5",
        "rootfs.squashfs": "424a59d74286ef3cb7ab557169277b8361e589c7ef673aec74e193296dce67b6",
        "easystick-stamp-p4.dtb": "675a04475b8671bcbbc01830c5927c1e5fd9c47f3382cb289a10a5e8b8519ee0",
    },
    "c68-0045-observe-20260819": {
        "boot-shim.bin": "c7ec1d122f6d067945402e006188996c88ca9f14fa550061e619b59f695110d7",
        "Image": "0df2840668ac358fa12b443bdb13c7d1a7af497f8bacb654c24e82824ccd47ed",
        "rootfs.squashfs": "424a59d74286ef3cb7ab557169277b8361e589c7ef673aec74e193296dce67b6",
        "easystick-stamp-p4.dtb": "675a04475b8671bcbbc01830c5927c1e5fd9c47f3382cb289a10a5e8b8519ee0",
    },
    "c68-0045-0047-observe-20260819": {
        "boot-shim.bin": "c7ec1d122f6d067945402e006188996c88ca9f14fa550061e619b59f695110d7",
        "Image": "6bee6c79b9f9e5cd03a5dc39ef2ba0931063739a7e25d6c1c4ace6d09dd95bc6",
        "rootfs.squashfs": "424a59d74286ef3cb7ab557169277b8361e589c7ef673aec74e193296dce67b6",
        "easystick-stamp-p4.dtb": "675a04475b8671bcbbc01830c5927c1e5fd9c47f3382cb289a10a5e8b8519ee0",
    },
    "c68-0045-0047-l3-20260819": {
        "boot-shim.bin": "c7ec1d122f6d067945402e006188996c88ca9f14fa550061e619b59f695110d7",
        "Image": "21541b5f4b89441658d7133f02904c988a896cb56c44eebc272f6ad33167a24d",
        "rootfs.squashfs": "0c96e2d14b51a90a4e414326098895bca41272c12040b9ce550075d350e5c557",
        "easystick-stamp-p4.dtb": "675a04475b8671bcbbc01830c5927c1e5fd9c47f3382cb289a10a5e8b8519ee0",
    },
}


def expected_hashes_for_profile() -> dict[str, str]:
    if CAPTURE_PROFILE in EXPECTED_BY_PROFILE:
        return EXPECTED_BY_PROFILE[CAPTURE_PROFILE]
    override = os.environ.get("EASYSTICK_EXPECTED_HASHES_JSON", "").strip()
    if not override:
        raise SystemExit(
            "unknown capture profile; provide EASYSTICK_EXPECTED_HASHES_JSON"
        )
    try:
        hashes = json.loads(override)
    except json.JSONDecodeError as error:
        raise SystemExit(f"invalid EASYSTICK_EXPECTED_HASHES_JSON: {error}")
    required = {"boot-shim.bin", "Image", "rootfs.squashfs", "easystick-stamp-p4.dtb"}
    if not isinstance(hashes, dict) or set(hashes) != required:
        raise SystemExit(
            "EASYSTICK_EXPECTED_HASHES_JSON must contain exactly "
            "boot-shim.bin, Image, rootfs.squashfs, easystick-stamp-p4.dtb"
        )
    normalized = {str(name): str(value).lower() for name, value in hashes.items()}
    if any(not re.fullmatch(r"[0-9a-f]{64}", value) for value in normalized.values()):
        raise SystemExit("EASYSTICK_EXPECTED_HASHES_JSON contains a non-SHA256 value")
    return normalized


EXPECTED = expected_hashes_for_profile()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


BOOT_EVENT_PATTERNS = (
    ("WAIT", "C68 WAIT"),
    ("RELEASE", "C68 RELEASE"),
    ("UP", "C67 UP "),
    ("START", "C67 START "),
    ("CALLIN", "C67 CALLIN"),
    ("ONLINE", "C67 ONLINE"),
    ("COMPLETE", "C67 COMPLETE"),
    ("UP_RETURN", "C67 UP_RETURN"),
    ("STARTUP", "C67 STARTUP"),
    ("BROUGHT_UP", "Brought up"),
    ("INIT", "Run /sbin/init"),
    ("SHELL", "ES_SSH SHELL_ENTER"),
    ("L3_PASS", "M5STAMP_L3SMOKE PASS"),
    ("STRESS_PASS", "M5STAMP_STRESS PASS"),
    ("STRESS_FAIL", "M5STAMP_STRESS FAIL"),
    ("TSENS_REF_PASS", "P4_TSENS_REF PASS"),
    ("TSENS_REF_FAIL", "P4_TSENS_REF FAIL"),
    ("TSENS_CLEANUP", "P4_TSENS_REF CLEANUP"),
    ("TSENS_LINUX_PASS", "P4_TSENS_LINUX PASS"),
)


def extract_boot_events(text: str) -> list[dict[str, object]]:
    events: list[dict[str, object]] = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        for name, marker in BOOT_EVENT_PATTERNS:
            if marker in line:
                events.append(
                    {
                        "line": line_number,
                        "event": name,
                        "marker": marker,
                    }
                )
    return events


def extract_tsens_reference(text: str) -> list[dict[str, object]]:
    references: list[dict[str, object]] = []
    pattern = re.compile(r"\bP4_TSENS_REF\s+(PASS|FAIL)(?:\s+(.*))?$")
    for line_number, line in enumerate(text.splitlines(), start=1):
        match = pattern.search(line)
        if not match:
            continue
        fields: dict[str, object] = {}
        for token in (match.group(2) or "").split():
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            try:
                fields[key] = int(value, 0)
            except ValueError:
                fields[key] = value
        references.append(
            {
                "line": line_number,
                "status": match.group(1),
                "fields": fields,
            }
        )
    return references


def extract_tsens_linux(text: str) -> list[dict[str, object]]:
    probes: list[dict[str, object]] = []
    pattern = re.compile(r"\bP4_TSENS_LINUX\s+PASS(?:\s+(.*))?$")
    for line_number, line in enumerate(text.splitlines(), start=1):
        match = pattern.search(line)
        if not match:
            continue
        fields: dict[str, object] = {}
        for token in (match.group(1) or "").split():
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            try:
                fields[key] = int(value, 0)
            except ValueError:
                fields[key] = value
        probes.append({"line": line_number, "fields": fields})
    return probes


def extract_stress_summaries(text: str) -> list[dict[str, object]]:
    summaries: list[dict[str, object]] = []
    pattern = re.compile(r"\bM5STAMP_STRESS PASS(?:\s+(.*))?$")
    for line_number, line in enumerate(text.splitlines(), start=1):
        match = pattern.search(line)
        if not match:
            continue
        fields: dict[str, object] = {}
        for token in (match.group(1) or "").split():
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            try:
                fields[key] = int(value, 0)
            except ValueError:
                fields[key] = value
        summaries.append({"line": line_number, "fields": fields})
    return summaries


def validate_tsens_capture(
    tsens_references: list[dict[str, object]],
    tsens_linux: list[dict[str, object]],
    stress_summaries: list[dict[str, object]],
    cleanup_seen: bool,
) -> list[str]:
    errors: list[str] = []
    reference = next(
        (
            item
            for item in reversed(tsens_references)
            if item.get("status") == "PASS"
        ),
        None,
    )
    linux = tsens_linux[-1] if tsens_linux else None
    if reference is None:
        errors.append("oracle-pass-missing")
    if not cleanup_seen:
        errors.append("oracle-cleanup-missing")
    if linux is None:
        errors.append("linux-pass-missing")
    if not stress_summaries:
        errors.append("stress-summary-missing")
        return errors

    if reference is not None and linux is not None:
        reference_fields = reference["fields"]
        linux_fields = linux["fields"]
        for key in ("efuse_raw", "delta_t", "range_reg", "dac_probe", "dac_readback"):
            if reference_fields.get(key) != linux_fields.get(key):
                errors.append(f"{key}-mismatch")
        for key in ("efuse_raw", "delta_t", "range_reg", "dac_probe", "dac_readback"):
            if key not in linux_fields:
                errors.append(f"linux-{key}-missing")

    fields = stress_summaries[-1]["fields"]
    required = (
        "temp_reads",
        "temp_failures",
        "temp_start_mc",
        "temp_min_mc",
        "temp_max_mc",
        "temp_end_mc",
    )
    for key in required:
        if key not in fields:
            errors.append(f"stress-{key}-missing")
    if any(key not in fields for key in required):
        return errors

    if fields["temp_reads"] <= 0:
        errors.append("stress-temp-reads-zero")
    if fields["temp_failures"] != 0:
        errors.append("stress-temp-read-failures")
    temperatures = [
        fields["temp_start_mc"],
        fields["temp_min_mc"],
        fields["temp_max_mc"],
        fields["temp_end_mc"],
    ]
    if any(value < -40000 or value > 125000 for value in temperatures):
        errors.append("stress-temperature-out-of-range")
    if fields["temp_min_mc"] > fields["temp_max_mc"]:
        errors.append("stress-temperature-range-inverted")
    return errors


def extract_top_blocks(text: str) -> list[list[str]]:
    blocks: list[list[str]] = []
    current: list[str] | None = None
    for line in text.splitlines():
        if "M5STAMP_TOP BEGIN" in line:
            current = [line]
            continue
        if current is None:
            continue
        current.append(line)
        if "M5STAMP_TOP END" in line:
            blocks.append(current)
            current = None
    return blocks


def classify_capture(
    text: str, command: str | None, command_sent: bool
) -> str:
    if "HP_SYS_HP_WDT_RESET" in text or "CPU has been reset by WDT" in text:
        return "watchdog-reset"
    if "Guru Meditation Error" in text or "Kernel panic" in text:
        return "panic"
    if "rcu_sched detected stalls" in text or "RCU Stall" in text:
        return "rcu-stall"
    if not command_sent and command:
        return "no-command-sent"
    if "M5STAMP_STRESS FAIL" in text:
        return "stress-fail"
    if "M5STAMP_L3SMOKE FAIL" in text:
        return "l3-smoke-fail"
    if "M5STAMP_STRESS PASS" in text or "M5STAMP_L3SMOKE PASS" in text:
        return "helper-pass"
    if "M5STAMP_STRESS topology" in text or "M5STAMP_STRESS WORKER_START" in text:
        return "stress-incomplete"
    if "Run /sbin/init" in text:
        return "userspace-no-helper"
    return "boot-incomplete"


def esptool(*args: str, check: bool = False) -> subprocess.CompletedProcess:
    return subprocess.run(
        ESPTOOL + list(args),
        check=check,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def wait_for_chip() -> None:
    for attempt in range(90):
        for before in ("default_reset", "usb_reset", "no_reset"):
            result = esptool(
                "--chip",
                "esp32p4",
                "--port",
                PORT,
                "--baud",
                "115200",
                "--before",
                before,
                "--after",
                "no_reset",
                "chip_id",
            )
            if result.returncode == 0:
                print(f"connected via {before} after {attempt} retries")
                return
        time.sleep(1)
    raise SystemExit(f"{PORT} is not reachable")


def verify_inputs() -> dict[str, str]:
    hashes = {}
    for name, expected in EXPECTED.items():
        path = TRIPLE / name
        actual = sha256_file(path)
        if actual != expected:
            raise SystemExit(f"SHA mismatch {name}: {actual} != {expected}")
        hashes[name] = actual
        print(f"SHA_OK {name} {actual}")
    return hashes


def flash_and_verify(hashes: dict[str, str]) -> None:
    wait_for_chip()
    common = (
        "--chip",
        "esp32p4",
        "--port",
        PORT,
        "--baud",
        "115200",
    )
    esptool(
        *common,
        "--before",
        "default_reset",
        "--after",
        "hard_reset",
        "write_flash",
        "-z",
        "--flash_mode",
        "keep",
        "--flash_freq",
        "keep",
        "--flash_size",
        "keep",
        "0x10000",
        str(TRIPLE / "boot-shim.bin"),
        "0x90000",
        str(TRIPLE / "Image"),
        "0x810000",
        str(TRIPLE / "rootfs.squashfs"),
        "0xf10000",
        str(TRIPLE / "easystick-stamp-p4.dtb"),
        check=True,
    )
    for offset, name in (
        ("0x10000", "boot-shim.bin"),
        ("0x90000", "Image"),
        ("0x810000", "rootfs.squashfs"),
        ("0xf10000", "easystick-stamp-p4.dtb"),
    ):
        esptool(
            *common,
            "--before",
            "default_reset",
            "--after",
            "no_reset",
            "verify_flash",
            offset,
            str(TRIPLE / name),
            check=True,
        )
    print("FLASH_AND_VERIFY_OK")


def capture(hashes: dict[str, str]) -> None:
    import serial

    reset = esptool(
        "--chip",
        "esp32p4",
        "--port",
        PORT,
        "--before",
        "default_reset",
        "--after",
        "hard_reset",
        "chip_id",
    )
    # Open the serial port soon after the reset command.  The boot-shim oracle
    # runs before Linux and can otherwise finish before the capture loop starts,
    # making a successful reference measurement invisible in the evidence.
    time.sleep(SERIAL_START_DELAY_SECONDS)

    buffer = bytearray()
    first_byte_seconds = None
    port_opens = 0
    started = time.time()
    deadline = started + WINDOW_SECONDS
    serial_port = None
    command_sent = False
    command_sent_seconds = None
    command_trigger = None
    console_activated = False
    while time.time() < deadline:
        if serial_port is None:
            try:
                serial_port = serial.Serial(
                    PORT, 115200, timeout=0.25, write_timeout=1
                )
                port_opens += 1
            except Exception:
                time.sleep(0.3)
                continue
        try:
            chunk = serial_port.read(65536)
        except Exception:
            try:
                serial_port.close()
            except Exception:
                pass
            serial_port = None
            time.sleep(0.3)
            continue
        if chunk:
            if first_byte_seconds is None:
                first_byte_seconds = round(time.time() - started, 3)
            buffer.extend(chunk)
            if (
                CAPTURE_COMMAND
                and not console_activated
                and b"Run /sbin/init" in buffer
            ):
                serial_port.write(b"\n")
                console_activated = True
            if CAPTURE_COMMAND and not command_sent:
                if b"~ #" in buffer:
                    command_trigger = "prompt"
                elif b"ES_HOSTKEY NONEMPTY_OK" in buffer:
                    command_trigger = "init-hostkey-ready"
                if command_trigger is not None:
                    serial_port.write((CAPTURE_COMMAND + "\n").encode("ascii"))
                    command_sent = True
                    command_sent_seconds = round(time.time() - started, 3)
    if serial_port is not None:
        serial_port.close()

    text = buffer.decode("utf-8", "replace")
    OUT_BIN.write_bytes(buffer)
    OUT_TXT.write_text(text, encoding="utf-8")
    patterns = [
        "C68 WAIT",
        "C68 RELEASE",
        "C67 UP ",
        "C67 START ",
        "C67 CALLIN",
        "C67 ONLINE",
        "C67 COMPLETE",
        "C67 STARTUP",
        "C67 UP_RETURN",
        "esp32p4-ipi: providing IPIs",
        "smp.c:176",
        "Brought up",
        "Run /sbin/init",
        "Kernel panic",
        "Guru Meditation Error",
        "HP_SYS_HP_WDT_RESET",
        "CPU has been reset by WDT",
        "rst:0x7 (HP_SYS_HP_WDT_RESET)",
        "WARNING:",
        "soft lockup",
        "RCU Stall",
        "rcu_sched detected stalls",
        "OBS timer-",
        "OBS cpu1-",
        "OBS cpu1-illegal",
        "M5STAMP L3 smoke endpoint",
        "M5STAMP_L3SMOKE PASS",
        "M5STAMP_L3SMOKE FAIL",
        "M5STAMP_STRESS topology",
        "M5STAMP_STRESS PASS",
        "M5STAMP_STRESS FAIL",
        "M5STAMP_TOP BEGIN",
        "M5STAMP_TOP END",
        "P4_TSENS_REF PASS",
        "P4_TSENS_REF FAIL",
        "P4_TSENS_REF CLEANUP",
        "P4_TSENS_LINUX PASS",
    ]
    hits = {pattern: (pattern in text) for pattern in patterns}
    brought_up = re.findall(r"Brought up\s+\d+\s+nodes?,\s+\d+\s+CPUs", text)
    last_c67 = [line for line in text.splitlines() if "C67 " in line][-12:]
    boot_events = extract_boot_events(text)
    tsens_references = extract_tsens_reference(text)
    tsens_linux = extract_tsens_linux(text)
    stress_summaries = extract_stress_summaries(text)
    top_blocks = extract_top_blocks(text)
    tsens_required = (
        os.environ.get("EASYSTICK_CAPTURE_TSENS", "0") == "1"
        or "-tsens-" in CAPTURE_PROFILE
    )
    tsens_validation_errors = (
        validate_tsens_capture(
            tsens_references,
            tsens_linux,
            stress_summaries,
            "P4_TSENS_REF CLEANUP" in text,
        )
        if tsens_required
        else []
    )
    classification = classify_capture(
        text, CAPTURE_COMMAND or None, command_sent
    )
    if tsens_validation_errors and classification not in {
        "watchdog-reset",
        "panic",
        "rcu-stall",
    }:
        classification = "tsens-fail"
    capture_manifest = {
        "schema": "easystick.c68-capture/v2",
        "profile": CAPTURE_PROFILE,
        "port": PORT,
        "seconds": WINDOW_SECONDS,
        "bytes": len(buffer),
        "sha256": hashlib.sha256(buffer).hexdigest(),
        "port_opens": port_opens,
        "first_byte_seconds": first_byte_seconds,
        "command": CAPTURE_COMMAND or None,
        "console_activated": console_activated,
        "command_sent": command_sent,
        "command_sent_seconds": command_sent_seconds,
        "command_trigger": command_trigger,
        "classification": classification,
        "boot_events": boot_events,
        "boot_event_names": [event["event"] for event in boot_events],
        "tsens_oracle": {
            "references": tsens_references,
            "cleanup_seen": "P4_TSENS_REF CLEANUP" in text,
        },
        "tsens_linux": {
            "probes": tsens_linux,
            "pass_seen": bool(tsens_linux),
        },
        "tsens_stress": {
            "summaries": stress_summaries,
            "temperature_summary_seen": any(
                "temp_reads" in summary["fields"]
                for summary in stress_summaries
            ),
        },
        "tsens_validation": {
            "required": tsens_required,
            "passed": not tsens_validation_errors,
            "errors": tsens_validation_errors,
        },
        "helper_lines": [
            line
            for line in text.splitlines()
            if "M5STAMP_L3SMOKE" in line or "M5STAMP_STRESS" in line
        ],
        "top_blocks": top_blocks,
        "top_lines": [line for block in top_blocks for line in block],
        "flash": {
            "boot-shim.bin": hashes["boot-shim.bin"],
            "Image": hashes["Image"],
            "rootfs.squashfs": hashes["rootfs.squashfs"],
            "dtb": hashes["easystick-stamp-p4.dtb"],
            "offsets": {
                "boot-shim.bin": "0x10000",
                "Image": "0x90000",
                "rootfs.squashfs": "0x810000",
                "dtb": "0xf10000",
            },
        },
        "reset": {
            "method": "esptool default_reset/hard_reset chip_id",
            "returncode": reset.returncode,
        },
        "hits": hits,
        "brought_up_matches": brought_up,
        "last_c67_lines": last_c67,
        "captured_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    OUT_JSON.write_text(json.dumps(capture_manifest, indent=2), encoding="utf-8")
    print(json.dumps(capture_manifest, indent=2))
    print("--- tail ---")
    print(text[-5000:] if text else "(empty)")


if __name__ == "__main__":
    artifact_hashes = verify_inputs()
    flash_and_verify(artifact_hashes)
    capture(artifact_hashes)
