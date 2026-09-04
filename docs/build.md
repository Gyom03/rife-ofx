# Build du prototype

## Prerequis

- Windows 11 x64
- Visual Studio 2022 Build Tools, workload Desktop development with C++
- CMake 3.24 ou plus recent
- Vulkan SDK LunarG avec headers et loader library

Verifier :

```powershell
$env:VULKAN_SDK
Test-Path "$env:VULKAN_SDK\Include\vulkan\vulkan.h"
Test-Path "$env:VULKAN_SDK\Lib\vulkan-1.lib"
```

## Compiler le build RIFE

Depuis `C:\Users\guill\Desktop\RIFE` :

```powershell
cmake -S . -B build-rife -G "Visual Studio 17 2022" -A x64 `
  -DOPENFX_SOURCE_DIR="C:\Users\guill\Desktop\RIFE\build-rife\_deps\openfx-src" `
  -DRIFE_ENABLE_INFERENCE=ON `
  -DVulkan_INCLUDE_DIR="$env:VULKAN_SDK\Include" `
  -DVulkan_LIBRARY="$env:VULKAN_SDK\Lib\vulkan-1.lib"
cmake --build build-rife --config Release --parallel
```

Le bundle est :

```text
build-rife\RifeFrameInterpolator.ofx.bundle
```

et la DLL elle-meme :

```text
build-rife\RifeFrameInterpolator.ofx.bundle\Contents\Win64\RifeFrameInterpolator.ofx
```

Le build copie uniquement le manifeste et le modele de demonstration 4.6.
Les poids 4.25 restent externes.

## Tests unitaires

```powershell
ctest --test-dir build-rife -C Release --output-on-failure
```

`CadenceMappingTest` couvre le ratio rationnel, la calibration de phase et le
mapping arithmetique, sans hote OFX ni GPU : reduction des cadences NTSC,
recuperation de chaque phase pour quatre cadences, marche sur soixante frames
consecutives, et les cas ou la calibration doit refuser de conclure.

```powershell
& .\build-rife\Release\CadenceMappingTest.exe
& .\build-rife\Release\CadenceMappingTest.exe --dump   # imprime un cycle complet
```

## Tester RIFE sans Resolve

Le smoke test accepte un dossier de modeles et un manifeste separe :

```powershell
& .\build-rife\Release\RifeSmokeTest.exe `
  .\external-models `
  rife-v4.25 `
  .\build-rife\RifeFrameInterpolator.ofx.bundle\Contents\Resources\models\registry.csv
```

Le resultat attendu inclut :

```text
RIFE status=0, ... model=rife-v4.25, padded=128x128
```

La resolution padded depend du modele : avec l'image volontairement non alignee
130x66, RIFE 4.25 utilise 192x128 et RIFE 4.25 Lite utilise 256x128.

## Installer dans Resolve

Fermer Resolve, puis installer le bundle depuis une console PowerShell :

```powershell
Start-Process -FilePath ".\install_plugin.bat" -Verb RunAs -Wait
```

Le script copie le bundle vers :

```text
C:\Program Files\Common Files\OFX\Plugins\RifeFrameInterpolator.ofx.bundle
```

Equivalent manuel, dans une console administrateur :

```powershell
robocopy ".\build-rife\RifeFrameInterpolator.ofx.bundle" `
  "C:\Program Files\Common Files\OFX\Plugins\RifeFrameInterpolator.ofx.bundle" /E
```

Configurer ensuite les poids externes :

```powershell
Start-Process -FilePath ".\install_models.bat" -Wait
```

Relancer Resolve, ajouter `RIFE Frame Interpolator`, activer `Debug`, choisir
`Mode=Advanced` puis le modele voulu.

## Diagnostic

Le plugin ecrit ses diagnostics avec `OutputDebugStringA`, prefixe `[RifeOFX]`,
et duplique la meme trace dans `%TEMP%\RifeOFX-temporal.log`.

Variables d'environnement, a definir avant de lancer Resolve :

| Variable | Effet |
|----------|-------|
| `RIFEOFX_DEBUG=1` | force la trace detaillee sans attendre le parametre `Debug` |
| `RIFEOFX_PROBE_RETIMER=1` | declare aussi `kOfxImageEffectContextRetimer` pour observer si l'hote le propose |
| `RIFEOFX_MODELS_ROOT` | dossier externe contenant les poids |

La procedure de test temporel complete est dans
[temporal-test.md](temporal-test.md).
