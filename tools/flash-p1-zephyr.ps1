[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = "High")]
param(
    [Parameter(Mandatory = $true)]
    [string]$Port,

    [Parameter(Mandatory = $true)]
    [string]$ZephyrBuildDirectory,

    [Parameter(Mandatory = $true)]
    [string]$StockReadback,

    [Parameter(Mandatory = $true)]
    [string]$Report,

    [string]$PythonPath = "py",

    [Parameter(Mandatory = $true)]
    [string]$ExpectedZephyrSha256,

    [switch]$AllowP1NgWrite
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$whatIfRequested = [bool]$WhatIfPreference
# Read-only cmdlets such as Get-FileHash honor WhatIfPreference. Disable it
# during the preflight, then handle the requested dry run explicitly at the
# write boundary below.
$WhatIfPreference = $false

if (-not $AllowP1NgWrite) {
    throw "Refusing P4 write. Re-run with -AllowP1NgWrite after reviewing the P1-NG condition."
}

$zephyrRoot = (Resolve-Path -LiteralPath $ZephyrBuildDirectory).Path
$stockPath = (Resolve-Path -LiteralPath $StockReadback).Path
$reportPath = [IO.Path]::GetFullPath($Report)
$zephyrBin = Join-Path $zephyrRoot "zephyr\zephyr.bin"

if (-not (Test-Path -LiteralPath $zephyrBin -PathType Leaf)) {
    throw "Zephyr image not found: $zephyrBin"
}
if (-not (Test-Path -LiteralPath $stockPath -PathType Leaf)) {
    throw "P4 stock readback not found: $stockPath"
}
if (Test-Path -LiteralPath $reportPath) {
    throw "Refusing to overwrite flash report: $reportPath"
}
$reportParent = Split-Path -Parent $reportPath
if (-not (Test-Path -LiteralPath $reportParent -PathType Container)) {
    throw "Flash report parent not found: $reportParent"
}

$stockFile = Get-Item -LiteralPath $stockPath
if ($stockFile.Length -ne 0x1000000) {
    throw "P4 stock readback must be exactly 16 MiB: $stockPath"
}
$stockHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $stockPath).Hash.ToLowerInvariant()
$expectedStockSha256 = "229459f251eaf6222f0c07968702d72a3818e520da1788ce465027d969020c24"
if ($stockHash -ne $expectedStockSha256) {
    throw "P4 stock readback SHA-256 mismatch: got $stockHash, expected $expectedStockSha256"
}

$zephyrHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zephyrBin).Hash.ToLowerInvariant()
if ($zephyrHash -ne $ExpectedZephyrSha256.ToLowerInvariant()) {
    throw "Zephyr image SHA-256 mismatch: got $zephyrHash, expected $ExpectedZephyrSha256"
}

$imageInfoArgs = @(
    "-m", "esptool", "--chip", "esp32p4",
    "image_info", $zephyrBin
)
& $PythonPath @imageInfoArgs
if ($LASTEXITCODE -ne 0) {
    throw "Zephyr image_info failed; no P4 write was attempted"
}

$chipArgs = @(
    "-m", "esptool", "--chip", "esp32p4", "--port", $Port,
    "--baud", "921600", "--before", "default_reset", "--after", "no_reset",
    "chip_id"
)
$chipOutput = (& $PythonPath @chipArgs 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw "P4 ROM chip_id failed; no write was attempted"
}

$writeArgs = @(
    "-m", "esptool", "--chip", "esp32p4", "--port", $Port,
    "--baud", "921600", "--before", "default_reset", "--after", "hard_reset",
    "write_flash", "-z", "--flash_mode", "dio", "--flash_freq", "80m",
    "--flash_size", "16MB", "0x2000", $zephyrBin
)
if ($whatIfRequested) {
    Write-Host "WhatIf: no bytes written"
    return
}

& $PythonPath @writeArgs
if ($LASTEXITCODE -ne 0) {
    throw "P4 Zephyr write failed; restore the preserved stock readback before retrying"
}

$verifyArgs = @(
    "-m", "esptool", "--chip", "esp32p4", "--port", $Port,
    "--baud", "921600", "--before", "default_reset", "--after", "no_reset",
    "verify_flash", "0x2000", $zephyrBin
)
& $PythonPath @verifyArgs
if ($LASTEXITCODE -ne 0) {
    throw "P4 Zephyr verify failed; restore the preserved stock readback before retrying"
}

[ordered]@{
    schema = 1
    captured_utc = [DateTime]::UtcNow.ToString("o")
    mode = "P1-NG-negative-control"
    target = "M5Stack Stamp-P4 host with attached Stamp-AddOn C6"
    port = $Port
    chip_probe = $chipOutput.Trim()
    host_image = $zephyrBin
    host_image_sha256 = $zephyrHash
    flash_address = "0x2000"
    flash_mode = "dio"
    flash_frequency = "80m"
    flash_size = "16MB"
    stock_readback = $stockPath
    stock_readback_sha256 = $stockHash
    c6_write_performed = $false
    c6_state = "unchanged existing ESP-Hosted-NG 1.0.6"
    operation = "P4-only Zephyr image write and verify; no C6 write"
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $reportPath -Encoding UTF8 -NoNewline

Write-Host "P4 P1-NG write and verify passed"
Write-Host "C6 write: none"
Write-Host "Flash report: $reportPath"
Write-Host "Recovery image: $stockPath"
