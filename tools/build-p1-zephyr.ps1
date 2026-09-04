[CmdletBinding()]
param(
    [string]$ZephyrSource = 'D:\Users\Developer\easystick-tmp-20260820\p1-zephyr',
    [string]$WestWorkspace = 'D:\Users\Developer\easystick-tmp-20260820\zephyr-p1-work',
    [string]$BuildDirectory = 'D:\Users\Developer\easystick-tmp-20260820\build-p1-zephyr',
    [string]$ContainerImage = 'zephyrprojectrtos/ci@sha256:e3d3643e50dbbbb22aa6e3efd65d9edc8a281d3e113e03c39527a896b1feedc0',
    [switch]$SkipWestUpdate,
    [switch]$ReuseBuildDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$appSource = Join-Path $repoRoot 'projects\easystick-stamp-p4\firmware\zephyr-p1'
$patch = Join-Path $appSource 'patches\0001-p1-transport-ladder.patch'
$overlay = Join-Path $appSource 'boards\esp32p4_function_ev_board_esp32p4_hpcore.overlay'
$expectedZephyr = 'd544481d9ad9c711cefe984c5ea926d71cb56341'
$board = 'esp32p4_function_ev_board/esp32p4/hpcore'

foreach ($path in @($ZephyrSource, $appSource)) {
    if (-not (Test-Path -LiteralPath $path -PathType Container)) {
        throw "Directory not found: $path"
    }
}
if (-not (Test-Path -LiteralPath $patch -PathType Leaf)) {
    throw "P1 transport patch not found: $patch"
}
if (-not (Test-Path -LiteralPath $overlay -PathType Leaf)) {
    throw "P1 overlay not found: $overlay"
}
if (-not (Test-Path -LiteralPath $WestWorkspace -PathType Container)) {
    New-Item -ItemType Directory -Path $WestWorkspace | Out-Null
}
if (-not (Test-Path -LiteralPath $BuildDirectory -PathType Container)) {
    New-Item -ItemType Directory -Path $BuildDirectory | Out-Null
}

$zephyrHead = (git -C $ZephyrSource rev-parse HEAD).Trim()
if ($zephyrHead -ne $expectedZephyr) {
    throw "Unexpected Zephyr revision: $zephyrHead"
}

$reverseCheck = & git -C $ZephyrSource apply --reverse --check $patch 2>$null
if ($LASTEXITCODE -ne 0) {
    & git -C $ZephyrSource apply --check $patch
    if ($LASTEXITCODE -ne 0) {
        throw "P1 transport patch cannot be applied to Zephyr $zephyrHead"
    }
    & git -C $ZephyrSource apply $patch
    if ($LASTEXITCODE -ne 0) {
        throw "P1 transport patch application failed"
    }
}

$buildEntries = @(Get-ChildItem -LiteralPath $BuildDirectory -Force)
if ($buildEntries.Count -gt 0 -and -not $ReuseBuildDirectory) {
    throw "Build directory is not empty; choose a new external directory: $BuildDirectory"
}
$pristine = if ($ReuseBuildDirectory) { 'auto' } else { 'always' }

$dockerArgs = @(
    'run', '--rm',
    '-v', "$WestWorkspace`:/work/ws",
    '-v', "$ZephyrSource`:/work/ws/zephyr",
    '-v', "$appSource`:/work/app:ro",
    '-v', "$BuildDirectory`:/work/build",
    '--env', 'ZEPHYR_BASE=/work/ws/zephyr',
    '--env', 'ZEPHYR_SDK_INSTALL_DIR=/opt/toolchains/zephyr-sdk-1.0.1',
    '--env', 'CMAKE_PREFIX_PATH=/opt/toolchains',
    '--env', 'HOME=/work/ws',
    '--env', 'TMPDIR=/work/build/tmp',
    '--env', 'XDG_CACHE_HOME=/work/build/cache',
    $ContainerImage,
    'bash', '-lc',
    "set -eu; mkdir -p /work/build/tmp /work/build/cache; cd /work/ws; python3 -m pip install --disable-pip-version-check --no-cache-dir -r /work/ws/zephyr/scripts/requirements-base.txt; if [ ! -f .west/config ]; then west init -l /work/ws/zephyr; fi; $(if ($SkipWestUpdate) { ':' } else { 'west update' }); west build --pristine=$pristine -d /work/build -b $board /work/app -- -DDTC_OVERLAY_FILE=/work/app/boards/esp32p4_function_ev_board_esp32p4_hpcore.overlay"
)

& docker @dockerArgs
if ($LASTEXITCODE -ne 0) {
    throw "Zephyr P1 build failed with exit code $LASTEXITCODE"
}

$zephyrBin = Join-Path $BuildDirectory 'zephyr\zephyr.bin'
$zephyrDts = Join-Path $BuildDirectory 'zephyr\zephyr.dts'
$zephyrConfig = Join-Path $BuildDirectory 'zephyr\.config'
foreach ($path in @($zephyrBin, $zephyrDts, $zephyrConfig)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Expected Zephyr build artifact is missing: $path"
    }
}

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zephyrBin).Hash.ToLowerInvariant()
$patchHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $patch).Hash.ToLowerInvariant()
$overlayHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $overlay).Hash.ToLowerInvariant()
Write-Output "ZEPHYR_SOURCE=$zephyrHead"
Write-Output "P1_PATCH_SHA256=$patchHash"
Write-Output "P1_OVERLAY_SHA256=$overlayHash"
Write-Output "ZEPHYR_BIN=$zephyrBin"
Write-Output "ZEPHYR_BIN_SHA256=$hash"
Write-Output "ZEPHYR_DTS=$zephyrDts"
Write-Output "ZEPHYR_CONFIG=$zephyrConfig"
