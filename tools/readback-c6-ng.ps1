[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Port,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [string]$Python = "py"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ($Port -match '^(?i:COM10)$') {
    throw "Refusing COM10: it is reserved for the P4 host and is not a C6 programmer"
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")).Path
$destination = [IO.Path]::GetFullPath($OutputDirectory)
$repoPrefix = $repoRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if ($destination -eq $repoRoot -or
    $destination.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to write a C6 backup inside the repository: $destination"
}

if (Test-Path -LiteralPath $destination) {
    $entries = @(Get-ChildItem -LiteralPath $destination -Force)
    if ($entries.Count -gt 0) {
        throw "Refusing to overwrite a non-empty C6 backup directory: $destination"
    }
} else {
    New-Item -ItemType Directory -Path $destination | Out-Null
}

$flashIdPath = Join-Path $destination "flash-id.txt"
$flashPath = Join-Path $destination "c6-ng-full-flash.bin"
$manifestPath = Join-Path $destination "manifest.json"

Write-Host "Reading C6 flash identification through $Port"
& $Python -3 -m esptool --chip esp32c6 --port $Port --before default_reset --after no_reset `
    flash_id 2>&1 | Tee-Object -FilePath $flashIdPath
if ($LASTEXITCODE -ne 0) {
    throw "C6 flash_id failed; no flash write was attempted"
}

Write-Host "Reading complete C6 flash through $Port"
& $Python -3 -m esptool --chip esp32c6 --port $Port --before default_reset --after no_reset `
    read_flash --no-progress 0 ALL $flashPath
if ($LASTEXITCODE -ne 0) {
    throw "C6 read_flash failed; preserve any partial file and use a new directory"
}

$file = Get-Item -LiteralPath $flashPath
if ($file.Length -le 0 -or ($file.Length % 0x1000) -ne 0) {
    throw "C6 full-flash backup has an invalid size: $($file.Length)"
}

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $flashPath).Hash.ToLowerInvariant()
[ordered]@{
    schema = 1
    captured_utc = [DateTime]::UtcNow.ToString("o")
    chip = "esp32c6"
    port = $Port
    fixture = "operator-confirmed C6-only UART-boot/ESP-Prog path"
    operation = "read-only esptool flash_id and read_flash; no erase or write"
    flash_id_file = "flash-id.txt"
    full_flash_file = "c6-ng-full-flash.bin"
    full_flash_bytes = $file.Length
    full_flash_sha256 = $hash
    historical_ng_application_sha256 = "2ac39933c84c7688fb5d251ffc01bd15e53b665a272b6053dedca448e40e1827"
    historical_ng_application_sha256_status = "reference only; extract and compare the application partition before C6 write"
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8 -NoNewline

Write-Host "C6 read-only backup complete: $destination"
Write-Host "Full flash SHA256: $hash"
Write-Host "Manifest: $manifestPath"
