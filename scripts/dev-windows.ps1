$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

$CliPath = Join-Path $Root "build\bin\Release\MeshSimplifierCli.exe"
$ServerExe = Join-Path $Root "build\bin\Release\MeshSimplifierServer.exe"
$BackendPort = if ($env:MESH_SIMPLIFIER_PORT) { $env:MESH_SIMPLIFIER_PORT } else { "8877" }
if (-not (Test-Path -LiteralPath $CliPath) -or -not (Test-Path -LiteralPath $ServerExe)) {
  & ".\scripts\build-windows.ps1"
  if ($LASTEXITCODE -ne 0) {
    throw "Native backend build failed."
  }
}

$env:MESH_SIMPLIFIER_CLI = $CliPath
$env:MESH_SIMPLIFIER_PORT = $BackendPort
Start-Process -FilePath $ServerExe -WorkingDirectory $Root -WindowStyle Normal

Set-Location "$Root\web"
npm install
$env:MESH_SIMPLIFIER_PORT = $BackendPort
npm run dev
