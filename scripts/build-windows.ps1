param(
  [string]$Preset = "Release",
  [string]$Triplet = "x64-windows-release"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

$BuildDir = Join-Path $Root "build"
$CMakeArgs = @("-S", ".", "-B", $BuildDir, "-DCMAKE_BUILD_TYPE=$Preset")

$CandidateVcpkgRoots = @(
  $env:VCPKG_ROOT,
  "D:\vcpkg",
  "D:\tools\vcpkg",
  "$env:USERPROFILE\vcpkg",
  "C:\vcpkg",
  "C:\tools\vcpkg"
) | Where-Object { $_ -and $_.Trim() }

$Toolchain = $null
foreach ($Candidate in $CandidateVcpkgRoots) {
  $CandidateToolchain = Join-Path $Candidate "scripts\buildsystems\vcpkg.cmake"
  if (Test-Path -LiteralPath $CandidateToolchain) {
    $Toolchain = $CandidateToolchain
    break
  }
}

if (-not $Toolchain) {
  throw "vcpkg toolchain not found. Run .\scripts\bootstrap-vcpkg.ps1 first, or set VCPKG_ROOT to your vcpkg directory."
}

$CMakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$Toolchain"
$CMakeArgs += "-DVCPKG_TARGET_TRIPLET=$Triplet"
$CMakeArgs += "-DVCPKG_MANIFEST_MODE=OFF"

$TripletDir = Join-Path $Root "triplets"
if (Test-Path -LiteralPath (Join-Path $TripletDir "$Triplet.cmake")) {
  $CMakeArgs += "-DVCPKG_OVERLAY_TRIPLETS=$TripletDir"
}

cmake @CMakeArgs
if ($LASTEXITCODE -ne 0) {
  throw "CMake configure failed. Check dependency/toolchain messages above."
}

cmake --build $BuildDir --config $Preset --parallel
if ($LASTEXITCODE -ne 0) {
  throw "CMake build failed. Check compiler output above."
}

Write-Host ""
$CliExe = Join-Path $BuildDir "bin\$Preset\MeshSimplifierCli.exe"
if (-not (Test-Path -LiteralPath $CliExe)) {
  $CliExe = Join-Path $BuildDir "bin\MeshSimplifierCli.exe"
}
Write-Host "Native CLI built at: $CliExe"

$ServerExe = Join-Path $BuildDir "bin\$Preset\MeshSimplifierServer.exe"
if (-not (Test-Path -LiteralPath $ServerExe)) {
  $ServerExe = Join-Path $BuildDir "bin\MeshSimplifierServer.exe"
}
Write-Host "Backend launcher built at: $ServerExe"
