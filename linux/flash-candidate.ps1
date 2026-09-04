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

# This is an intentionally explicit P4-only write gate.  It does not flash
# the AddOn C6, erase NVS/PHY data, or infer a C6 programming port from COM10.
$ErrorActionPreference = "Stop"
if (-not $AllowCandidateWrite) {
	throw "Refusing candidate write. Re-run with -AllowCandidateWrite after reviewing the M2 map, stock readback, and recovery procedure."
}

$scriptRoot = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $scriptRoot "..\..\..\..")).Path
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
	@{ Address = "0x10000"; Name = "boot-shim.bin"; Max = 0x80000 },
	@{ Address = "0x90000"; Name = "Image"; Max = 0x720000 },
	@{ Address = "0x810000"; Name = "rootfs.squashfs"; Max = 0x700000 },
	@{ Address = "0xf10000"; Name = "easystick-stamp-p4.dtb"; Max = 0x10000 }
)
$paths = @{}
foreach ($artifact in $artifacts) {
	$paths[$artifact.Name] = Assert-File $artifact.Name $artifact.Max
}

$versionOutput = (& $PythonPath -m esptool version 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $versionOutput -notmatch "4\.8\.1") {
	throw "This target is pinned to esptool 4.8.1; detected: $($versionOutput.Trim())"
}

Write-Host "P4 candidate write gate passed"
Write-Host "  port=$Port"
Write-Host "  stock=$stockPath"
Write-Host "  stock_sha256=$stockHash"
Write-Host "  artifacts=$artifactRoot"
Write-Host "  C6 write: none (COM10 is the P4 module USB-C path)"

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
	"write_flash", "-z", "--flash_mode", "keep", "--flash_freq", "keep",
	"--flash_size", "keep"
)
foreach ($artifact in $artifacts) {
	$writeArgs += $artifact.Address
	$writeArgs += $paths[$artifact.Name]
}

$shouldWrite = -not [bool]$WhatIfPreference
if (-not $shouldWrite) {
	Write-Host "WhatIf: no bytes written"
	return
}

& $PythonPath @writeArgs
if ($LASTEXITCODE -ne 0) {
	throw "P4 candidate write failed. Enter ROM mode and restore the preserved stock readback before retrying."
}

$verifyArgs = @(
	"-m", "esptool", "--chip", "esp32p4", "--port", $Port,
	"--baud", "460800", "--before", "default_reset", "--after", "no_reset",
	"verify_flash"
)
foreach ($artifact in $artifacts) {
	# esptool rewrites the bootloader image-header flash parameter byte and
	# digest while programming.  Its write-side "Hash of data verified" is the
	# authoritative check for that artifact; verify the remaining artifacts
	# byte-for-byte against their build outputs below.
	if ($artifact.Name -eq "bootloader.bin") {
		continue
	}
	$verifyArgs += $artifact.Address
	$verifyArgs += $paths[$artifact.Name]
}
& $PythonPath @verifyArgs
if ($LASTEXITCODE -ne 0) {
	throw "P4 candidate verify failed. Do not continue to Linux acceptance; restore the stock readback."
}

Write-Host "P4 candidate write and verify passed"
Write-Host "Recovery command (only after entering ROM mode):"
Write-Host "  $PythonPath -m esptool --chip esp32p4 --port $Port write_flash 0x0 $stockPath"
