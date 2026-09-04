[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$Port,

  [Parameter(Mandatory = $true)]
  [string]$OutputDirectory,

  [string]$Python = "python",
  [int]$FlashBytes = 0x1000000,
  [int]$ChunkBytes = 0x20000
)

$ErrorActionPreference = "Stop"

if ($FlashBytes -le 0 -or $ChunkBytes -le 0) {
  throw "FlashBytes and ChunkBytes must be positive"
}
if (($FlashBytes % 0x1000) -ne 0 -or ($ChunkBytes % 0x1000) -ne 0) {
  throw "FlashBytes and ChunkBytes must be 4 KiB aligned"
}
if (($FlashBytes % $ChunkBytes) -ne 0) {
  throw "FlashBytes must be an exact multiple of ChunkBytes"
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")).Path
$destination = [IO.Path]::GetFullPath($OutputDirectory)
$repoPrefix = $repoRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if ($destination -eq $repoRoot -or $destination.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
  throw "Refusing to write a stock backup inside the repository: $destination"
}

New-Item -ItemType Directory -Path $destination -Force | Out-Null
$manifestPath = Join-Path $destination "manifest.json"
if (Test-Path -LiteralPath $manifestPath) {
  throw "Refusing to overwrite an existing backup manifest: $manifestPath"
}

$chunks = @()
$offset = 0
while ($offset -lt $FlashBytes) {
  $attemptSize = [Math]::Min($ChunkBytes, $FlashBytes - $offset)
  $completed = $false
  while ($attemptSize -ge 0x1000) {
    $name = "flash-{0:X8}-{1:X8}.bin" -f $offset, $attemptSize
    $path = Join-Path $destination $name
    if (Test-Path -LiteralPath $path) {
      throw "Refusing to overwrite an existing chunk: $path"
    }

    Write-Host ("Reading 0x{0:X}..0x{1:X} from {2} (chunk 0x{3:X})" -f $offset, ($offset + $attemptSize), $Port, $attemptSize)
    & $Python -m esptool --chip esp32p4 --port $Port --before default_reset --after no_reset `
      read_flash --no-progress ("0x{0:X}" -f $offset) ("0x{0:X}" -f $attemptSize) $path
    if ($LASTEXITCODE -eq 0) {
      $item = Get-Item -LiteralPath $path
      if ($item.Length -ne $attemptSize) {
        throw "short read for ${path}: expected $attemptSize bytes, got $($item.Length)"
      }
      $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $path
      $chunks += [ordered]@{
        offset = $offset
        size = $attemptSize
        file = $name
        sha256 = $hash.Hash.ToLowerInvariant()
      }
      $offset += $attemptSize
      $completed = $true
      break
    }
    if (Test-Path -LiteralPath $path) {
      throw "esptool failed after creating a partial file; preserve it and restart in a new directory: $path"
    }
    $nextSize = [int]([Math]::Floor($attemptSize / 2))
    $nextSize = $nextSize - ($nextSize % 0x1000)
    if ($nextSize -lt 0x1000) {
      $nextSize = 0x1000
    }
    Write-Warning ("Read failed at 0x{0:X} with 0x{1:X}; retrying with 0x{2:X}" -f $offset, $attemptSize, $nextSize)
    $attemptSize = $nextSize
  }
  if (-not $completed) {
    throw "esptool could not read flash at offset 0x{0:X} even at 4 KiB" -f $offset
  }
}

[ordered]@{
  schema = 1
  captured_utc = [DateTime]::UtcNow.ToString("o")
  port = $Port
  flash_bytes = $FlashBytes
  chunk_bytes = $ChunkBytes
  operation = "read-only esptool read-flash; no erase or write"
  chunks = $chunks
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8 -NoNewline

Write-Host "Read-only backup complete: $destination"
Write-Host "Manifest: $manifestPath"
