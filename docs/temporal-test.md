# Test de l'acces temporel OpenFX

Objectif : verifier experimentalement si Resolve renvoie les vraies frames du
media source, ou des frames deja conformees a la cadence de la timeline.

## 1. Compiler

Depuis `C:\Users\guill\Desktop\RIFE` :

```powershell
cmake -S . -B build-rife -G "Visual Studio 17 2022" -A x64 `
  -DOPENFX_SOURCE_DIR="C:\Users\guill\Desktop\RIFE\build-rife\_deps\openfx-src" `
  -DRIFE_ENABLE_INFERENCE=ON `
  -DVulkan_INCLUDE_DIR="$env:VULKAN_SDK\Include" `
  -DVulkan_LIBRARY="$env:VULKAN_SDK\Lib\vulkan-1.lib"
cmake --build build-rife --config Release --parallel
ctest --test-dir build-rife -C Release --output-on-failure
```

Le bundle produit :

```text
build-rife\RifeFrameInterpolator.ofx.bundle
```

## 2. Installer

Fermer Resolve, puis :

```powershell
Start-Process -FilePath ".\install_plugin.bat" -Verb RunAs -Wait
```

Destination :

```text
C:\Program Files\Common Files\OFX\Plugins\RifeFrameInterpolator.ofx.bundle
```

Relancer Resolve apres chaque installation.

## 3. Configuration du test prioritaire

| Element         | Valeur    |
|-----------------|-----------|
| Media source    | 50 fps    |
| Timeline        | 60 fps    |
| Retime Process  | Nearest   |
| Plugin          | RIFE Frame Interpolator |

1. Creer une timeline 60 fps, y poser un clip source 50 fps.
2. Clip > Retime Process > Nearest.
3. Ajouter `RIFE Frame Interpolator` sur le clip.
4. Mettre `Debug` a true.
5. Laisser la calibration de cadence se faire (13 frames consecutives).
6. Verifier `Detected Framerate`. Si Resolve annonce 60 au lieu de 50, saisir
   `50` dans `Source Framerate` : le plugin ne devine pas la cadence originale a
   partir d'une valeur OFX qui vaut deja 60.
7. Placer la tete de lecture au milieu du clip, pas sur la premiere frame, et
   avancer image par image sur une dizaine de frames.

Pour capturer la trace avant meme d'avoir accede a l'interface, lancer Resolve
avec :

```powershell
$env:RIFEOFX_DEBUG = "1"
& "C:\Program Files\Blackmagic Design\DaVinci Resolve\Resolve.exe"
```

## 4. Ou lire les logs

- DebugView (Sysinternals), filtre `[RifeOFX]`.
- Fichier persistant : `%TEMP%\RifeOFX-temporal.log`. Le chemin exact est
  imprime au chargement du plugin (`temporal log file=...`).

## 5. Resultat de l'experience

Les deux questions que ce document servait a trancher le sont :

**Resolve conforme le media avant que l'effet ne le voie.** Mesure sur le chemin
de rendu, clip 50 fps sur timeline 60 fps : un doublon exact toutes les six
frames, cinq images originales par cycle.

```text
7003  sigA=0xa33e63a56f06
7004  sigA=0xa33e63a56f06   <- doublon
7009  sigA=0xe832df075eee
7010  sigA=0xe832df075eee   <- doublon
```

**Resolve n'expose pas le contexte Retimer** (section 8), et n'expose nulle part
le debut du clip sur l'axe de rendu : toutes les plages sont clip-locales
(`0..1999`) alors que `kOfxPropTime` est global a la composition (`6975`).

Le plugin adresse donc les images originales dans le flux conforme. La phase du
conform est mesuree une fois, puis le mapping est arithmetique. Voir
[cadence.md](cadence.md) pour l'architecture et la procedure de verification.

## 6. Logs attendus

Au chargement :

```text
[RifeOFX] plugin build=Sep  4 2026 16:00:59
[RifeOFX] host name=DaVinciResolve
[RifeOFX] host temporalClipAccess=1
[RifeOFX] plugin temporalClipAccess setStatus=0 readBackStatus=0 value=1
[RifeOFX] describeInContext context=OfxImageEffectContextFilter
[RifeOFX] source clip temporalClipAccess setStatus=0 readBackStatus=0 value=1
[RifeOFX] clip timing sourceConnected=1 sourceFrameRate(mapped)=60.000000           sourceFrameRate(unmapped)=60.000000 outputFrameRate(mapped)=60.000000           projectFrameRate=60.000000 sourceFrameRange=0.000..1999.000           outputFrameRange=0.000..1999.000 continuousSamples=0
[RifeOFX] source frame rate current=50 default=60 hostReported=60 -> kept (user value)
[RifeOFX] calibration=calibrated phase=0 manual=0 periodSource=5 periodTimeline=6           samples=13/13 reason=run=13 from=6975 distinct=11 repeats=2
```

Verifier en premier la ligne `plugin build=` : si elle ne correspond pas au
dernier build, Resolve tourne sur un ancien bundle et rien d'autre n'est
interpretable.

Par frame de sortie, filtre `[RifeOFX][Cadence]` :

```text
[RifeOFX][Cadence]
timelineTime=6979
phase=0
cyclePosition=1
periodSource=5 periodTimeline=6
sourcePosition=5815.833333
sourceFrameA=5815
sourceFrameB=5816
timelineTimeA=6978.000
timelineTimeB=6980.000
timestep=0.833333
signatureA=0x02921c816438e610
signatureB=0xc8d9d419005d2246
rifeInference=1
calibration=calibrated policy=interpolate identicalInputs=0 renderScale=1x1 sourceBounds=2560x1440
```

Et, une seule fois par instance :

```text
[RifeOFX] renderTimeProbe requestedTime=6975.000000 status=0 addressable=1 signature=0x...
[RifeOFX] cadenceProbe source=renderPath from=6975.000 length=13 pattern=AABCDEFFGHIJK distinct=11
```

`cadenceProbe` etiquette treize temps de rendu consecutifs par la premiere
occurrence de chaque image, a partir des images que le rendu a reellement
recues :

- `ABCDEFGHIJKLM`, `distinct=13` : une image differente par frame, entree non
  conformee ;
- `AABCDEFFGHIJK`, `distinct=11` : une image repetee tous les six temps, entree
  conformee, et la position de la repetition donne la phase.

## 7. Ce qu'il faut verifier

Sur soixante frames consecutives :

1. `timestep` suit le cycle `0, 0.8333, 0.6667, 0.5, 0.3333, 0.1667` ;
2. `timelineTimeA != timelineTimeB` des que `rifeInference=1` ;
3. `identicalInputs=0` sur toutes les frames interpolees ;
4. `sourceFrameA` avance de 50 sur 60 frames de timeline ;
5. les temps jamais demandes sont exactement les doublons de l'hote, tous a la
   meme position de cycle ;
6. `calibration=calibrated` apparait une fois et ne revient pas ;
7. aucune ligne `source does not cover the render window`.

## 8. Probe du contexte Retimer

Le contexte retimer n'est pas declare par defaut, pour ne pas perturber le test
principal. Pour l'activer :

```powershell
$env:RIFEOFX_PROBE_RETIMER = "1"
$env:RIFEOFX_DEBUG = "1"
& "C:\Program Files\Blackmagic Design\DaVinci Resolve\Resolve.exe"
```

Lignes a chercher :

```text
[RifeOFX] RIFEOFX_PROBE_RETIMER is set: also advertising kOfxImageEffectContextRetimer
[RifeOFX] describeInContext context=OfxImageEffectContextRetimer
[RifeOFX] createInstance context=OfxImageEffectContextRetimer
[RifeOFX] retimer SourceTime param status=0 present=1 retimerContext=1
```

- Si `describeInContext context=OfxImageEffectContextRetimer` n'apparait jamais,
  Resolve n'offre pas ce contexte aux plugins OFX tiers.
- S'il apparait, `SourceTime paramGetHandle status=0 present=1` confirme que le
  parametre mandate existe et que la position source pourrait venir de l'hote.

Resultat mesure : `describeInContext` n'est appele qu'avec `Filter`, et
`SourceTime` renvoie `status=3` (`kOfxStatErrUnknown`) dans tous les contextes.

## 9. Cas limites a couvrir pendant le test

- `timestep` exactement 0 : `policy=holdA`, `rifeInference=0`, une seule image
  demandee ;
- cadence non entiere (23.976, 29.97, 59.94) : le ratio doit se reduire
  exactement, `periodSource`/`periodTimeline` le montre ;
- cadences identiques : `calibration=notApplicable`, passthrough ;
- clip statique ou fondu : `calibration=ambiguous`, passthrough, jamais de
  supposition ;
- rendu proxy et pleine resolution sur le meme temps : `renderScale` doit
  apparaitre dans la trace et `source does not cover the render window` ne doit
  jamais apparaitre.

Le detail de chaque cas d'echec de calibration est dans [cadence.md](cadence.md).
