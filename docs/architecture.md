# Architecture par etapes

## Passthrough et acces temporel

`src/PluginMain.cpp` est le plugin OpenFX. `TemporalFrameProvider` expose :

```cpp
getFrame(time)
getFrameOffset(time, offset)
getTemporalWindow(time, before, after)
```

La fenetre par defaut demande `F[n-1]`, `F[n]`, `F[n+1]` et `F[n+2]`. Les
appels sont caches par timestamp et les bords temporels sont traites
explicitement pour Resolve. RIFE consomme actuellement `F[n]` et `F[n+1]`;
les deux frames externes restent disponibles pour les futures verifications.

## Moteur multi-modeles

```text
RifeEngine
  +-- ModelRegistry
  +-- IRifeBackend
        +-- RifeNcnnBackend
        +-- futur OnnxBackend
        +-- futur TensorRTBackend
```

`ModelRegistry` lit `registry.csv`. Les chemins des modeles sont relatifs au
registre et peuvent etre resolus dans le dossier externe indique par
`RIFEOFX_MODELS_ROOT`; aucun chemin de poids absolu n'est code dans le plugin.

`RifeEngine::loadModel(id)` charge le modele une seule fois, garde le backend
en vie entre les frames et detruit proprement l'ancien modele lors d'un
changement. Les mutex protegent le changement de modele et l'inference.

## Adaptateur valide

RIFE 4.25 reutilise le pipeline NCNN/Vulkan RIFE v4, mais son alignment est
different de celui de v4.6. `RifeNcnnBackend` applique l'alignement du
descriptor avant `RIFE::process`, puis recadre la sortie. Cette logique ne
depend pas d'un chemin de modele ou d'un nom de version hardcode.

Le prototype valide 4.6, 4.25, 4.25 Lite et 4.22 Lite. Les variantes 4.26 et
4.26 Large sont activees pour validation apres smoke test; les choix non
valides restent refuses avec un message `model adapter not validated yet`.

## Diagnostics

Avec `Debug`, `OutputDebugStringA` rapporte le modele, backend, GPU, resolution
d'entree, resolution padded et temps d'inference. La VRAM exacte n'est pas
exposee par l'API actuelle; le registre conserve seulement une estimation quand
elle est connue.
