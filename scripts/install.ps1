param(
  [string]$Version = "",
  [string]$InstallPrefix = "",
  [string]$Repo = "",
  [string]$BaseUrl = "",
  [string]$Target = "",
  [switch]$VerifyAttestations,
  [switch]$WithBackendSdk,
  [switch]$Help
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir

function Show-Usage {
  @'
Usage: install.ps1 [-Version VERSION] [-InstallPrefix DIR] [-Repo OWNER/REPO] [-BaseUrl URL_OR_PATH] [-Target TARGET] [-VerifyAttestations] [-WithBackendSdk] [-Help]

Defaults:
  Version       VERSION file when available
  InstallPrefix $HOME\.local
  Repo          RELEASE_REPOSITORY file when available, otherwise billlza/nebula
'@ | Write-Output
}

function Read-DefaultValue([string]$Path, [string]$Fallback) {
  if (Test-Path $Path) {
    return (Get-Content -Path $Path -Raw).Trim()
  }
  return $Fallback
}

function Read-BoolEnv([string]$Name) {
  $raw = [Environment]::GetEnvironmentVariable($Name)
  if ([string]::IsNullOrWhiteSpace($raw)) { return $false }
  switch ($raw.Trim().ToLowerInvariant()) {
    "1" { return $true }
    "true" { return $true }
    "yes" { return $true }
    "on" { return $true }
    "0" { return $false }
    "false" { return $false }
    "no" { return $false }
    "off" { return $false }
    default { throw "invalid boolean value for $Name: $raw" }
  }
}

if ($Help) {
  Show-Usage
  exit 0
}

if (-not $Version) {
  $Version = if ($env:NEBULA_INSTALL_VERSION) { $env:NEBULA_INSTALL_VERSION } else { Read-DefaultValue (Join-Path $RepoRoot "VERSION") "" }
}
if (-not $Repo) {
  $Repo = if ($env:NEBULA_INSTALL_REPOSITORY) { $env:NEBULA_INSTALL_REPOSITORY } else { Read-DefaultValue (Join-Path $RepoRoot "RELEASE_REPOSITORY") "billlza/nebula" }
}
if (-not $InstallPrefix) {
  $InstallPrefix = if ($env:NEBULA_INSTALL_PREFIX) { $env:NEBULA_INSTALL_PREFIX } else { Join-Path $HOME ".local" }
}
if (-not $BaseUrl -and $env:NEBULA_INSTALL_BASE_URL) { $BaseUrl = $env:NEBULA_INSTALL_BASE_URL }
if (-not $Target -and $env:NEBULA_INSTALL_TARGET) { $Target = $env:NEBULA_INSTALL_TARGET }
if (-not $VerifyAttestations.IsPresent -and (Read-BoolEnv "NEBULA_INSTALL_VERIFY_ATTESTATIONS")) {
  $VerifyAttestations = $true
}
if (-not $WithBackendSdk.IsPresent -and (Read-BoolEnv "NEBULA_INSTALL_WITH_BACKEND_SDK")) {
  $WithBackendSdk = $true
}
if (-not $Version) {
  throw "version is required when VERSION is unavailable; pass -Version or set NEBULA_INSTALL_VERSION"
}
if ($WithBackendSdk) {
  throw "-WithBackendSdk is only supported for the linux-x86_64 install target"
}

function Resolve-Target {
  if ($Target) { return $Target }
  if ($env:PROCESSOR_ARCHITECTURE -eq "AMD64") { return "windows-x86_64" }
  throw "unsupported platform: Windows $env:PROCESSOR_ARCHITECTURE"
}

function Resolve-BaseUrl {
  if ($BaseUrl) { return $BaseUrl.TrimEnd('/') }
  return "https://github.com/$Repo/releases/download/v$Version"
}

function Asset-Name([string]$ResolvedTarget) {
  switch ($ResolvedTarget) {
    "windows-x86_64" { return "nebula-v$Version-windows-x86_64.zip" }
    default { throw "unsupported install target: $ResolvedTarget" }
  }
}

function Bundle-Stem([string]$ResolvedTarget) {
  switch ($ResolvedTarget) {
    "windows-x86_64" { return "nebula-v$Version-windows-x86_64" }
    default { throw "unsupported install target: $ResolvedTarget" }
  }
}

function Copy-Or-Download([string]$Source, [string]$Destination) {
  if ($Source.StartsWith("file://")) {
    Copy-Item -Path $Source.Substring(7) -Destination $Destination -Force
    return
  }
  if (Test-Path $Source) {
    Copy-Item -Path $Source -Destination $Destination -Force
    return
  }
  Invoke-WebRequest -Uri $Source -OutFile $Destination
}

function Verify-AttestationPrereqs {
  if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw "gh is required when -VerifyAttestations is enabled"
  }
}

function Run-AttestationVerify([string]$Subject, [string]$Bundle, [string[]]$ExtraArgs = @()) {
  $args = @(
    "attestation", "verify", $Subject,
    "--repo", $Repo,
    "--signer-workflow", "$Repo/.github/workflows/release.yml",
    "--bundle", $Bundle
  ) + $ExtraArgs
  & gh @args
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("nebula-install-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmp | Out-Null

function Remove-PreviousInstall([string]$Prefix) {
  $manifestPath = Join-Path $Prefix "share\nebula\install-manifest.txt"
  if (-not (Test-Path $manifestPath)) { return }
  Get-Content -Path $manifestPath | ForEach-Object {
    $rel = $_.Trim()
    if (-not $rel) { return }
    if ([System.IO.Path]::IsPathRooted($rel)) {
      Write-Warning "skipping unsafe install-manifest entry: $rel"
      return
    }
    foreach ($segment in ($rel -split '[\\/]')) {
      if (-not $segment -or $segment -eq '.' -or $segment -eq '..') {
        Write-Warning "skipping unsafe install-manifest entry: $rel"
        return
      }
    }
    $target = Join-Path $Prefix $rel
    if (Test-Path $target) {
      Remove-Item -Path $target -Force -Recurse -ErrorAction SilentlyContinue
    }
  }
}

function Write-InstallManifest([string]$Prefix, [string]$PayloadDir) {
  $stateDir = Join-Path $Prefix "share\nebula"
  New-Item -ItemType Directory -Path $stateDir -Force | Out-Null
  $manifestPath = Join-Path $stateDir "install-manifest.txt"
  $payloadRoot = [System.IO.Path]::GetFullPath($PayloadDir)
  $files = Get-ChildItem -Path $PayloadDir -Recurse -File | ForEach-Object {
    $full = [System.IO.Path]::GetFullPath($_.FullName)
    $relative = $full.Substring($payloadRoot.Length).TrimStart('\', '/')
    $relative -replace '/', '\'
  } | Sort-Object
  Set-Content -Path $manifestPath -Value $files
}

try {
  $resolvedTarget = Resolve-Target
  $resolvedBaseUrl = Resolve-BaseUrl
  $assetName = Asset-Name $resolvedTarget
  $bundleStem = Bundle-Stem $resolvedTarget
  $checksumsPath = Join-Path $tmp "SHA256SUMS.txt"
  $assetPath = Join-Path $tmp $assetName
  $checksumsBundlePath = Join-Path $tmp "SHA256SUMS.txt.intoto.jsonl"
  $provenanceBundlePath = Join-Path $tmp "$bundleStem.provenance.intoto.jsonl"
  $sbomBundlePath = Join-Path $tmp "$bundleStem.sbom.intoto.jsonl"

  Copy-Or-Download "$resolvedBaseUrl/SHA256SUMS.txt" $checksumsPath
  Copy-Or-Download "$resolvedBaseUrl/$assetName" $assetPath

  $expectedLine = Get-Content -Path $checksumsPath | Where-Object { $_ -match [regex]::Escape($assetName) } | Select-Object -First 1
  if (-not $expectedLine) {
    throw "missing checksum entry for $assetName"
  }
  $expectedSha = ($expectedLine -split "\s+")[0].Trim()
  $actualSha = (Get-FileHash -Path $assetPath -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($expectedSha.ToLowerInvariant() -ne $actualSha) {
    throw "checksum mismatch for $assetName`nexpected: $expectedSha`nactual:   $actualSha"
  }

  if ($VerifyAttestations) {
    Verify-AttestationPrereqs
    Copy-Or-Download "$resolvedBaseUrl/SHA256SUMS.txt.intoto.jsonl" $checksumsBundlePath
    Copy-Or-Download "$resolvedBaseUrl/$bundleStem.provenance.intoto.jsonl" $provenanceBundlePath
    Copy-Or-Download "$resolvedBaseUrl/$bundleStem.sbom.intoto.jsonl" $sbomBundlePath

    Run-AttestationVerify $checksumsPath $checksumsBundlePath
    Run-AttestationVerify $assetPath $provenanceBundlePath
    Run-AttestationVerify $assetPath $sbomBundlePath @("--predicate-type", "https://spdx.dev/Document/v2.3")
  }

  $extractRoot = Join-Path $tmp "extract"
  Expand-Archive -Path $assetPath -DestinationPath $extractRoot -Force
  $payloadDir = Join-Path $extractRoot "nebula-v$Version-windows-x86_64"
  $binarySource = Join-Path $payloadDir "bin\nebula.exe"
  $runtimeHeader = Join-Path $payloadDir "include\runtime\nebula_runtime.hpp"
  if (-not (Test-Path $binarySource)) {
    throw "extracted archive missing executable: $binarySource"
  }
  if (-not (Test-Path $runtimeHeader)) {
    throw "extracted archive missing runtime headers: $runtimeHeader"
  }

  New-Item -ItemType Directory -Path $InstallPrefix -Force | Out-Null
  Remove-PreviousInstall $InstallPrefix
  $installBinDir = Join-Path $InstallPrefix "bin"
  New-Item -ItemType Directory -Path $installBinDir -Force | Out-Null
  Copy-Item -Path (Join-Path $payloadDir "bin\*") -Destination $installBinDir -Recurse -Force

  $installIncludeDir = Join-Path $InstallPrefix "include"
  if (Test-Path (Join-Path $payloadDir "include")) {
    New-Item -ItemType Directory -Path $installIncludeDir -Force | Out-Null
    Copy-Item -Path (Join-Path $payloadDir "include\*") -Destination $installIncludeDir -Recurse -Force
  }

  $installShareDir = Join-Path $InstallPrefix "share"
  if (Test-Path (Join-Path $payloadDir "share")) {
    New-Item -ItemType Directory -Path $installShareDir -Force | Out-Null
    Copy-Item -Path (Join-Path $payloadDir "share\*") -Destination $installShareDir -Recurse -Force
  }
  Write-InstallManifest $InstallPrefix $payloadDir

  $binaryDest = Join-Path $installBinDir "nebula.exe"

  Write-Output "Installed Nebula to $binaryDest"
  & $binaryDest --version
  Write-Output "Verified archive integrity against SHA256SUMS.txt before installation."
  if ($VerifyAttestations) {
    Write-Output "Verified checksum, provenance, and SBOM attestations with gh before installation."
  }
  Write-Output "Nebula build/run/test/bench uses a host C++23 compiler. Default contract: clang++ when CXX is unset."
  Write-Output "Set CXX=/path/to/clang++ if you need to override the host compiler explicitly."
  Write-Output "Hosted registry helpers ship under $InstallPrefix\\share\\nebula\\registry; --registry-url workflows require Python 3.11+ on PATH, or set PYTHON to a compatible interpreter."
  Write-Output "Git-backed dependencies require git on PATH before nebula add --git, fetch, or update."
  Write-Output "Nebula backend SDK is not installed by default on Windows."
  Write-Output "For upgrade/rollback/uninstall and stronger provenance/SBOM verification, see $InstallPrefix\\share\\doc\\nebula\\install_lifecycle.md and $InstallPrefix\\share\\doc\\nebula\\release_verification.md."

  $pathEntries = ($env:PATH -split ';') | Where-Object { $_ }
  if ($pathEntries -notcontains $installBinDir) {
    Write-Output "Add $installBinDir to PATH if it is not already available in your shell profile."
  }
} finally {
  Remove-Item -Path $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
