[CmdletBinding()]
param(
    [string]$C6Source = 'D:\Users\Developer\easystick-tmp-20260820\p1-esp-hosted-mcu',
    [string]$IdfSource = '',
    [string]$BuildDirectory = 'D:\Users\Developer\easystick-tmp-20260820\build-p1-c6'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
if ([string]::IsNullOrWhiteSpace($IdfSource)) {
    $IdfSource = Join-Path $repoRoot 'projects\easystick-stamp-p4\firmware\vendor\esp-idf'
}

$expectedC6 = '3f0d1076749afdb589f00c075d8dce895e3dd32d'
$expectedIdf = '2c211b236707889e8400c4dc5644dd5c4ee071e0'
$expectedProtobuf = 'abc67a11c6db271bedbb9f58be85d6f4e2ea8389'
$image = 'espressif/idf:v5.5.3'
$defaults = @(
    '/work/c6/slave/sdkconfig.defaults.esp32c6',
    '/work/c6/slave/sdkconfig.ci.sdio',
    '/work/repo/projects/easystick-stamp-p4/firmware/zephyr-p1/c6/sdkconfig.p1.defaults'
) -join ';'

foreach ($path in @($C6Source, $IdfSource)) {
    if (-not (Test-Path -LiteralPath $path -PathType Container)) {
        throw "Source directory not found: $path"
    }
}
if (-not (Test-Path -LiteralPath $BuildDirectory -PathType Container)) {
    New-Item -ItemType Directory -Path $BuildDirectory | Out-Null
}

$c6Head = (git -C $C6Source rev-parse HEAD).Trim()
$idfHead = (git -C $IdfSource rev-parse HEAD).Trim()
$protobufPath = Join-Path $C6Source 'common\protobuf-c'
$protobufHead = (git -C $protobufPath rev-parse HEAD).Trim()
if ($c6Head -ne $expectedC6) {
    throw "Unexpected ESP-Hosted-MCU revision: $c6Head"
}
if ($idfHead -ne $expectedIdf) {
    throw "Unexpected ESP-IDF revision: $idfHead"
}
if ($protobufHead -ne $expectedProtobuf) {
    throw "Unexpected protobuf-c revision: $protobufHead"
}

$buildEntries = @(Get-ChildItem -LiteralPath $BuildDirectory -Force)
if ($buildEntries.Count -gt 0) {
    throw "Build directory is not empty; choose a new external directory: $BuildDirectory"
}

$idfImageHead = (& docker run --rm --entrypoint /bin/bash $image -lc `
    'git -C /opt/esp/idf rev-parse HEAD' | Out-String).Trim()
if ($LASTEXITCODE -ne 0) {
    throw "Could not inspect the ESP-IDF revision inside $image"
}
if ($idfImageHead -ne $expectedIdf) {
    throw "ESP-IDF container revision $idfImageHead != $expectedIdf"
}

$dockerArgs = @(
    'run', '--rm',
    '-v', "$C6Source`:/work/c6",
    '-v', "$repoRoot`:/work/repo:ro",
    '-v', "$BuildDirectory`:/work/build",
    '--entrypoint', '/bin/bash',
    '--env', "IDF_PATH=/opt/esp/idf",
    '--env', "SDKCONFIG_DEFAULTS=$defaults",
    $image,
    '-lc',
    'set -eu; source /opt/esp/idf/export.sh; cd /work/c6/slave; python "$IDF_PATH/tools/idf.py" -B /work/build set-target esp32c6 && python "$IDF_PATH/tools/idf.py" -B /work/build reconfigure && python "$IDF_PATH/tools/idf.py" -B /work/build build'
)

& docker @dockerArgs
if ($LASTEXITCODE -ne 0) {
    throw "ESP-Hosted-MCU C6 build failed with exit code $LASTEXITCODE"
}

$networkAdapter = Join-Path $BuildDirectory 'network_adapter.bin'
if (-not (Test-Path -LiteralPath $networkAdapter -PathType Leaf)) {
    throw "Expected C6 application image was not produced: $networkAdapter"
}

$infoArgs = @(
    'run', '--rm',
    '-v', "$BuildDirectory`:/work/build:ro",
    $image,
    'python', '-m', 'esptool',
    '--chip', 'esp32c6',
    'image_info', '/work/build/network_adapter.bin'
)
& docker @infoArgs
if ($LASTEXITCODE -ne 0) {
    throw "esptool image_info failed with exit code $LASTEXITCODE"
}

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $networkAdapter).Hash.ToLowerInvariant()
Write-Output "C6_SOURCE=$c6Head"
Write-Output "ESP_IDF=$idfHead"
Write-Output "ESP_IDF_CONTAINER=$idfImageHead"
Write-Output "PROTOBUF_C=$protobufHead"
Write-Output "NETWORK_ADAPTER=$networkAdapter"
Write-Output "NETWORK_ADAPTER_SHA256=$hash"
