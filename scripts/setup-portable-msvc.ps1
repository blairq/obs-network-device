<#
.SYNOPSIS
    Descarga e instala una toolchain MSVC x64 + Windows 11 SDK minima en modo portable (sin permisos de Administrador).
.DESCRIPTION
    Obtiene el manifiesto oficial de Visual Studio 2022, descarga unicamente los paquetes base x64 de MSVC 14.4x
    y los 8 MSI minimos del Windows 11 SDK (10.0.22621), extrae todo en %LOCALAPPDATA%\obs-portable-msvc
    sin modificar el registro ni requerir UAC, y genera env.ps1 para compilar con Ninja + CMake.
#>
[CmdletBinding()]
param(
    [string]$InstallDir = "$env:LOCALAPPDATA\obs-portable-msvc",
    [switch]$KeepCache,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Download-VerifiedFile {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$OutFile,
        [string]$ExpectedSha256 = ""
    )

    $dir = Split-Path -Parent $OutFile
    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }

    if ((Test-Path $OutFile) -and $ExpectedSha256 -ne "") {
        $actual = (Get-FileHash -Path $OutFile -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -eq $ExpectedSha256.ToLowerInvariant()) {
            return
        }
    }

    Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing

    if ($ExpectedSha256 -ne "") {
        $actual = (Get-FileHash -Path $OutFile -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $ExpectedSha256.ToLowerInvariant()) {
            Remove-Item $OutFile -Force -ErrorAction SilentlyContinue
            throw "Hash SHA256 invalido para $OutFile. Esperado: $ExpectedSha256, obtenido: $actual"
        }
    }
}

$envScriptPath = Join-Path $InstallDir "env.ps1"
if ((Test-Path $envScriptPath) -and -not $Force) {
    Write-Host "MSVC portable ya existe en $InstallDir. Verificando..." -ForegroundColor Green
    . $envScriptPath
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        (& cl.exe 2>&1) | Select-Object -First 2 | ForEach-Object { "$_" }
        $ErrorActionPreference = $prevEap
        exit 0
    }
}

$InstallDir = [System.IO.Path]::GetFullPath($InstallDir)
$cacheDir = Join-Path $InstallDir "_cache"
$vsixCache = Join-Path $cacheDir "vsix"
$sdkCache = Join-Path $cacheDir "sdk"
$winSdkRoot = Join-Path $InstallDir "WinSDK"

New-Item -ItemType Directory -Path $InstallDir, $vsixCache, $sdkCache, $winSdkRoot -Force | Out-Null

Write-Host "=== 1/4 Descargando catalogo oficial de Visual Studio 2022 ===" -ForegroundColor Cyan
$channel = Invoke-RestMethod -Uri "https://aka.ms/vs/17/release/channel"
$vsItem = $channel.channelItems | Where-Object { $_.id -eq "Microsoft.VisualStudio.Manifests.VisualStudio" } | Select-Object -First 1
if (-not $vsItem -or -not $vsItem.payloads) {
    throw "No se pudo obtener la URL del manifiesto de Visual Studio."
}

$manifestPath = Join-Path $cacheDir "VisualStudio.vsman"
Download-VerifiedFile -Url $vsItem.payloads[0].url -OutFile $manifestPath
$manifest = Get-Content -Path $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json

# Detectar la ultima version 14.4x disponible en el manifiesto
$vcCandidates = $manifest.packages |
    Where-Object { $_.id -match "^Microsoft\.VC\.(14\.4[0-9]\.17\.[0-9]+)\.Tools\.HostX64\.TargetX64\.base$" } |
    Sort-Object {
        if ($_.id -match "^Microsoft\.VC\.(14\.4[0-9]\.17\.[0-9]+)\.") { [version]$Matches[1] }
        else { [version]"0.0.0.0" }
    } -Descending
$vcLatest = $vcCandidates | Select-Object -First 1
if (-not $vcLatest) {
    throw "No se encontro ningun paquete MSVC 14.4x HostX64.TargetX64 en el manifiesto."
}
$null = $vcLatest.id -match "^Microsoft\.VC\.(14\.4[0-9]\.17\.[0-9]+)\."
$vcPrefix = "Microsoft.VC.$($Matches[1])"
Write-Host "Toolchain MSVC seleccionada: $vcPrefix" -ForegroundColor Green

$msvcPackageIds = @(
    "$vcPrefix.Tools.HostX64.TargetX64.base",
    "$vcPrefix.Tools.HostX64.TargetX64.Res.base",
    "$vcPrefix.CRT.Headers.base",
    "$vcPrefix.CRT.x64.Desktop.base",
    "$vcPrefix.CRT.x64.Store.base"
)

Write-Host "`n=== 2/4 Descargando y extrayendo MSVC x64 minimo ===" -ForegroundColor Cyan
foreach ($pkgId in $msvcPackageIds) {
    $pkg = $manifest.packages |
        Where-Object { $_.id -eq $pkgId -and (-not $_.language -or $_.language -eq "en-US") } |
        Select-Object -First 1
    if (-not $pkg) {
        throw "No se encontro el paquete requerido: $pkgId"
    }

    foreach ($payload in $pkg.payloads) {
        $safeName = [System.IO.Path]::GetFileName($payload.fileName)
        $vsixFile = Join-Path $vsixCache $safeName
        $mb = [Math]::Round($payload.size / 1MB, 1)
        Write-Host "  Descargando $pkgId ($mb MB)..."
        Download-VerifiedFile -Url $payload.url -OutFile $vsixFile -ExpectedSha256 $payload.sha256

        $zip = [System.IO.Compression.ZipFile]::OpenRead($vsixFile)
        try {
            foreach ($entry in $zip.Entries) {
                if ($entry.FullName.EndsWith("/")) { continue }
                if (-not $entry.FullName.StartsWith("Contents/", [System.StringComparison]::OrdinalIgnoreCase)) { continue }
                $relPath = $entry.FullName.Substring(9).Replace('/', '\')
                $destPath = Join-Path $InstallDir $relPath
                $destParent = Split-Path -Parent $destPath
                if (-not (Test-Path $destParent)) {
                    New-Item -ItemType Directory -Path $destParent -Force | Out-Null
                }
                [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destPath, $true)
            }
        } finally {
            $zip.Dispose()
        }
    }
}

Write-Host "`n=== 3/4 Descargando y extrayendo Windows 11 SDK (10.0.22621) minimo x64 ===" -ForegroundColor Cyan
$sdkAlreadyReady = (Test-Path (Join-Path $winSdkRoot "Windows Kits\10\Include\10.0.22621.0\um\windows.h")) -and
                   (Test-Path (Join-Path $winSdkRoot "Windows Kits\10\Lib\10.0.22621.0\um\x64\kernel32.lib")) -and
                   (-not $Force)
if ($sdkAlreadyReady) {
    Write-Host "  Windows 11 SDK ya extraido en $winSdkRoot (omitido)." -ForegroundColor DarkGray
} else {
$sdkPkg = $manifest.packages | Where-Object { $_.id -eq "Win11SDK_10.0.22621" } | Select-Object -First 1
if (-not $sdkPkg) {
    throw "No se encontro el paquete Win11SDK_10.0.22621 en el manifiesto."
}

$wantedMsis = @(
    "Universal CRT Headers Libraries and Sources-x86_en-us.msi",
    "Windows SDK for Windows Store Apps Tools-x86_en-us.msi",
    "Windows SDK for Windows Store Apps Headers-x86_en-us.msi",
    "Windows SDK for Windows Store Apps Libs-x86_en-us.msi",
    "Windows SDK Desktop Headers x86-x86_en-us.msi",
    "Windows SDK Desktop Headers x64-x86_en-us.msi",
    "Windows SDK Desktop Libs x64-x86_en-us.msi",
    "Windows SDK Desktop Tools x64-x86_en-us.msi"
)

# Mapear payloads del SDK por nombre de archivo base para ubicar CABs rapido
$cabLookup = @{}
foreach ($p in $sdkPkg.payloads) {
    $leaf = [System.IO.Path]::GetFileName($p.fileName)
    $cabLookup[$leaf.ToLowerInvariant()] = $p
}

$downloadedMsis = @()
$neededCabs = New-Object System.Collections.Generic.HashSet[string]

foreach ($msiName in $wantedMsis) {
    $msiPayload = $cabLookup[$msiName.ToLowerInvariant()]
    if (-not $msiPayload) {
        throw "No se encontro el MSI requerido del WinSDK: $msiName"
    }
    $msiOut = Join-Path $sdkCache $msiName
    Write-Host "  MSI: $msiName"
    Download-VerifiedFile -Url $msiPayload.url -OutFile $msiOut -ExpectedSha256 $msiPayload.sha256
    $downloadedMsis += $msiOut

    # Leer bytes como Latin1 para detectar exactamente los .cab de 32 hex referenciados por el MSI
    $bytes = [System.IO.File]::ReadAllBytes($msiOut)
    $asciiText = [System.Text.Encoding]::GetEncoding(28591).GetString($bytes)
    $cabMatches = [regex]::Matches($asciiText, "[0-9a-fA-F]{32}\.cab")
    foreach ($m in $cabMatches) {
        [void]$neededCabs.Add($m.Value.ToLowerInvariant())
    }
}

Write-Host "  Descargando $($neededCabs.Count) archivo(s) CAB necesarios..."
foreach ($cabKey in $neededCabs) {
    if (-not $cabLookup.ContainsKey($cabKey)) {
        throw "El CAB '$cabKey' referenciado por el MSI no figura en el paquete SDK."
    }
    $cabPayload = $cabLookup[$cabKey]
    $cabOut = Join-Path $sdkCache ([System.IO.Path]::GetFileName($cabPayload.fileName))
    Download-VerifiedFile -Url $cabPayload.url -OutFile $cabOut -ExpectedSha256 $cabPayload.sha256
}

$targetDirNoSlash = $winSdkRoot.TrimEnd('\')
foreach ($msiFile in $downloadedMsis) {
    $leaf = Split-Path -Leaf $msiFile
    Write-Host "  Extrayendo $leaf (msiexec /a, sin UAC)..."
    $proc = Start-Process -FilePath "msiexec.exe" -ArgumentList @(
        "/a", "`"$msiFile`"",
        "/qn",
        "TARGETDIR=`"$targetDirNoSlash`""
    ) -Wait -PassThru -NoNewWindow
    if ($proc.ExitCode -ne 0) {
        throw "msiexec /a fallo para '$leaf' con codigo $($proc.ExitCode)."
    }
}

# Limpiar arquitecturas no usadas de Lib para ahorrar espacio
$sdkLibDirs = Get-ChildItem (Join-Path $winSdkRoot "Windows Kits\10\Lib") -Directory -ErrorAction SilentlyContinue
foreach ($verDir in $sdkLibDirs) {
    foreach ($sub in @("ucrt", "um")) {
        $subPath = Join-Path $verDir.FullName $sub
        if (Test-Path $subPath) {
            foreach ($archToDrop in @("arm", "arm64", "x86")) {
                $dropPath = Join-Path $subPath $archToDrop
                if (Test-Path $dropPath) {
                    Remove-Item $dropPath -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
        }
    }
}
}

if (-not $KeepCache) {
    Write-Host "Limpiando cache temporal de instalacion..."
    Remove-Item $cacheDir -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "`n=== 4/4 Generando env.ps1 y verificando toolchain ===" -ForegroundColor Cyan
$msvcVerDir = Get-ChildItem (Join-Path $InstallDir "VC\Tools\MSVC") -Directory |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $msvcVerDir) {
    throw "No se encontro VC\Tools\MSVC\<version> en $InstallDir"
}

$sdkVerDir = Get-ChildItem (Join-Path $winSdkRoot "Windows Kits\10\Include") -Directory |
    Where-Object { $_.Name -match "^10\.0\." } |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdkVerDir) {
    throw "No se encontro Windows Kits\10\Include\10.0.* en $winSdkRoot"
}

$msvcVer = $msvcVerDir.Name
$sdkVer = $sdkVerDir.Name

$envContent = @"
# Entorno portable MSVC x64 + Windows 11 SDK (sin instalacion de sistema)
`$PortableRoot = `$PSScriptRoot
`$MsvcRoot = Join-Path `$PortableRoot "VC\Tools\MSVC\$msvcVer"
`$SdkBase = Join-Path `$PortableRoot "WinSDK\Windows Kits\10"
`$SdkVer = "$sdkVer"

`$binPaths = @(
    (Join-Path `$MsvcRoot "bin\Hostx64\x64"),
    (Join-Path `$SdkBase "bin\`$SdkVer\x64"),
    (Join-Path `$SdkBase "bin\x64")
) | Where-Object { Test-Path `$_ }

`$existingPath = `$env:Path -split ';' | Where-Object { `$_ -and `$_.Trim() -ne '' }
`$env:Path = ((@(`$binPaths) + @(`$existingPath)) | Select-Object -Unique) -join ';'

`$includePaths = @(
    (Join-Path `$MsvcRoot "include"),
    (Join-Path `$SdkBase "Include\`$SdkVer\ucrt"),
    (Join-Path `$SdkBase "Include\`$SdkVer\shared"),
    (Join-Path `$SdkBase "Include\`$SdkVer\um"),
    (Join-Path `$SdkBase "Include\`$SdkVer\winrt"),
    (Join-Path `$SdkBase "Include\`$SdkVer\cppwinrt")
) | Where-Object { Test-Path `$_ }
`$env:INCLUDE = `$includePaths -join ';'

`$libPaths = @(
    (Join-Path `$MsvcRoot "lib\x64"),
    (Join-Path `$SdkBase "Lib\`$SdkVer\ucrt\x64"),
    (Join-Path `$SdkBase "Lib\`$SdkVer\um\x64")
) | Where-Object { Test-Path `$_ }
`$env:LIB = `$libPaths -join ';'

`$env:WindowsSdkDir = "`$SdkBase\"
`$env:WindowsSDKVersion = "`$SdkVer\"
`$env:VCToolsVersion = "$msvcVer"
"@

Set-Content -Path $envScriptPath -Value $envContent -Encoding UTF8

. $envScriptPath

$requiredFiles = @(
    (Join-Path $InstallDir "VC\Tools\MSVC\$msvcVer\bin\Hostx64\x64\cl.exe"),
    (Join-Path $InstallDir "VC\Tools\MSVC\$msvcVer\bin\Hostx64\x64\link.exe"),
    (Join-Path $InstallDir "VC\Tools\MSVC\$msvcVer\bin\Hostx64\x64\lib.exe"),
    (Join-Path $InstallDir "VC\Tools\MSVC\$msvcVer\bin\Hostx64\x64\dumpbin.exe"),
    (Join-Path $winSdkRoot "Windows Kits\10\bin\$sdkVer\x64\rc.exe"),
    (Join-Path $winSdkRoot "Windows Kits\10\bin\$sdkVer\x64\mt.exe"),
    (Join-Path $winSdkRoot "Windows Kits\10\Include\$sdkVer\um\windows.h"),
    (Join-Path $winSdkRoot "Windows Kits\10\Include\$sdkVer\um\winsock2.h"),
    (Join-Path $winSdkRoot "Windows Kits\10\Include\$sdkVer\um\d3d11.h"),
    (Join-Path $winSdkRoot "Windows Kits\10\Include\$sdkVer\ucrt\stdio.h"),
    (Join-Path $winSdkRoot "Windows Kits\10\Lib\$sdkVer\um\x64\kernel32.lib"),
    (Join-Path $winSdkRoot "Windows Kits\10\Lib\$sdkVer\ucrt\x64\ucrt.lib"),
    (Join-Path $InstallDir "VC\Tools\MSVC\$msvcVer\lib\x64\msvcrt.lib")
)

foreach ($req in $requiredFiles) {
    if (-not (Test-Path $req)) {
        throw "Falta archivo requerido tras la extraccion: $req"
    }
}

$totalBytes = (Get-ChildItem $InstallDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
$totalMb = [Math]::Round($totalBytes / 1MB, 1)

Write-Host "`nToolchain MSVC portable lista en: $InstallDir ($totalMb MB en disco)" -ForegroundColor Green
Write-Host ("MSVC : " + $msvcVer)
Write-Host ("SDK  : " + $sdkVer)
$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
(& cl.exe 2>&1) | Select-Object -First 2 | ForEach-Object { "$_" }
$ErrorActionPreference = $prevEap
