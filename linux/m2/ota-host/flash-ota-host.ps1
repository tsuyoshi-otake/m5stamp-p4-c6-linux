[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = "High")]
param(
	[Parameter(Mandatory = $true)]
	[string] $Port,

	[Parameter(Mandatory = $true)]
	[string] $PythonPath,

	[Parameter(Mandatory = $true)]
	[string] $ArtifactsDirectory,

	[Parameter(Mandatory = $true)]
	[string] $StockReadback,

	[switch] $AllowCandidateWrite,

	[string] $ExpectedStockSha256 = "229459f251eaf6222f0c07968702d72a3818e520da1788ce465027d969020c24"
)

# This image is a temporary P4 host used only to migrate the C6 slave over the
# already-wired SDIO link. It is not the Linux image and does not program a C6
# USB port. Keep the same explicit stock-readback gate as the Linux flasher.
$ErrorActionPreference = "Stop"
if (-not $AllowCandidateWrite) {
	throw "Refusing temporary OTA-host write. Re-run with -AllowCandidateWrite after reviewing the recovery procedure."
}

$artifactRoot = (Resolve-Path -LiteralPath $ArtifactsDirectory).Path
$stockPath = (Resolve-Path -LiteralPath $StockReadback).Path

function Assert-File([string] $Name, [UInt32] $MaxBytes) {
	$path = Join-Path $artifactRoot $Name
	if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
		throw "Missing artifact: $path"
	}
	$item = Get-Item -LiteralPath $path
	if ($item.Length -le 0 -or $item.Length -gt $MaxBytes) {
		throw "Artifact $Name has size $($item.Length), expected 1..$MaxBytes bytes"
	}
	return $path
}

if ((Get-Item -LiteralPath $stockPath).Length -ne 0x1000000) {
	throw "Stock readback must be exactly 16 MiB: $stockPath"
}
$stockHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $stockPath).Hash.ToLowerInvariant()
if ($stockHash -ne $ExpectedStockSha256.ToLowerInvariant()) {
	throw "Stock readback SHA-256 mismatch: got $stockHash, expected $ExpectedStockSha256"
}

$artifacts = @(
	@{ Address = "0x2000"; Name = "bootloader.bin"; Max = 0x6000 },
	@{ Address = "0x8000"; Name = "partition-table.bin"; Max = 0x1000 },
	@{ Address = "0xd000"; Name = "ota_data_initial.bin"; Max = 0x2000 },
	@{ Address = "0x10000"; Name = "host_performs_slave_ota.bin"; Max = 0x200000 },
	@{ Address = "0x5f0000"; Name = "network_adapter.bin"; Max = 0x200000 }
)
$paths = @{}
foreach ($artifact in $artifacts) {
	$paths[$artifact.Name] = Assert-File $artifact.Name $artifact.Max
}

# The C6 network-adapter image is intentionally stored in a P4 data
# partition.  Validate it as a C6 image before the write; esptool's normal
# image-chip check cannot be used when that raw image is included beside P4
# application images.
$c6Info = (& $PythonPath -m esptool --chip esp32c6 image_info $paths["network_adapter.bin"] 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) {
	throw "C6 slave image validation failed; no P4 or C6 bytes were written: $($c6Info.Trim())"
}

$versionOutput = (& $PythonPath -m esptool version 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $versionOutput -notmatch "4\.8\.1") {
	throw "This target is pinned to esptool 4.8.1; detected: $($versionOutput.Trim())"
}

Write-Host "P4 temporary OTA-host write gate passed"
Write-Host "  port=$Port"
Write-Host "  stock=$stockPath"
Write-Host "  stock_sha256=$stockHash"
Write-Host "  artifacts=$artifactRoot"
Write-Host "  C6 write: SDIO slave image at 0x5f0000 (no C6 USB/serial port)"

$chipArgs = @(
	"-m", "esptool", "--chip", "esp32p4", "--port", $Port,
	"--baud", "460800", "--before", "default_reset", "--after", "no_reset",
	"chip_id"
)
& $PythonPath @chipArgs
if ($LASTEXITCODE -ne 0) {
	throw "P4 ROM chip_id failed; no write was attempted"
}

$writeArgs = @(
	"-m", "esptool", "--chip", "esp32p4", "--port", $Port,
	"--baud", "460800", "--before", "default_reset", "--after", "no_reset",
	"write_flash", "-z", "--flash_mode", "dio", "--flash_freq", "80m",
	"--flash_size", "16MB"
)
foreach ($artifact in ($artifacts | Where-Object { $_.Name -ne "network_adapter.bin" })) {
	$writeArgs += $artifact.Address
	$writeArgs += $paths[$artifact.Name]
}

if ($WhatIfPreference) {
	Write-Host "WhatIf: no bytes written"
	return
}

& $PythonPath @writeArgs
if ($LASTEXITCODE -ne 0) {
	throw "P4 temporary OTA-host application write failed. Enter ROM mode and restore the preserved stock readback before retrying."
}

# This is the only deliberately forced write: the C6 binary is not a P4
# image, and it is being written as opaque data at the reserved slave_fw
# partition offset.  The C6 header was validated above and the offset/size
# are fixed by the checked-in partition map.
$c6WriteArgs = @(
	"-m", "esptool", "--chip", "esp32p4", "--port", $Port,
	"--baud", "460800", "--before", "default_reset", "--after", "no_reset",
	"write_flash", "--force", "0x5f0000", $paths["network_adapter.bin"]
)
& $PythonPath @c6WriteArgs
if ($LASTEXITCODE -ne 0) {
	throw "C6 slave image write failed after P4 updater write. Restore the preserved stock readback before retrying."
}

$verifyArgs = @(
	"-m", "esptool", "--chip", "esp32p4", "--port", $Port,
	"--baud", "460800", "--before", "default_reset", "--after", "no_reset",
	"verify_flash"
)
foreach ($artifact in $artifacts) {
	$verifyArgs += $artifact.Address
	$verifyArgs += $paths[$artifact.Name]
}
& $PythonPath @verifyArgs
if ($LASTEXITCODE -ne 0) {
	throw "P4 temporary OTA-host verify failed. Do not continue; restore the stock readback."
}

Write-Host "P4 temporary OTA-host write and verify passed"
Write-Host "Recovery command (only after entering ROM mode):"
Write-Host "  $PythonPath -m esptool --chip esp32p4 --port $Port write_flash 0x0 $stockPath"
