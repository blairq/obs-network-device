<#
.SYNOPSIS
    Empaqueta el plugin obs-network-device en un archivo ZIP listo para instalar en OBS Studio.
.PARAMETER Config
    Configuracion compilada a empaquetar (RelWithDebInfo, Release, Debug). Por defecto: RelWithDebInfo.
.PARAMETER BuildDir
    Directorio de compilacion opcional (si se omite, busca automaticamente en build_ninja_x64 y build_x64).
.PARAMETER OutDir
    Directorio de salida del paquete ZIP y SHA256SUMS.txt. Por defecto: dist/ en la raiz del repositorio.
.PARAMETER IncludePdb
    Si se especifica, incluye obs-network-device.pdb junto a la DLL dentro del ZIP.
#>
[CmdletBinding()]
param(
    [ValidateSet("RelWithDebInfo", "Release", "Debug")]
    [string]$Config = "RelWithDebInfo",
    [string]$BuildDir = "",
    [string]$OutDir = "",
    [switch]$IncludePdb
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BuildspecPath = Join-Path $RepoRoot "buildspec.json"
if (-not (Test-Path $BuildspecPath)) {
    throw "No se encontro buildspec.json en $RepoRoot"
}

$buildspec = Get-Content $BuildspecPath -Raw | ConvertFrom-Json
$PluginName = if ($buildspec.name) { [string]$buildspec.name } else { "obs-network-device" }
$Version = if ($buildspec.version) { [string]$buildspec.version } else { "1.0.0" }

if ($OutDir -eq "") {
    $OutDir = Join-Path $RepoRoot "dist"
}

$searchDirs = if ($BuildDir -ne "") {
    if ([System.IO.Path]::IsPathRooted($BuildDir)) { @($BuildDir) } else { @(Join-Path $RepoRoot $BuildDir) }
} else {
    @(
        (Join-Path $RepoRoot "build_ninja_x64"),
        (Join-Path $RepoRoot "build_x64")
    )
}

$dllCandidates = @()
foreach ($dir in $searchDirs) {
    $dllCandidates += @(
        (Join-Path $dir "rundir\$Config\$PluginName.dll"),
        (Join-Path $dir "rundir\$PluginName.dll"),
        (Join-Path $dir "$Config\$PluginName.dll"),
        (Join-Path $dir "$PluginName.dll")
    )
}

$dllItem = $dllCandidates |
    Where-Object { Test-Path $_ } |
    ForEach-Object { Get-Item $_ } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if (-not $dllItem) {
    throw "No se encontro $PluginName.dll para configuracion '$Config'. Compila primero con .\scripts\build.ps1 -Config $Config"
}

$dllSource = $dllItem.FullName
$pdbSource = [System.IO.Path]::ChangeExtension($dllSource, ".pdb")

$StageParent = Join-Path $OutDir "stage"
$StageRoot = Join-Path $StageParent $PluginName
$StageBin = Join-Path $StageRoot "bin\64bit"
$StageData = Join-Path $StageRoot "data"

if (Test-Path $StageParent) {
    Remove-Item $StageParent -Recurse -Force
}

New-Item -ItemType Directory -Path $StageBin -Force | Out-Null
New-Item -ItemType Directory -Path $StageData -Force | Out-Null

Copy-Item -Path $dllSource -Destination (Join-Path $StageBin "$PluginName.dll") -Force
if ($IncludePdb -and (Test-Path $pdbSource)) {
    Copy-Item -Path $pdbSource -Destination (Join-Path $StageBin "$PluginName.pdb") -Force
}

$dataSource = Join-Path $RepoRoot "data"
if (Test-Path $dataSource) {
    Copy-Item -Path (Join-Path $dataSource "*") -Destination $StageData -Recurse -Force
}

$licensePath = Join-Path $RepoRoot "LICENSE"
if (Test-Path $licensePath) {
    Copy-Item -Path $licensePath -Destination (Join-Path $StageRoot "LICENSE") -Force
}

$installTxt = @"
OBS Network Device v$Version - Instalacion en Windows (x64)
=========================================================

[ES] Instalacion manual en OBS Studio (32.0+ / 33.0+):
1. Cerra OBS Studio si esta abierto.
2. Descomprime este archivo ZIP de forma que la carpeta '$PluginName' quede en:
     C:\ProgramData\obs-studio\plugins\$PluginName
   (Dentro deben quedar bin\64bit\$PluginName.dll y data\locale\*.ini)
3. Abri OBS Studio:
   - Agregar camara: Fuentes -> '+' -> 'Dispositivo de red'
     (o Herramientas -> 'Explorar dispositivos de red...')
   - Control PTZ: clic derecho sobre la fuente ONVIF -> 'Ver control PTZ'
     o Paneles -> 'Control PTZ (Red)' / Herramientas -> 'Control PTZ (Red)'.

[EN] Manual install in OBS Studio (32.0+ / 33.0+):
1. Close OBS Studio if running.
2. Extract this ZIP so the '$PluginName' folder sits at:
     C:\ProgramData\obs-studio\plugins\$PluginName
   (Containing bin\64bit\$PluginName.dll and data\locale\*.ini)
3. Launch OBS Studio and add a 'Network Device' source.
"@
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText((Join-Path $StageRoot "INSTALL.txt"), $installTxt, $utf8NoBom)

$ZipName = "$PluginName-v$Version-windows-x64.zip"
$ZipPath = Join-Path $OutDir $ZipName
if (Test-Path $ZipPath) {
    Remove-Item $ZipPath -Force
}

Compress-Archive -Path $StageRoot -DestinationPath $ZipPath -Force

$zipHash = (Get-FileHash -Path $ZipPath -Algorithm SHA256).Hash.ToLowerInvariant()
$sumsPath = Join-Path $OutDir "SHA256SUMS.txt"
[System.IO.File]::WriteAllText($sumsPath, "$zipHash  $ZipName`n", $utf8NoBom)

Remove-Item $StageParent -Recurse -Force

Write-Host "Release ZIP generado en: $ZipPath" -ForegroundColor Green
Write-Host ("SHA256: " + $zipHash) -ForegroundColor Cyan
Write-Host ("Checksums: " + $sumsPath) -ForegroundColor DarkGray
