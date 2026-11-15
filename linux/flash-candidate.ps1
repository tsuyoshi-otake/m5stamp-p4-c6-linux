[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = "High")]
param(
	[Parameter(Mandatory = $true)]
	[string] $Port,

	[string] $PythonPath = "python",

	[Alias("ArtifactsDir")]
	[string] $ArtifactsDirectory = $PSScriptRoot,

	[string] $StockReadback,

	[switch] $AllowCandidateWrite,

	# Default is to preserve the boot slots (0xf40000/0xf80000) and the
	# bootmeta A/B selection — writing boot.img makes the next boot commit
	# the staged template over the live boot FS (user Wi-Fi config lives
	# there).  -WriteBoot is the only way to write them.
	[switch] $WriteBoot,

	# Accepted for compatibility; preserving the boot slots is now the
	# default, so this switch is a no-op.
	[switch] $PreserveBoot,

	# Default is to hard-reset the board after the write verifies, so Linux
	# boots without a manual reset.  -NoReset keeps the old behaviour of
	# leaving the board in download mode.
	[switch] $NoReset,

	[string] $ExpectedStockSha256 = "229459f251eaf6222f0c07968702d72a3818e520da1788ce465027d969020c24"
)

# This is an intentionally explicit P4-only write gate.  It does not flash
# the AddOn C6, erase NVS/PHY data, or infer a C6 programming port from COM10.
$ErrorActionPreference = "Stop"
if (-not $AllowCandidateWrite) {
	throw "Refusing candidate write. Re-run with -AllowCandidateWrite after reviewing the M2 map, stock readback, and recovery procedure."
}

$artifactRoot = (Resolve-Path -LiteralPath $ArtifactsDirectory).Path
$stockPath = $null

# The package decides the board: a Tab5 package carries esp32p4-tab5.dtb, a
# Stamp-P4 package esp32p4-m5stamp-p4.dtb.  The flash map is the same; the
# DTB name and the stock image the readback is checked against are not.
$dtbName = "esp32p4-m5stamp-p4.dtb"
if (Test-Path -LiteralPath (Join-Path $artifactRoot "esp32p4-tab5.dtb") -PathType Leaf) {
	$dtbName = "esp32p4-tab5.dtb"
	if (-not $PSBoundParameters.ContainsKey("ExpectedStockSha256")) {
		$ExpectedStockSha256 = "a260eda93e4e5ddc1d3abddd361b1eb326c07d4248b67d1b493feba057fde231"
	}
}

# SHA-256 through .NET.  Get-FileHash is in a module that Windows PowerShell
# cannot load when it is started from a PowerShell 7 shell: it inherits that
# shell's PSModulePath and finds the wrong Microsoft.PowerShell.Utility.
function Get-Sha256([string] $Path) {
	$stream = [System.IO.File]::OpenRead($Path)
	try {
		$sha = [System.Security.Cryptography.SHA256]::Create()
		try {
			$bytes = $sha.ComputeHash($stream)
		} finally {
			$sha.Dispose()
		}
	} finally {
		$stream.Dispose()
	}
	return ([System.BitConverter]::ToString($bytes) -replace "-", "").ToLowerInvariant()
}

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

if ($StockReadback) {
	$stockPath = (Resolve-Path -LiteralPath $StockReadback).Path
	if ((Get-Item -LiteralPath $stockPath).Length -ne 0x1000000) {
		throw "Stock readback must be exactly 16 MiB: $stockPath"
	}
	$stockHash = Get-Sha256 $stockPath
	if ($stockHash -ne $ExpectedStockSha256.ToLowerInvariant()) {
		throw "Stock readback SHA-256 mismatch: got $stockHash, expected $ExpectedStockSha256"
	}
} else {
	$stockHash = "not supplied"
}

$artifacts = @(
	@{ Address = "0x2000"; Name = "bootloader.bin"; Max = 0x6000 },
	@{ Address = "0x8000"; Name = "partition-table.bin"; Max = 0x1000 },
	@{ Address = "0x10000"; Name = "boot-shim.bin"; Max = 0x80000 },
	@{ Address = "0x90000"; Name = "Image"; Max = 0x780000 },
	@{ Address = "0x810000"; Name = "rootfs.squashfs"; Max = 0x700000 },
	@{ Address = "0xf10000"; Name = $dtbName; Max = 0x10000 }
)
if ($WriteBoot) {
	$artifacts += @{ Address = "0xf40000"; Name = "boot.img"; Max = 0x40000 }
}
$paths = @{}
foreach ($artifact in $artifacts) {
	$paths[$artifact.Name] = Assert-File $artifact.Name $artifact.Max
}

# Written-vs-booted reconciliation: the kernel image carries its "Linux
# version ..." banner, and the running kernel prints it again on boot.  The
# part from '#' is what `uname -v` reports; it identifies the exact build.
$kernelBanner = $null
$unameV = $null
$imageText = [System.Text.Encoding]::ASCII.GetString(
	[System.IO.File]::ReadAllBytes($paths["Image"]))
# The image holds two "Linux version" strings: a link-time placeholder that
# ends in "# SMP" with no build number, and the real banner.  Only the one
# with a build number and date is what the running kernel reports.
$bannerMatch = [regex]::Match($imageText,
	"Linux version \d[\x20-\x7e]*? (#\d+ [\x20-\x7e]+)")
if ($bannerMatch.Success) {
	$kernelBanner = $bannerMatch.Value
	$unameV = $bannerMatch.Groups[1].Value
}

$versionOutput = (& $PythonPath -m esptool version 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $versionOutput -notmatch "4\.8\.1") {
	throw "This target is pinned to esptool 4.8.1; detected: $($versionOutput.Trim())"
}

Write-Host "P4 candidate write gate passed"
Write-Host "  port=$Port"
if ($stockPath) {
	Write-Host "  stock=$stockPath"
} else {
	Write-Host "  stock=not supplied"
}
Write-Host "  stock_sha256=$stockHash"
Write-Host "  artifacts=$artifactRoot"
foreach ($artifact in $artifacts) {
	$artifactHash = Get-Sha256 $paths[$artifact.Name]
	Write-Host "    $($artifact.Address) sha256=$artifactHash  $($artifact.Name)"
}
if ($kernelBanner) {
	Write-Host "  kernel=$kernelBanner"
} else {
	Write-Warning "No 'Linux version' banner found in Image; the booted kernel cannot be checked against this write."
}
if ($WriteBoot) {
	Write-Host "  boot slots: WRITE boot.img + reset A/B metadata (-WriteBoot)"
} else {
	Write-Host "  boot slots: preserved (default; pass -WriteBoot to update the boot FS)"
}
if ($NoReset) {
	Write-Host "  reset: none (-NoReset); the board stays in download mode"
} else {
	Write-Host "  reset: hard reset after the last flash operation"
}
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

# With -WriteBoot the A/B metadata erase follows and does the reset itself,
# so verify leaves the board in download mode for it.  Otherwise verify is
# the last flash operation and resets the board into the new image.
$verifyAfter = "hard_reset"
if ($WriteBoot -or $NoReset) {
	$verifyAfter = "no_reset"
}
$verifyArgs = @(
	"-m", "esptool", "--chip", "esp32p4", "--port", $Port,
	"--baud", "460800", "--before", "default_reset", "--after", $verifyAfter,
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

# Reset A/B selection only after slot0 has been written and verified.  A power
# loss before this point leaves the previously selected slot bootable; after
# the erase, boot-shim deliberately falls back to the newly verified slot0.
if ($WriteBoot) {
	$eraseAfter = "hard_reset"
	if ($NoReset) {
		$eraseAfter = "no_reset"
	}
	$metadataArgs = @(
		"-m", "esptool", "--chip", "esp32p4", "--port", $Port,
		"--baud", "460800", "--before", "default_reset", "--after", $eraseAfter,
		"erase_region", "0xfc0000", "0x2000"
	)
	& $PythonPath @metadataArgs
	if ($LASTEXITCODE -ne 0) {
		throw "P4 A/B metadata reset failed. Re-run the same release flash command."
	}
}

Write-Host "P4 candidate write and verify passed"
if ($NoReset) {
	Write-Host "Board left in download mode; reset it to boot"
} else {
	Write-Host "Board reset; Linux boots now"
}
if ($unameV) {
	Write-Host "Expect after boot: uname -v = $unameV"
}
if ($stockPath) {
	Write-Host "Recovery command (only after entering ROM mode):"
	Write-Host "  $PythonPath -m esptool --chip esp32p4 --port $Port write_flash 0x0 $stockPath"
} else {
	Write-Warning "No stock readback was supplied; keep the release image available for recovery."
}
