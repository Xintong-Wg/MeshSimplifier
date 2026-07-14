param(
  [string]$InstallDir = $(if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "D:\vcpkg" }),
  [string]$Triplet = "x64-windows-release"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$TripletFile = Join-Path $Root "triplets\$Triplet.cmake"

if (-not (Test-Path -LiteralPath $InstallDir)) {
  git clone --depth 1 https://github.com/microsoft/vcpkg.git $InstallDir
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $InstallDir)) {
    throw "Failed to clone vcpkg into '$InstallDir'. Check GitHub/network access, then rerun this script or set VCPKG_ROOT to an existing vcpkg checkout."
  }
}

Push-Location $InstallDir
try {
  if (-not (Test-Path ".\vcpkg.exe")) {
    .\bootstrap-vcpkg.bat
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path ".\vcpkg.exe")) {
      throw "Failed to bootstrap vcpkg in '$InstallDir'."
    }
  }

  $InstallArgs = @(
    "install",
    "--classic",
    "--triplet", $Triplet,
    "--overlay-triplets", (Split-Path -Parent $TripletFile),
    "opencascade",
    "tbb",
    "glm",
    "spdlog",
    "nlohmann-json",
    "meshoptimizer",
    "draco"
  )

  .\vcpkg.exe @InstallArgs
  if ($LASTEXITCODE -ne 0) {
    throw "vcpkg dependency installation failed. Check the network/proxy output above, then rerun this script."
  }
} finally {
  Pop-Location
}

Write-Host ""
Write-Host "Set this for future shells:"
Write-Host "`$env:VCPKG_ROOT='$InstallDir'"
Write-Host "Build with triplet: $Triplet"
