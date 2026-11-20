# obs-network-device

Plugin nativo en C++ / Qt6 para **OBS Studio (v32+ / v33+)** que agrega una fuente **"Dispositivo de red"** (*Network Device*) con descubrimiento automático en LAN, inspección de dispositivo, autenticación, vista previa con captura (screenshot), selección de streams y **control PTZ automático** integrado en un dock de OBS.

Actualmente implementa **ONVIF (Profile S / Profile T / Media v1 & Media2 v2 / PTZ)** sobre una arquitectura modular (`IDeviceProvider`) pensada para sumar futuros protocolos de red (NDI, RTSP genérico, etc.) dentro del mismo explorador.

---

## Características principales

- **Nueva fuente en OBS (`+` → Dispositivo de red):** se integra como una fuente más de video y audio (usa internamente `ffmpeg_source` de OBS, heredando RTSP TCP/UDP, H.264/H.265, decodificación por hardware y mezcla de audio en el Mixer).
- **Escaneo automático de red (WS-Discovery):** envía probes UDP multicast (`239.255.255.250:3702`) por todas las interfaces IPv4 activas para encontrar cámaras ONVIF en la red local, con opción de agregado manual por `IP:puerto`.
- **Autenticación compatible ONVIF:** sincronización automática de reloj (`GetSystemDateAndTime`) + firma **WS-UsernameToken PasswordDigest** y soporte HTTP Basic/Digest para capturas. Si la cámara exige credenciales, avisa en pantalla y permite ingresar usuario/clave al instante.
- **Inspección completa del dispositivo:**
  - Marca, modelo, versión de firmware y número de serie (`GetDeviceInformation`).
  - Detección de servicios **Media v1**, **Media2 v2 (H.265)** y **PTZ** (`GetServices` con fallback a `GetCapabilities`).
  - Listado de perfiles/streams con resolución, codec, FPS, audio y soporte PTZ (`GetProfiles` + `GetStreamUri`).
  - **Vista previa (Screenshot):** descarga y muestra la captura en vivo de la cámara (`GetSnapshotUri`) con botón para refrescar.
- **Dock PTZ automático (`Paneles` → `Control PTZ (Red)`):**
  - Cada fuente configurada con soporte PTZ se registra automáticamente (sin configurar la IP dos veces).
  - Pad direccional de 8 direcciones + **Home**, **Zoom +/-**, control de velocidad y gestión completa de **Presets** reales de la cámara (`Ir`, `Guardar`, `Borrar`, `Recargar`).
  - Opción **"Seguir escena activa"** para controlar automáticamente la cámara presente en la escena actual.
- **Acceso rápido en menú:** `Herramientas` → `Explorar dispositivos de red...` para descubrir una cámara y agregarla directamente a la escena activa.

---

## Requisitos en Windows

- **Windows 10 / 11 (x64)**
- **OBS Studio 32.0+** (probado con **OBS Studio 33.0.1**; utiliza los binarios Qt6 incluidos en OBS sin DLLs extra)
- **Git**, **CMake 3.28+**, **Ninja** y **Visual Studio 2022 Build Tools** (carga de trabajo *Desarrollo para el escritorio con C++* + Windows 10/11 SDK)

---

## Instalación rápida de dependencias y compilación

### 1. Instalar herramientas de compilación (una sola vez)

Abrí **PowerShell como Administrador** y ejecutá:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\setup-env.ps1
```

Esto verifica e instala automáticamente con `winget`:
- `Kitware.CMake`
- `Ninja-build.Ninja`
- `Microsoft.VisualStudio.2022.BuildTools` (C++ + Windows SDK)

### 2. Descargar dependencias de OBS/Qt6, compilar e instalar

Desde una terminal PowerShell normal en la carpeta del repo:

```powershell
.\scripts\build.ps1 -Install
```

Qué hace `build.ps1`:
1. Configura CMake con el preset `windows-x64` (descarga automáticamente `obs-deps`, `Qt6` y los fuentes de `obs-studio 32.0.4` en `.deps/` y compila `libobs` + `obs-frontend-api`).
2. Compila `obs-network-device.dll` y la suite de tests `obs-network-device-tests.exe`.
3. Ejecuta los tests unitarios con `ctest`.
4. Con `-Install`, copia el plugin en `C:\ProgramData\obs-studio\plugins\obs-network-device\`.

> **Nota:** La primera compilación puede demorar varios minutos porque descarga y compila `libobs` en `.deps/`. Las compilaciones siguientes son incrementales y tardan segundos.

---

## Uso en OBS Studio

1. Abrí **OBS Studio**.
2. En **Fuentes**, hacé clic en **+** → **Dispositivo de red** (o usá **Herramientas** → **Explorar dispositivos de red...**).
3. En la ventana del explorador:
   - Se escanea la red automáticamente (o podés agregar la cámara por **IP + Puerto**).
   - Seleccioná la cámara; si tiene candado o pide autenticación, cargá **Usuario** y **Contraseña** y tocá **Conectar / Escanear dispositivo**.
   - Revisá el **Screenshot**, elegí el perfil de video deseado (MainStream, SubStream, etc.) y tocá **Usar esta cámara**.
4. Si la cámara tiene PTZ, abrí **Paneles** → **Control PTZ (Red)** para moverla y usar sus presets.

---

## Arquitectura del proyecto

```text
obs-network-device/
├── .github/workflows/build.yml   # CI automático en GitHub Actions (Windows x64)
├── cmake/                        # Módulos CMake basados en obs-plugintemplate
├── data/locale/                  # Traducciones (es-ES, en-US)
├── scripts/
│   ├── setup-env.ps1             # Instala CMake, Ninja y VS Build Tools con winget
│   ├── build.ps1                 # Configura, descarga .deps, compila y corre ctest
│   └── install.ps1               # Instala el .dll y locales en OBS Studio
├── src/
│   ├── core/                     # IDeviceProvider, tipos de dispositivo, DeviceRegistry
│   ├── onvif/                    # WS-Discovery, WS-UsernameToken, OnvifClient (SOAP/HTTP)
│   ├── source/                   # Fuente compuesta OBS (envuelve ffmpeg_source)
│   ├── ui/                       # Explorador visual de red + Dock PTZ (Qt6)
│   └── plugin-main.cpp           # Registro de módulo, menú Herramientas y Dock
├── tests/
│   └── test_onvif.cpp            # Tests unitarios de auth, discovery y parsers XML ONVIF
├── buildspec.json
├── CMakeLists.txt
└── CMakePresets.json
```

### Extender a otros protocolos (NDI, RTSP, etc.)

Cada protocolo implementa la interfaz `IDeviceProvider` (`src/core/device-provider.hpp`). Para sumar un nuevo protocolo en el futuro solo se implementa su proveedor y se registra en el explorador (`DeviceExplorerDialog`), reutilizando la fuente compuesta de OBS y la UI existente.

---

## Licencia

Distribuido bajo licencia **GNU General Public License v2.0** (ver [LICENSE](LICENSE)).
