# obs-network-device

![OBS Studio](https://img.shields.io/badge/OBS%20Studio-32.0%2B%20%2F%2033.0%2B-302E31?logo=obsstudio&logoColor=white)
![Platform](https://img.shields.io/badge/Windows-x64-0078D6?logo=windows&logoColor=white)
![C++17 / Qt6](https://img.shields.io/badge/C%2B%2B17-Qt6-00599C?logo=cplusplus&logoColor=white)
![Release](https://img.shields.io/badge/Release-v1.0.0-22c55e)
![License](https://img.shields.io/badge/License-GPL--2.0-blue)

Plugin nativo en **C++17 / Qt6** para **OBS Studio** que incorpora la fuente **"Dispositivo de red"** (*Network Device*) con descubrimiento automático en LAN (**ONVIF**), audio integrado al Mezclador de OBS, selección de perfiles y transporte de baja latencia directamente desde Propiedades, y un sistema completo de **control PTZ interactivo con mapa panorámico, recuadro de zoom, presets visuales y joystick analógico virtual + USB**.

Pensado sobre una arquitectura modular (`IDeviceProvider`) para sumar otros protocolos de red a futuro (NDI, RTSP genérico, etc.) dentro del mismo flujo de OBS Studio.

---

## Características principales

### 1. Descubrimiento, inspección y fuente de audio/video en OBS
- **Escaneo automático de red (WS-Discovery):** envía probes UDP multicast (`239.255.255.250:3702`) por todas las interfaces IPv4 activas para encontrar cámaras ONVIF automáticamente, más agregado manual por `IP:puerto`.
- **Autenticación compatible ONVIF:** sincronización automática de reloj (`GetSystemDateAndTime`) + firma **WS-UsernameToken PasswordDigest** y autenticación HTTP Basic/Digest para capturas.
- **Audio nativo en el Mezclador de OBS:** reenvía el audio de la cámara como canal propio del **Mezclador de audio** con vúmetro, volumen, filtros y monitoreo independientes (`obs_source_output_audio`).
- **Media v1 + Media v2:** consulta `GetServices` / `GetCapabilities`, `GetDeviceInformation` (marca, modelo, firmware, serie) y todos los perfiles de video/audio (`H.264`, `H.265`, `MJPEG`) con vista previa en vivo (`GetSnapshotUri`).

### 2. Entradas detectadas y ajuste fino de latencia en Propiedades
Desde **Propiedades** de cualquier fuente `Dispositivo de red`:
- **Detección en un clic (`Detectar perfiles / entradas de cámara`)** y selector **`Perfil / Entrada detectada`** para alternar entre los perfiles H.264, MJPEG, RTSP Media1/Media2 y Snapshot HTTP reales de tu cámara.
- **7 modos de transporte / entrada (`Protocolo / Tipo de entrada`):**
  - `RTSP sobre UDP (Menor delay)`
  - `RTSP sobre TCP (Estable)`
  - `RTSP UDP Multicast`
  - `RTSP sobre HTTP (Túnel)`
  - `RTSP Automático (FFmpeg)`
  - `HTTP MJPEG directo`
  - `Snapshot HTTP continuo (JPEG sin buffer)` — streamer HTTP nativo multi-hilo con Keep-Alive y renderizado directo sin colas de FFmpeg (`1–30 fps`).
- **Control de latencia (`Modo de latencia`):** `Cero buffer` (`analyzeduration=0`, `probesize=32768`, `max_delay=0`), `Ultra baja latencia`, `Equilibrado` y `Estándar`, junto con demuxer (`input_format`), slider de buffer en MB, **Decodificación por hardware (GPU)**, switch **`Capturar audio del stream`** (`allowed_media_types=video` al desactivarlo) y **Opciones extra de FFmpeg**.

### 3. Control PTZ interactivo con Panorámica, Zoom por cuadro y Joystick
Accesible con **clic derecho sobre la fuente ONVIF → `Ver control PTZ`**, desde el botón **`Abrir control PTZ...`** en Propiedades, desde **`Herramientas` → `Control PTZ (Red)`** o desde **`Paneles` → `Control PTZ (Red)`**:

- **Mapa panorámico clicable (`Panorámica` + `Ventana grande`):**
  - Barrido automático en grilla configurable (`3×2` a `7×4` o personalizada) por Snapshot ONVIF HTTP con fallback automático a captura del frame renderizado de la fuente en OBS (`OBS_TASK_GRAPHICS` / `gs_texrender`).
  - Cosido con registro de solapamientos y extrapolación bilineal precisa hasta todo el rango PTZ (`[-1.0, +1.0]`), mostrando el encuadre actual en **cian** y los pines numerados en **ámbar**.
  - **Clic simple:** mueve Pan/Tilt al punto exacto.
  - **Clic y arrastrar:** dibuja un **cuadro verde** sobre la zona deseada y envía **Pan + Tilt + Zoom** (`AbsoluteMove`) para encuadrarla automáticamente, calibrable por cámara con el selector **`Zoom opt:`** (`2x–40x`).
- **Persistencia automática por cámara:** cada cámara guarda su panorámica, calibración de ejes/zoom y presets en `%APPDATA%\obs-studio\plugin_config\obs-network-device\cameras\` identificada por fabricante/modelo/número de serie (con respaldo por host).
- **Presets visuales con miniatura:** guardá encuadres con imagen de referencia y coordenadas `Pan/Tilt/Zoom`, además de los presets internos ONVIF de la cámara.
- **Joystick 2D proporcional + Gamepad/Joystick USB:** pad virtual en pantalla con retorno al centro e inversión persistida de Pan/Tilt, y soporte a 50 Hz para gamepads **XInput** (carga dinámica) y joysticks USB **`winmm`** con disparo rápido de presets y `Home`.

---

## Instalación rápida (Windows x64)

1. Descargá `obs-network-device-v1.0.0-windows-x64.zip` desde la sección [**Releases**](https://github.com/blairq/obs-network-device/releases/latest).
2. Verificá opcionalmente la integridad con `SHA256SUMS.txt`.
3. Cerrá **OBS Studio** si está abierto.
4. Descomprimí el ZIP de modo que la carpeta `obs-network-device` quede en:

   ```text
   C:\ProgramData\obs-studio\plugins\obs-network-device\
   ├── bin\64bit\obs-network-device.dll
   ├── data\locale\
   │   ├── en-US.ini
   │   └── es-ES.ini
   ├── INSTALL.txt
   └── LICENSE
   ```

5. Abrí **OBS Studio**. Usa las librerías Qt6 y FFmpeg ya incluidas en OBS Studio (`32.0+` / `33.0+`) sin instalar runtimes adicionales.

---

## Uso rápido y consejos de baja latencia

1. En OBS Studio, tocá **Fuentes → `+` → `Dispositivo de red`** (o **`Herramientas` → `Explorar dispositivos de red...`**).
2. Elegí tu cámara escaneada (o ingresá `IP:puerto`), cargá usuario/clave si hace falta y tocá **`Usar esta cámara`**.
3. Para abrir el control PTZ en cualquier momento: **clic derecho sobre la fuente ONVIF → `Ver control PTZ`** (o **`Paneles` → `Control PTZ (Red)`**).
4. En **Capturar panorámica...**, dejá el rango vertical amplio (por defecto `-0.85 a 0.85`) para cubrir toda la altura del cabezal, y luego movete con clic simple o **clic y arrastrar** un cuadro.
5. **Para reducir el delay al mínimo en Propiedades:**
   - Tocá **`Detectar perfiles / entradas de cámara`** y probá **`RTSP sobre UDP (Menor delay)`** + **`Cero buffer`** + **Decodificación por hardware (GPU)**.
   - Si no necesitás el audio en esa fuente, destildá **`Capturar audio del stream`**.
   - Si tu cámara expone un perfil **MJPEG** (o **`Snapshot HTTP continuo`**), suele ofrecer la menor latencia posible al evitar el buffering de GOP H.264.

---

## Compilar y empaquetar desde el código fuente (Windows x64)

### Requisitos
- **Windows 10 / 11 (x64)**
- **CMake 3.28+**, **Ninja** y compilador **MSVC** (`vs2022` o modo portable sin Administrador)

### 1. Preparar compilador (una sola vez)

- **Opción A — Portable sin permisos de Administrador (~450 MB en `%LOCALAPPDATA%\obs-portable-msvc`):**
  ```powershell
  winget install Kitware.CMake Ninja-build.Ninja
  .\scripts\setup-portable-msvc.ps1
  ```
- **Opción B — Con Visual Studio 2022 Build Tools (PowerShell como Administrador):**
  ```powershell
  Set-ExecutionPolicy -Scope Process Bypass
  .\scripts\setup-env.ps1
  ```

### 2. Compilar, testear, instalar y empaquetar

```powershell
# Descarga obs-deps / Qt6 / libobs 32.0.4 en .deps/ (solo la 1ra vez), compila y corre ctest
.\scripts\build.ps1 -Install

# Genera el ZIP de release y SHA256SUMS.txt en dist/
.\scripts\package.ps1
```

---

## Estructura del repositorio

```text
obs-network-device/
├── cmake/                             # Módulos CMake (basados en obs-plugintemplate)
├── data/locale/                       # Traducciones (es-ES.ini, en-US.ini)
├── scripts/
│   ├── setup-portable-msvc.ps1        # Descarga MSVC + Win11 SDK portable sin Admin
│   ├── setup-env.ps1                  # Instala CMake, Ninja y VS Build Tools (Admin)
│   ├── build.ps1                      # Configura, compila y ejecuta OnvifCoreTests
│   ├── install.ps1                    # Instala DLL, PDB y locales en OBS Studio
│   └── package.ps1                    # Genera ZIP de distribución y SHA256SUMS.txt
├── src/
│   ├── core/                          # DeviceRegistry, StreamOptions, HttpSnapshotStreamer,
│   │                                  # PanoramaBuilder/Mapper, PanoramaScanner, PanoramaStore, FrameCapture
│   ├── input/                         # JoystickController (XInput dinámico + WinMM 50 Hz)
│   ├── onvif/                         # WS-Discovery, WS-UsernameToken, OnvifClient (Media1/Media2/PTZ)
│   ├── source/                        # Fuente OBS compuesta + modo Snapshot HTTP nativo
│   ├── ui/                            # DeviceExplorerDialog, PtzDockWidget, PanoramaViewWidget,
│   │                                  # PanoramaScanDialog, VirtualJoystickWidget
│   └── plugin-main.cpp                # Registro de módulo, Dock, Herramientas y menú contextual
├── tests/
│   └── test_onvif.cpp                 # Suite unitaria Qt (auth, discovery, parsers, panorámica, stream options)
├── buildspec.json
├── CHANGELOG.md
├── CMakeLists.txt
└── CMakePresets.json
```

### Extender a nuevos protocolos

Todo protocolo de descubrimiento/inspección implementa `IDeviceProvider` ([src/core/device-provider.hpp](src/core/device-provider.hpp)), reutilizando el explorador, la fuente compuesta de OBS y los controles de red existentes.

---

## Licencia

Distribuido bajo licencia **GNU General Public License v2.0** — ver [LICENSE](LICENSE).
