# Windows x64, PowerShell 5.1+. Run once at build/package time, never at capture time.
# All build products stay under build\; downloads are kept in the specified cache.
[CmdletBinding()]
param(
    [string]$CacheDirectory = $(if ($env:PI_SCRATCH_DIR) { Join-Path $env:PI_SCRATCH_DIR 'ocr-downloads' } else { Join-Path $env:TEMP 'screenshot-translator-ocr-cache' }),
    [switch]$Offline
)
$ErrorActionPreference = 'Stop'
if (-not [Environment]::Is64BitOperatingSystem) { throw 'OCR requires 64-bit Windows 10/11.' }
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$root = $PSScriptRoot
$manifest = Get-Content -Raw -LiteralPath (Join-Path $root 'ocr\runtime-lock.json') | ConvertFrom-Json
$cache = [IO.Path]::GetFullPath($CacheDirectory)
New-Item -ItemType Directory -Force -Path $cache | Out-Null
$pythonZip = Join-Path $cache $manifest.python.name
$expected = $manifest.python.sha256
if (-not (Test-Path -LiteralPath $pythonZip) -or (Get-FileHash -LiteralPath $pythonZip -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    if ($Offline) { throw "Offline Python cache missing or checksum mismatch: $pythonZip" }
    $part = "$pythonZip.part"
    try {
        Write-Host 'Downloading pinned embedded CPython 3.11.9 (x64)...'
        Invoke-WebRequest -UseBasicParsing -Uri $manifest.python.url -OutFile $part
        if ((Get-FileHash -LiteralPath $part -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'Embedded Python SHA256 mismatch.' }
        Move-Item -LiteralPath $part -Destination $pythonZip -Force
    } finally {
        if (Test-Path -LiteralPath $part) { Remove-Item -LiteralPath $part -Force }
    }
}
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$stage = Join-Path $build ('ocr-stage-' + [Guid]::NewGuid().ToString('N'))
$bootstrap = Join-Path $cache ('bootstrap-' + [Guid]::NewGuid().ToString('N'))
$target = Join-Path $build 'ocr'
$backup = Join-Path $build ('ocr-old-' + [Guid]::NewGuid().ToString('N'))
try {
    Expand-Archive -LiteralPath $pythonZip -DestinationPath $bootstrap
    New-Item -ItemType Directory -Path $stage | Out-Null
    Copy-Item -Path (Join-Path $bootstrap '*') -Destination $stage -Recurse
    # Run provisioning from scratch, not stage: stage CRT DLLs must be replaceable.
    $python = Join-Path $bootstrap 'python.exe'
    $arguments = @('-I', '-B', '-X', 'utf8', (Join-Path $root 'ocr\provision.py'), '--stage', $stage, '--cache', $cache)
    if ($Offline) { $arguments += '--offline' }
    # Windows PowerShell turns native stderr into errors under Stop. Preserve
    # the complete diagnostics and rely on the actual child exit status instead.
    $ErrorActionPreference = 'Continue'
    & $python @arguments
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code -ne 0) { throw "OCR provisioning failed (exit $code); existing runtime was not replaced." }
    if (Test-Path -LiteralPath $target) { Move-Item -LiteralPath $target -Destination $backup }
    try {
        Move-Item -LiteralPath $stage -Destination $target
    } catch {
        if (Test-Path -LiteralPath $backup) { Move-Item -LiteralPath $backup -Destination $target }
        throw
    }
    if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Recurse -Force }
    Write-Host "OCR runtime ready: $target"
    Write-Host 'Ship the entire build\ocr directory next to the application. No runtime downloads.'
} finally {
    $ErrorActionPreference = 'Stop'
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    if (Test-Path -LiteralPath $bootstrap) { Remove-Item -LiteralPath $bootstrap -Recurse -Force }
}
