[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)]
	[string] $Port,

	[Parameter(Mandatory = $true)]
	[string] $Output,

	[string] $PythonPath = "python3",
	[int] $TimeoutSeconds = 40,
	[switch] $Reset
)

# Read-only evidence collector. It never invokes esptool and never writes the
# target flash. Keep the raw bytes: terminal rendering can hide framing,
# non-UTF8 bytes, and early boot markers.
#
# Two properties this collector has to hold, both learned the hard way on this
# target (see a0-lastgood-restore-and-rootfs-bisect-2026-08-12.md §8):
#
#  1. Opening the port must not reset the board.  pyserial's ordinary
#     constructor asserts DTR *and* RTS while opening, and on an ESP32
#     USB-Serial/JTAG that pair is the reset/boot strap -- so the obvious
#     "read-only" capture restarts the target it is measuring, and every
#     capture window then contains a fresh boot that the capture itself
#     caused.  The port is therefore constructed closed, both control lines
#     are forced low, and only then opened.  Without -Reset nothing in this
#     script ever raises either line.
#
#  2. It must reopen for the whole window, not just until the first byte.  The
#     USB-Serial/JTAG device re-enumerates when the running image
#     re-initializes it, which silently invalidates an open handle -- once at
#     shim-to-kernel handoff, and potentially again later.  A collector that
#     stops reopening after its first byte goes deaf at the first
#     re-enumeration and reports a truncated boot as a complete one.  So a
#     silent handle is closed and reopened until the deadline, and the number
#     of opens plus the arrival time of the first byte are recorded as
#     evidence rather than hidden as retries.
#
# With -Reset, use the esptool HardReset control-line sequence rather than an
# RTS-only pulse.  USB-Serial/JTAG can otherwise leave a P4 that is currently
# in ROM download mode in that same mode; the RTS transition must be followed
# by an explicit DTR update on Windows' usbser.sys path.
#
# Note on the payload below: it is passed to python -c, and the Windows native
# argument marshaller eats double-quote characters out of that argument -- an
# empty pair such as b"" collapses and produces a syntax error. The payload
# therefore uses single quotes throughout and builds the file mode with chr().
$outputPath = [System.IO.Path]::GetFullPath($Output)
$outputDir = Split-Path -Parent $outputPath
if ($outputDir) {
	New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
}

$python = @'
import sys, time
import serial

port, output, timeout_s, do_reset = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4] == '1'
started_at = time.time()
deadline = started_at + timeout_s
chunks = []
opens = 0
first_byte_at = None


def open_passive(name):
    # Construct closed, force both control lines low, then open, so that
    # opening the port cannot drive the target reset strap.
    s = serial.Serial()
    s.port = name
    s.baudrate = 115200
    s.timeout = 2
    s.dtr = False
    s.rts = False
    s.open()
    return s


def set_rts_with_dtr_update(s, state):
    # Matches esptool's ResetStrategy._setRTS workaround for usbser.sys:
    # resend DTR so the updated RTS state reaches the device.
    s.rts = state
    s.dtr = s.dtr


def hard_reset(s):
    set_rts_with_dtr_update(s, True)
    time.sleep(0.1)
    set_rts_with_dtr_update(s, False)


while time.time() < deadline:
    try:
        s = open_passive(port)
    except Exception:
        time.sleep(0.2)
        continue
    opens += 1
    try:
        if do_reset and opens == 1:
            hard_reset(s)
        silent = 0
        while time.time() < deadline:
            block = s.read(65536)
            if block:
                chunks.append(block)
                if first_byte_at is None:
                    first_byte_at = round(time.time() - started_at, 3)
                silent = 0
            else:
                silent += 1
                if silent >= 2:
                    # Four seconds of silence on this handle.  Reopen: that is
                    # what a stale post-re-enumeration handle looks like, and
                    # it is indistinguishable from a quiet target here.
                    break
    finally:
        try:
            s.close()
        except Exception:
            pass
    time.sleep(0.1)

data = bytes().join(chunks)
with open(output, chr(119) + chr(98)) as f:
    f.write(data)
print(len(data))
print(opens)
print(-1 if first_byte_at is None else first_byte_at)
'@

$resetValue = if ($Reset) { "1" } else { "0" }
$reportText = & $PythonPath -c $python $Port $outputPath $TimeoutSeconds $resetValue
if ($LASTEXITCODE -ne 0) {
	throw "serial capture failed with exit code $LASTEXITCODE"
}
$report = @($reportText)
$openCount = $report[$report.Count - 2]
$firstByte = $report[$report.Count - 1]

$sha256 = [System.Security.Cryptography.SHA256]::Create()
$stream = [System.IO.File]::OpenRead($outputPath)
try {
	$hashBytes = $sha256.ComputeHash($stream)
}
finally {
	$stream.Dispose()
	$sha256.Dispose()
}
$hash = (-join ($hashBytes | ForEach-Object { $_.ToString("x2") }))
$manifestPath = "$outputPath.json"
$manifest = [ordered]@{
	port = $Port
	baud = 115200
	bytes = (Get-Item -LiteralPath $outputPath).Length
	sha256 = $hash
	reset_requested = [bool]$Reset
	control_lines_driven = [bool]$Reset
	port_opens = [int]$openCount
	first_byte_seconds = [double]$firstByte
	captured_utc = (Get-Date).ToUniversalTime().ToString("o")
	python = $PythonPath
	reset_strategy = if ($Reset) { "esptool-HardReset-compatible-RTS-DTR" } else { "none" }
}
$manifest | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath $manifestPath
Write-Host "Captured $($manifest.bytes) bytes to $outputPath"
Write-Host "SHA256 $hash"
Write-Host "port_opens $($manifest.port_opens) first_byte_seconds $($manifest.first_byte_seconds) control_lines_driven $($manifest.control_lines_driven)"
