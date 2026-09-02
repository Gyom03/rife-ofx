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

Le build copie uniquement le manifeste et le modele de demonstration 4.6.
Les poids 4.25 restent externes.

## Tester RIFE 4.25 sans Resolve

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

## Installer et tester dans Resolve

Fermer Resolve, puis installer le bundle dans une console PowerShell
administrateur :

```powershell
Start-Process -FilePath ".\install_plugin.bat" -Verb RunAs -Wait
```

Configurer ensuite les poids externes pour les nouvelles applications :

```powershell
Start-Process -FilePath ".\install_models.bat" -Wait
```

Relancer Resolve, ajouter `RIFE Frame Interpolator`, activer `Debug`, choisir
`Mode=Advanced`, puis `Model=RIFE 4.25`. Commencer avec `Interpolation Amount`
sur 0.5 et une courte plage de timeline.

Dans DebugView, le premier rendu doit contenir :

```text
model=rife-v4.25 backend=NCNN/Vulkan gpu=0 ... padded=...
```

Un modele absent ou une variante non valide doit produire une erreur explicite;
le plugin ne remplace pas silencieusement le modele par 4.6.

## Diagnostic

Le plugin ecrit ses diagnostics avec `OutputDebugStringA`. DebugView de
Sysinternals permet de voir les timestamps temporels, le chargement du modele,
les dimensions padded et le temps d'inference.
