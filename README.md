# VM Spoof Patcher (C++)

Genera un `vmware-vmx.exe` y un `EFI64.ROM` parcheados y coherentes para spoofear
GPU/SMBIOS/discos/USB/audio en VMware Workstation, con backups, validación por offset
y verificación del resultado. GUI nativa Win32.

- **GUI**: `build\vmspoof-gui.exe`
- Sin dependencias en runtime: LZMA SDK vendorizado, SHA-256 con BCrypt, GUI con GDI+.

## Requisitos

- Windows 10/11 x64.
- Visual Studio Build Tools con el workload **Desktop development with C++**
  (probado con VS BuildTools 18 / MSVC 14.51 y Windows SDK 10.0.26100).
- No hace falta CMake ni Python.

## Build

```bat
build.cmd
```

Genera:

```
build\vmspoof-gui.exe
build\tests.exe
release\   vmspoof-gui.exe, profiles\, back up\ y driver.pak (origen comprimido)
```

`release\` es lo único que hay que distribuir: binarios + perfiles + originales + driver,
sin objetos ni temporales de compilación. El paquete original va comprimido en
`driver.pak` y `patched driver\` solo se crea cuando el cliente pulsa `Generar driver`.

`build.cmd` localiza `vcvars64.bat` en la ruta habitual de BuildTools; si tu instalación
está en otro lado, define `VCINSTALLDIR` antes de ejecutarlo. También copia `profiles/`
a `build\profiles\` para que la GUI encuentre los perfiles aunque se lance desde otra
carpeta.

## GUI

Doble clic en `build\vmspoof-gui.exe` (pide elevación: instala directamente en VMware).

- Tema oscuro, DPI-aware, perfil `Full spoof` / `GPU only` o un JSON personalizado.
- Detecta la instalación de VMware desde el registro y usa los originales de
  `back up\` como entrada si existen.
- **No hay carpeta de salida**: `Instalar patch` parchea en `%TEMP%` y reemplaza
  `vmware-vmx.exe` / `EFI64.ROM` en `...\VMware Workstation\x64\` con backup atómico.
- Botones: `Instalar patch` (ambos), `Solo .exe`, `Solo EFI`, `Generar driver`,
  `Verificar`, `Restaurar` (originales), y `Configurar VM` para aplicar hardening al `.vmx`.
- `Generar driver` extrae `driver.pak`, lo copia a `patched driver\`,
  cambia el hardware ID del INF y los VID/DID de `vm3dgl*.dll` a los del perfil/IDs
  personalizados, y quita el `.PNF` cacheado. No instala el driver.
- Opciones: `Sobrescribir`, `Dry-run`, `Configurar VM`, `IDs personalizados`.
- El log muestra offsets, hashes, tamaño comprimido y errores; la píldora de estado
  pasa a `OK` o `Error`.

Antes de cada instalación se crea:

- `backup-YYYYMMDD-HHMMSS[\-n]\` con los archivos actuales.
- `vmspoof-original\` con la copia prístina del input (si coincide con el hash original),
  usada por `Restaurar`.

### Restaurar

`Restaurar` busca un `vmware-vmx.exe` / `EFI64.ROM` original, en este orden:

1. los campos de entrada de la GUI (por defecto `back up\`),
2. `...\x64\vmspoof-original\`,
3. los backups `backup-*\` cuyo hash coincida con el original,
4. `VMSPOOF_VMX_ORIG` / `VMSPOOF_EFI_ORIG` y `back up\` junto al repo.

Verifica el hash original, respalda el estado actual y reemplaza de forma atómica.
Si indicaste un `.vmx`, elimina el hardening aplicado (con backup del `.vmx`). No
borra el NVRAM.

## Salidas

La GUI parchea en `%TEMP%` e instala directamente. Con el perfil `fullspoof` el
`vmware-vmx.exe` resultante debe dar exactamente:

```
BE9AD4329EDB9442ADC49EABD7BCBAE55A193E9FB837F08AD1670973B56DE5F3
```

El `EFI64.patched` no tiene un SHA fijo (se recomprime con el SDK vendorizado, que no
produce un stream idéntico bit a bit al de liblzma). Lo que importa, y se verifica:

- ROM de `2.097.152` bytes.
- Sección LZMA válida y descomprimible a `0x48E010` bytes.
- El DXE difiere del original **solo** en:
  - `0x1CE759..0x1CE75C` (AHCI `8086:A114`)
  - `0x314355..0x314358` (SVGA `10DE:1001`)
- Con `gpu-only`, solo cambian los 4 bytes de SVGA.

`Generar driver` busca `driver.pak` (o un `oem5.inf_amd64_*`) junto al ejecutable o al
repo (también acepta `VMSPOOF_DRIVER_DIR`); el `.pak` se extrae en `%TEMP%` con el
`tar.exe` de Windows y se borra al terminar. Genera junto al ejecutable la carpeta
visible `patched driver\` con el INF y las DLLs ajustadas al VID/DID del perfil. El INF y los
`vm3dgl*.dll` modificados ya no coinciden con `vm3d.cat`, así que el driver se instala
igual que el paquete original modificado: con la comprobación de firma desactivada
(modo de pruebas). Incluye un `LEEME.txt` con el comando `pnputil`.

## Perfiles

En `profiles/`:

- `vmware-16.2.4-fullspoof.json`: 46 entradas, 385 bytes, GPU + AHCI/PCIe + USB +
  audio + SMBIOS + discos. Parchea SVGA y AHCI en el EFI.
- `vmware-16.2.4-gpu-only.json`: solo SVGA (`0x221590`, `0x2215db`). NO toca AHCI;
  el EFI solo parchea `VmwSvgaDxe`.

El perfil acepta el nombre (`fullspoof`, `gpu-only`) o una ruta `.json`. La búsqueda
por nombre mira `%VMSPOOF_PROFILES%`, `<dir del exe>\profiles`, `<dir del exe>\..\profiles`
(layout del repo), `.\profiles` y `..\profiles`.

**Regla crítica de coherencia**: si el `.exe` cambia el AHCI (`cat: "ahci"`), el perfil
debe llevar `efi64.ahci.patch = true` con el mismo `vid/did`; si no lo cambia, debe
llevar `false`. Lo mismo para SVGA. Un desajuste hace que el firmware UEFI no vea el
CD-ROM (AHCI) o no tenga GOP (SVGA). El cargador de perfiles rechaza la incoherencia.

Nota sobre el manifiesto publicado: la entrada `0x8b9488` (modelo del disco SCSI) es de
**30 bytes** (`...2020202020`); el valor de 32 bytes del enunciado original sobrescribía
los dos terminadores NUL y rompía el hash golden. El perfil del repo usa los 30 bytes
reales del build de referencia; el hash `BE9AD4...` lo confirma.

## Hardening del .vmx

El `EFI64.ROM` se instala globalmente, así que el `.vmx` ya no necesita
`efi64.filename`. `Configurar VM` en la GUI aplica al `.vmx` elegido el bloque de
hardening anti-detección de `Windows 10 x64.vmx`:

1. Hace backup del `.vmx` (`*.bak-<timestamp>`).
2. Agrega o actualiza las claves `isolation.*`, `monitor_control.*`, `cpuid.*`
   (máscara del bit de hypervisor y brand string `Intel(R) Core(TM) i5-7500`),
   `smbios.*`, `serialNumber.reflectHost`, `board-id.reflectHost`,
   `hw.model.reflectHost` y `sched.mem.pshare.enable`.
3. Marca el bloque con `# vmspoof hardening` y quita cualquier `efi64.filename` viejo.

`Restaurar` elimina esas claves y el marcador. No borra el NVRAM (`nvram = ...`).

### Arranque UEFI

- En este firmware el orden por defecto intenta NVMe/Network; pulsa **ESC** al arrancar
  para abrir el menú de boot y elegir el CD-ROM.
- Si usas el CD-ROM para instalar, en el `.vmx` debe estar conectado al encender:

```ini
ide1:0.present = "TRUE"
ide1:0.deviceType = "cdrom-image"
ide1:0.fileName = "C:\ISOs\Win11.iso"
ide1:0.startConnected = "TRUE"
```

### IDs personalizados

Activa `IDs personalizados` y edita `GPU` / `AHCI` (formato `VID:DID` en hex).

- SVGA actualiza los dos `cat=svga` del `.exe` y `VmwSvgaDxe`.
- AHCI actualiza todas las entradas `cat=ahci` y `VmwAhciDxe`.
- Las entradas `pcie` (root ports) conservan `8086:A114`; no se tocan para no romper el
  chipset del perfil.
- Con IDs personalizados se desactiva el hash golden esperado (no es conocido); la
  validación sigue siendo por offset y por coherencia EFI↔`.exe`. Para verificar, usa
  los mismos IDs con `Verificar`.
- `gpu-only` no admite IDs de AHCI (el perfil no parchea AHCI).

### Reinstalar / re-spoofear

Si el archivo de entrada ya está parcheado:

- Si coincide con el hash esperado del perfil, se reutiliza y se puede volver a
  instalar (no se modifica).
- Si el EFI ya tiene los IDs del perfil, se reutiliza sin recomprimir.
- Si está parcheado con otro perfil o con IDs viejos, se valida contra los valores
  conocidos (original / `new` del perfil / IDs actuales) y se puede re-spoofear; si el
  estado no es conocido, activa `Sobrescribir`.
- Instalar requiere elevación y VMware cerrado.

## Seguridad

- Sin `Dry-run`, la GUI siempre escribe en `C:\Program Files (x86)\VMware\...`.
- Toda salida se escribe con `tmp` + `MoveFileExW` (atómico).
- La instalación verifica que el destino no esté bloqueado (VM corriendo), crea
  `backup-YYYYMMDD-HHMMSS\` junto al archivo y luego reemplaza.
- Si un solo `orig` no coincide, no se escribe ninguna salida.
- El `.exe` y el `EFI64.ROM` parcheados son globales; el hardening del `.vmx` es por VM.

## Tests

```bat
tests\run-tests.cmd
```

89 checks, sin frameworks externos. Cubren:
- localización de la sección LZMA (`0x6E040`, datos `0x6E058`, capacidad `0xE37C8`);
- roundtrip LZMA (props, cabecera de tamaño, bytes);
- unicidad de patrones SVGA/AHCI;
- validación de `orig` y escritura todo-o-nada;
- coherencia de perfiles y rechazo de perfiles incoherentes;
- idempotencia (segundo pase no cambia el resultado) y reinstalación de archivos ya
  parcheados;
- IDs personalizados coherentes en `.exe` + EFI, y rechazo de AHCI custom en `gpu-only`;
- golden exe (`BE9AD4...`);
- golden EFI (solo los 8 bytes esperados, ROM de 2 MiB);
- errores de hash, salida existente y archivo inexistente;
- instalación/restauración en una instalación falsa (`VMSPOOF_VMWARE_DIR`): backups,
  stash `vmspoof-original`, hashes originales y limpieza del hardening del `.vmx`;
- `manifest-diff`.

Rutas configurables:

```bat
set VMSPOOF_VMX_ORIG=C:\ruta\vmware-vmx.exe
set VMSPOOF_EFI_ORIG=C:\ruta\EFI64.ROM
set VMSPOOF_VMWARE_DIR=C:\ruta\fake-vmware
tests\run-tests.cmd
```

Si faltan los binarios, los tests golden se marcan `SKIP` y el resto se ejecuta.

## Troubleshooting

- **"SHA-256 no coincide"**: tu VMware ya está parcheado o es otro build. Usa los
  originales de `back up\`; no actives `Sobrescribir` salvo que sepas que el build es
  compatible.
- **"LZMA comprimido excede la capacidad"**: el build del firmware no es el esperado.
  No se relajan parámetros en silencio.
- **No aparece el CD-ROM en UEFI**: pulsa `ESC` en el boot; el orden por defecto intenta
  NVMe/Network. Verifica `startConnected = "TRUE"`.
- **"acceso denegado"**: la GUI ya pide elevación; acepta el UAC. Cierra VMware
  (el chequeo de lock detecta VMs corriendo).
- **La GUI no cambia de tamaño/fuentes raras**: es DPI-aware; si usas escalado mixto,
  mueve la ventana al monitor y Windows enviará `WM_DPICHANGED`.

## Limitaciones (v1)

- Solo `EFI64.ROM` (2.097.152 bytes, build `VMW71.00V.16722896.B64.2008100651`).
  `EFI32.ROM` se rechaza con un mensaje claro.
- No se parchea `VGA.ROM` para CSM. Si usas BIOS/CSM, el spoof de GPU no aplica al
  `VGA.ROM`; se documenta como TODO.
- `AcpiPlatformDxe` (`15AD:0790/07A0`) no se toca: no fue necesario para el boot UEFI
  probado. Es una opción avanzada pendiente.
- Windows-only (BCrypt, DWM, rutas de VMware).
- La GUI no implementa accesibilidad completa (tab order fino/lector de pantalla).

## Estructura

```
src/core.cpp      perfiles (JSON), SHA-256, patch exe/EFI, verify, install, report
src/gui.cpp       GUI Win32 custom (GDI+, DPI-aware)
third_party/lzma  LZMA SDK 26.04 (public domain)
profiles/         perfiles versionados
tests/            test_core.cpp + run-tests.cmd
build.cmd         compila GUI y tests con MSVC
```
