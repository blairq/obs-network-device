<#
.SYNOPSIS
    Instala el plugin obs-network-device compilado en la carpeta global de plugins de OBS Studio.
.PARAMETER Config
    Configuracion compilada a instalar (RelWithDebInfo, Release, Debug). Por defecto: RelWithDebInfo.
#>
[CmdletBinding()]
param(
    [ValidateSet("RelWithDebInfo", "Release", "Debug")]
    [string]$Config = "RelWithDebInfo"
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildDir = Join-Path $RepoRoot "build_x64"
$PluginName = "obs-network-device"
$DestRoot = Join-Path $env:ProgramData "obs-studio\plugins\$PluginName"

$running = Get-Process -Name "obs64" -ErrorAction SilentlyContinue
if ($running) {
    Write-Warning "OBS Studio (obs64.exe) esta abierto. Si la DLL esta en uso la copia puede fallar. Cerra OBS Studio antes de continuar si da error."
}

$dllCandidates = @(
    (Join-Path $BuildDir "rundir\$Config\$PluginName.dll"),
    (Join-Path $BuildDir "$Config\$PluginName.dll")
)
$dllSource = $dllCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $dllSource) {
    throw "No se encontro $PluginName.dll para configuracion '$Config'. Compila primero con .\scripts\build.ps1 -Config $Config"
}

$pdbSource = [System.IO.Path]::ChangeExtension($dllSource, ".pdb")

$destBin = Join-Path $DestRoot "bin\64bit"
$destData = Join-Path $DestRoot "data"

New-Item -ItemType Directory -Path $destBin -Force | Out-Null
New-Item -ItemType Directory -Path $destData -Force | Out-Null

Copy-Item -Path $dllSource -Destination (Join-Path $destBin "$PluginName.dll") -Force
if (Test-Path $pdbSource) {
    Copy-Item -Path $pdbSource -Destination (Join-Path $destBin "$PluginName.pdb") -Force
}

$dataSource = Join-Path $RepoRoot "data"
if (Test-Path $dataSource) {
    Copy-Item -Path (Join-Path $dataSource "*") -Destination $destData -Recurse -Force
}

Write-Host "Plugin instalado correctamente en: $DestRoot" -ForegroundColor Green
Get-ChildItem $DestRoot -Recurse -File | ForEach-Object {
    Write-Host ("  - " + $_.FullName)
}
