[CmdletBinding()]
param(
    [string]$ZephyrSource = 'D:\Users\Developer\easystick-tmp-20260820\p1-zephyr',
    [string]$C6Source = 'D:\Users\Developer\easystick-tmp-20260820\p1-esp-hosted-mcu',
    [string]$IdfSource = '',
    [string]$ZephyrBuildDirectory = 'D:\Users\Developer\easystick-tmp-20260820\build-p1-zephyr',
    [string]$C6BuildDirectory = 'D:\Users\Developer\easystick-tmp-20260820\build-p1-c6'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
if ([string]::IsNullOrWhiteSpace($IdfSource)) {
    $IdfSource = Join-Path $repoRoot 'projects\easystick-stamp-p4\firmware\vendor\esp-idf'
}

$appSource = Join-Path $repoRoot 'projects\easystick-stamp-p4\firmware\zephyr-p1'
$protobufSource = Join-Path $C6Source 'common\protobuf-c'
$overlay = Join-Path $appSource 'boards\esp32p4_function_ev_board_esp32p4_hpcore.overlay'
$zephyrDts = Join-Path $ZephyrBuildDirectory 'zephyr\zephyr.dts'
$zephyrConfig = Join-Path $ZephyrBuildDirectory 'zephyr\.config'
$zephyrBin = Join-Path $ZephyrBuildDirectory 'zephyr\zephyr.bin'
$c6Bin = Join-Path $C6BuildDirectory 'network_adapter.bin'
$verifier = Join-Path $PSScriptRoot 'verify-p1-build.py'

foreach ($path in @(
        $ZephyrSource, $C6Source, $IdfSource, $protobufSource,
        $overlay, $zephyrDts, $zephyrConfig, $zephyrBin, $c6Bin, $verifier
    )) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "P1 verification input not found: $path"
    }
}

& py -3 $verifier `
    --zephyr-source $ZephyrSource `
    --c6-source $C6Source `
    --idf-source $IdfSource `
    --protobuf-source $protobufSource `
    --overlay $overlay `
    --zephyr-dts $zephyrDts `
    --zephyr-config $zephyrConfig `
    --zephyr-bin $zephyrBin `
    --c6-bin $c6Bin
if ($LASTEXITCODE -ne 0) {
    throw "P1 source/build verification failed with exit code $LASTEXITCODE"
}
