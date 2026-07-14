param(
  [string]$Configuration = "Release",
  [string]$Triplet = "x64-windows-release",
  [string]$PackageName = "MeshSimplifier-Windows-x64-$(Get-Date -Format 'yyyyMMdd-HHmm')",
  [string]$OutputRoot = "",
  [string]$NodeExe = "",
  [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $OutputRoot) {
  $OutputRoot = Join-Path $Root "release"
}

$OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
$PackageDir = [System.IO.Path]::GetFullPath((Join-Path $OutputRoot $PackageName))
$OutputPrefix = $OutputRoot.TrimEnd('\') + '\'
if (-not $PackageDir.StartsWith($OutputPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
  throw "Package directory must be a child of the output directory."
}

if (-not $SkipBuild) {
  & (Join-Path $PSScriptRoot "build-windows.ps1") -Preset $Configuration -Triplet $Triplet
  if ($LASTEXITCODE -ne 0) {
    throw "Native build failed."
  }

  Push-Location (Join-Path $Root "web")
  try {
    npm run typecheck
    if ($LASTEXITCODE -ne 0) {
      throw "Frontend typecheck failed."
    }
    npm run build
    if ($LASTEXITCODE -ne 0) {
      throw "Frontend build failed."
    }
  }
  finally {
    Pop-Location
  }
}

$ReleaseBin = Join-Path $Root "build\bin\$Configuration"
if (-not (Test-Path -LiteralPath $ReleaseBin)) {
  $ReleaseBin = Join-Path $Root "build\bin"
}

$RequiredPaths = @(
  (Join-Path $ReleaseBin "MeshSimplifierServer.exe"),
  (Join-Path $ReleaseBin "MeshSimplifierCli.exe"),
  (Join-Path $Root "backend\server\src\server.js"),
  (Join-Path $Root "backend\server\src\server-config.js"),
  (Join-Path $Root "backend\server\package.json"),
  (Join-Path $Root "web\dist\index.html")
)
foreach ($RequiredPath in $RequiredPaths) {
  if (-not (Test-Path -LiteralPath $RequiredPath)) {
    throw "Required package input is missing: $RequiredPath"
  }
}

if (-not $NodeExe) {
  $NodeCommand = Get-Command node -ErrorAction Stop
  $NodeExe = $NodeCommand.Source
}
$NodeExe = [System.IO.Path]::GetFullPath($NodeExe)
if (-not (Test-Path -LiteralPath $NodeExe -PathType Leaf)) {
  throw "Node runtime was not found: $NodeExe"
}
$NodeArch = & $NodeExe -p "process.arch"
if ($LASTEXITCODE -ne 0 -or $NodeArch.Trim() -ne "x64") {
  throw "The packaged Node runtime must be Windows x64."
}

New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
if (Test-Path -LiteralPath $PackageDir) {
  Remove-Item -LiteralPath $PackageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $PackageDir -Force | Out-Null

Copy-Item -Path (Join-Path $ReleaseBin "*") -Destination $PackageDir -Recurse -Force
Copy-Item -LiteralPath $NodeExe -Destination (Join-Path $PackageDir "node.exe") -Force

$ServerTarget = Join-Path $PackageDir "backend\server"
New-Item -ItemType Directory -Path (Join-Path $ServerTarget "src") -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $Root "backend\server\package.json") -Destination $ServerTarget -Force
Copy-Item -LiteralPath (Join-Path $Root "backend\server\src\server.js") -Destination (Join-Path $ServerTarget "src") -Force
Copy-Item -LiteralPath (Join-Path $Root "backend\server\src\server-config.js") -Destination (Join-Path $ServerTarget "src") -Force

$WebTarget = Join-Path $PackageDir "web\dist"
New-Item -ItemType Directory -Path $WebTarget -Force | Out-Null
Copy-Item -Path (Join-Path $Root "web\dist\*") -Destination $WebTarget -Recurse -Force

foreach ($DataFolder in @("imports", "outputs", "inspections", "cache", "tmp", "logs")) {
  New-Item -ItemType Directory -Path (Join-Path $PackageDir "data\$DataFolder") -Force | Out-Null
}

$VcRuntimeNames = @(
  "msvcp140.dll",
  "vcruntime140.dll",
  "vcruntime140_1.dll",
  "concrt140.dll"
)
foreach ($RuntimeName in $VcRuntimeNames) {
  $RuntimePath = Join-Path $env:WINDIR "System32\$RuntimeName"
  if (-not (Test-Path -LiteralPath $RuntimePath)) {
    throw "Required MSVC runtime is missing: $RuntimePath"
  }
  Copy-Item -LiteralPath $RuntimePath -Destination $PackageDir -Force
}

$NodeLicense = Join-Path (Split-Path -Parent $NodeExe) "LICENSE"
if (Test-Path -LiteralPath $NodeLicense) {
  Copy-Item -LiteralPath $NodeLicense -Destination (Join-Path $PackageDir "NODE-LICENSE.txt") -Force
}

$Launcher = @'
@echo off
cd /d "%~dp0"
MeshSimplifierServer.exe
set EXIT_CODE=%ERRORLEVEL%
if not "%EXIT_CODE%"=="0" (
  echo.
  echo Mesh Simplifier exited with code %EXIT_CODE%.
  pause
)
exit /b %EXIT_CODE%
'@
Set-Content -LiteralPath (Join-Path $PackageDir "Start-MeshSimplifier.cmd") -Value $Launcher -Encoding Ascii

$Readme = @"
Mesh Simplifier - Portable Windows x64 Package

Requirements:
- Windows 10 or Windows 11, 64-bit
- A browser with WebGL support
- No Node.js, CMake, Visual Studio, vcpkg, or other development environment is required

Start:
1. Extract the complete folder.
2. Double-click Start-MeshSimplifier.cmd or MeshSimplifierServer.exe.
3. The console window hosts the backend and opens http://127.0.0.1:8877/.
4. Close the console window to stop the service.

Keep every EXE, DLL, node.exe, backend, web, and data item together.
Imported CAD files and generated outputs are stored under the data directory.

Package: $PackageName
Built: $(Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz")
Node: $(& $NodeExe --version)
"@
Set-Content -LiteralPath (Join-Path $PackageDir "README.txt") -Value $Readme -Encoding UTF8

$ManifestFiles = Get-ChildItem -LiteralPath $PackageDir -Recurse -File | Sort-Object FullName
$Manifest = [ordered]@{
  package = $PackageName
  platform = "windows-x64"
  builtAt = (Get-Date).ToString("o")
  nodeVersion = (& $NodeExe --version)
  files = @(
    foreach ($File in $ManifestFiles) {
      [ordered]@{
        path = [System.IO.Path]::GetRelativePath($PackageDir, $File.FullName).Replace('\', '/')
        size = $File.Length
        sha256 = (Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
      }
    }
  )
}
$Manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $PackageDir "manifest.json") -Encoding UTF8

$ZipPath = "$PackageDir.zip"
if (Test-Path -LiteralPath $ZipPath) {
  Remove-Item -LiteralPath $ZipPath -Force
}
Compress-Archive -LiteralPath $PackageDir -DestinationPath $ZipPath -CompressionLevel Optimal

$PackageSize = (Get-ChildItem -LiteralPath $PackageDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
$ZipSize = (Get-Item -LiteralPath $ZipPath).Length
Write-Host ""
Write-Host "Portable package directory: $PackageDir"
Write-Host "Portable package ZIP:       $ZipPath"
Write-Host ("Package size:               {0:N1} MB" -f ($PackageSize / 1MB))
Write-Host ("ZIP size:                   {0:N1} MB" -f ($ZipSize / 1MB))
