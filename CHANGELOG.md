# Changelog

Todas las novedades relevantes de `obs-network-device` se documentan en este archivo.

## [1.0.0] - 2026-11-24

### Agregado
- Fuente nativa **`Dispositivo de red`** (`network_device_source`) para OBS Studio 32+ / 33+ en Windows x64.
- Descubrimiento automático ONVIF en LAN por **WS-Discovery** (`239.255.255.250:3702`) y agregado manual por IP/puerto.
- Inspección ONVIF **Media1 y Media2** (`H.264`, `H.265`, `MJPEG`), autenticación **WS-UsernameToken PasswordDigest** con ajuste automático de desfase horario y vista previa por Snapshot HTTP.
- Audio de cámara integrado como canal independiente en el **Mezclador de audio** de OBS con vúmetro propio.
- Selector de **`Perfil / Entrada detectada`** en Propiedades (`Detectar perfiles / entradas de cámara`) y 7 modos de entrada (`RTSP UDP`, `RTSP TCP`, `UDP Multicast`, `RTSP HTTP`, `RTSP Auto`, `HTTP MJPEG` y **`Snapshot HTTP continuo`** nativo sin buffer).
- Ajuste de latencia (`Cero buffer`, `Ultra baja latencia`, `Equilibrado`, `Estándar`), demuxer (`input_format`), buffer en MB, GPU hardware decode, switch de audio RTSP y opciones extra de FFmpeg.
- Dock **`Control PTZ (Red)`** con accesos directos desde **clic derecho sobre la fuente → `Ver control PTZ`**, botón en **Propiedades** y menú **Herramientas**.
- **Panorámica interactiva + `Ventana grande`:** captura en grilla serpentina (Snapshot ONVIF o captura gráfica OBS), cosido automático con solapamiento, clic para mover Pan/Tilt, **clic y arrastrar para encuadrar cuadro con Pan + Tilt + Zoom**, calibración de `Zoom opt` (`2x–40x`) y persistencia por cámara (`%APPDATA%\obs-studio\plugin_config\obs-network-device\cameras\`).
- **Presets visuales** con miniatura de imagen y coordenadas PTZ + pines sobre la panorámica, y gestión de presets internos ONVIF.
- **Joystick analógico virtual** 2D proporcional + soporte para **gamepads XInput y joysticks USB WinMM**.
- Script `scripts/package.ps1` para generar el paquete ZIP de release con verificación `SHA256SUMS.txt`.
