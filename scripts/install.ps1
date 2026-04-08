param(
  [string]$Version = "",
  [string]$InstallPrefix = "",
  [string]$Repo = "",
  [string]$BaseUrl = "",
  [string]$Target = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir

function Read-DefaultValue([string]$Path, [string]$Fallback) {
  if (Test-Path $Path) {
    return (Get-Content -Path $Path -Raw).Trim()
  }
  return $Fallback
}

if (-not $Version) {
  $Version = if ($env:NEBULA_INSTALL_VERSION) { $env:NEBULA_INSTALL_VERSION } else { Read-DefaultValue (Join-Path $RepoRoot "VERSION") "1.0.0" }
}
if (-not $Repo) {
  $Repo = if ($env:NEBULA_INSTALL_REPOSITORY) { $env:NEBULA_INSTALL_REPOSITORY } else { Read-DefaultValue (Join-Path $RepoRoot "RELEASE_REPOSITORY") "billlza/nebula" }
}
if (-not $InstallPrefix) {
  $InstallPrefix = if ($env:NEBULA_INSTALL_PREFIX) { $env:NEBULA_INSTALL_PREFIX } else { Join-Path $HOME ".local" }
}
if (-not $BaseUrl -and $env:NEBULA_INSTALL_BASE_URL) { $BaseUrl = $env:NEBULA_INSTALL_BASE_URL }
if (-not $Target -and $env:NEBULA_INSTALL_TARGET) { $Target = $env:NEBULA_INSTALL_TARGET }

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

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("nebula-install-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmp | Out-Null

try {
  $resolvedTarget = Resolve-Target
  $resolvedBaseUrl = Resolve-BaseUrl
  $assetName = Asset-Name $resolvedTarget
  $checksumsPath = Join-Path $tmp "SHA256SUMS.txt"
  $assetPath = Join-Path $tmp $assetName

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

  $extractRoot = Join-Path $tmp "extract"
  Expand-Archive -Path $assetPath -DestinationPath $extractRoot -Force
  $payloadDir = Join-Path $extractRoot "nebula-v$Version-windows-x86_64"
  $binarySource = Join-Path $payloadDir "bin\nebula.exe"
  if (-not (Test-Path $binarySource)) {
    throw "extracted archive missing executable: $binarySource"
  }

  $installBinDir = Join-Path $InstallPrefix "bin"
  New-Item -ItemType Directory -Path $installBinDir -Force | Out-Null
  $binaryDest = Join-Path $installBinDir "nebula.exe"
  Copy-Item -Path $binarySource -Destination $binaryDest -Force

  Write-Host "Installed Nebula to $binaryDest"
  & $binaryDest --version

  $pathEntries = ($env:PATH -split ';') | Where-Object { $_ }
  if ($pathEntries -notcontains $installBinDir) {
    Write-Host "Add $installBinDir to PATH if it is not already available in your shell profile."
  }
} finally {
  Remove-Item -Path $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
