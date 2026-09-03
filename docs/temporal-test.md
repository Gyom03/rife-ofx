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
5. Laisser `Source Time Base` sur `Source Frames (OFX standard)`.
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

## 5. Logs attendus

Au chargement :

```text
[RifeOFX] host name=DaVinciResolve
[RifeOFX] host temporalClipAccess=1
[RifeOFX] plugin temporalClipAccess setStatus=0 readBackStatus=0 value=1
[RifeOFX] describeInContext context=OfxImageEffectContextFilter
[RifeOFX] source clip temporalClipAccess setStatus=0 readBackStatus=0 value=1
[RifeOFX] retimer SourceTime param status=... present=0 retimerContext=0
[RifeOFX] clip timing sourceConnected=1 sourceFrameRate(mapped)=... \
          sourceFrameRate(unmapped)=... outputFrameRate(mapped)=... \
          projectFrameRate=... sourceFrameRange=... outputFrameRange=... \
          continuousSamples=0
```

Pour chaque frame de sortie :

```text
[RifeOFX]
outputTime=123.000
outputFrame=123
sourceFPS=50.000
outputFPS=60.000
sourcePosition=102.500000
timelineOrigin=0.000
sourceFrameA=102
sourceFrameB=103
sourceTimeA=102.000000
sourceTimeB=103.000000
timestep=0.500000
timeBase=sourceFrames
policy=interpolate
anchor=sourceFrameRange anchorResolved=1 clampEnabled=1
clampedAtStart=0 clampedAtEnd=0 ratesValid=1 hostProvidedPosition=0

[RifeOFX] fetchImage A requestedTime=102.000000
[RifeOFX] clipGetImage requestedTime=102.000000 status=0 bounds=[0,0,1920,1080] rowBytes=30720
[RifeOFX] fetchImage B requestedTime=103.000000
[RifeOFX] clipGetImage requestedTime=103.000000 status=0 bounds=[0,0,1920,1080] rowBytes=30720
[RifeOFX] frameA_debug_signature=0x1f3a... frameB_debug_signature=0x90c7... identicalImages=0
```

Et, en amont du rendu :

```text
[RifeOFX] getFramesNeeded outputTime=123.000000 sourceRange=[102.000000,103.000000] policy=interpolate thumbnail=0 status=0
```

Et, une seule fois par instance, la mesure qui tranche la question de l'axe :

```text
[RifeOFX] renderTimeProbe requestedTime=6816.000000 status=0 addressable=1 signature=0x... matchesMappedFrameA=0 mappedTimeA=6811.000000
[RifeOFX] cadenceProbe from=6384.000 length=13 pattern=AABCDEFFGHIJK distinct=11
```

`cadenceProbe` etiquette treize temps de rendu consecutifs par la premiere
occurrence de chaque image :

- `ABCDEFGHIJKLM`, `distinct=13` : chaque frame de la timeline porte une image
  differente, l'entree n'est pas conformee ;
- `AABCDEFFGHIJK`, `distinct=11` : une image est repetee tous les six temps.
  L'hote a conforme le media 50 -> 60 avant que l'effet ne le voie, et la
  position de la repetition donne la phase du conform.

## 6. Ancrage temporel

`timelineOrigin` est le temps OFX qui correspond a la position source 0, c'est a
dire le debut du clip sur l'axe utilise par `kOfxPropTime`. Il est choisi a
partir du temps de rendu, pas d'une valeur derivee :

| `anchor=` | Condition | Clamp |
|-----------|-----------|-------|
| `sourceFrameRange` | `outputTime` tombe dans la plage du clip Source | oui |
| `outputFrameRange` | `outputTime` tombe dans la plage du clip Output | non |
| `unresolved ...` | aucune plage annoncee ne contient `outputTime` | non |

Resolve peut annoncer une plage Source media-locale (`0..1999`) alors que le
temps de rendu est global a la composition (`6816`). Une plage venant d'un autre
axe ne doit jamais localiser le clip ni borner une requete : cela figerait
chaque frame sur la meme image. Quand aucune plage ne convient, l'ancrage se
fait sur le temps de rendu lui-meme, ce qui degrade en passthrough plutot que de
demander une frame source a plusieurs centaines d'images du curseur. La ligne
`anchor=unresolved ...` signale ce cas.

`renderTimeProbe` demande explicitement l'image au temps de rendu brut :

- `addressable=1` : le clip d'entree est adressable sur l'axe du rendu, donc une
  plage media-locale est bien a ignorer, et `outputFrameRange` est le bon
  ancrage ;
- `addressable=0` : le clip est adresse sur un autre axe, et il faudra
  determiner la correspondance timeline -> media avant d'aller plus loin ;
- `matchesMappedFrameA=1` avec un `mappedTimeA` different du temps de rendu :
  l'hote renvoie la meme image pour deux temps distincts, donc le clip est
  probablement deja conforme.

## 7. Ce qu'il faut verifier

Sur six frames de sortie consecutives en 50 -> 60, `sourcePosition` doit suivre
le cycle :

```text
0.0000  0.8333  1.6667  2.5000  3.3333  4.1667  5.0000 ...
```

et donc `timestep` :

```text
0.0000  0.8333  0.6667  0.5000  0.3333  0.1667  0.0000 ...
```

`sourceFrameA` doit avancer 0, 0, 1, 2, 3, 4, 5 : cinq frames source pour six
frames de sortie.

### Cas A : l'acces temporel fonctionne

`frameA_debug_signature` et `frameB_debug_signature` different a chaque frame ou
`policy=interpolate`, et la signature de la frame source `N` est la meme quel
que soit le temps de sortie qui la demande. Resolve nous donne bien les frames
originales et le plugin possede la conversion 50 -> 60.

### Cas B : Resolve renvoie des frames deja conformees

Symptomes : `identicalImages=1` sur des paires ou `timestep` est franchement
fractionnaire, ou bien la signature obtenue pour `requestedTime=102` change
selon le temps de sortie qui l'a demandee.

Dans ce cas, **ne pas ajouter de deduplication**. Marche a suivre :

1. Conserver `%TEMP%\RifeOFX-temporal.log` tel quel.
2. Rejouer le meme test avec `Source Time Base = Timeline Frames
   (host conformed)`. Ce mode demande les temps `origine + N * 60/50`, donc des
   temps fractionnaires : il repond a la question "Resolve echantillonne-t-il le
   clip d'entree sur l'axe de la timeline ?".
3. Rejouer avec le probe retimer (section suivante) pour savoir si Resolve
   expose `kOfxImageEffectContextRetimer`.
4. Reporter les trois traces dans `docs/ofx-audit.md`.

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
- S'il apparait, `hostProvidedPosition=1` dans la trace de rendu indique que la
  position source vient de `SourceTime` fourni par l'hote, et non plus de notre
  calcul de cadence.

## 9. Cas limites a couvrir pendant le test

- premiere frame du clip : `clampedAtStart=1`, `policy=holdA`, une seule image ;
- derniere frame du clip : `clampedAtEnd=1`, `policy=holdA` ;
- `timestep` proche de 0 ou 1 : `policy=holdA` / `holdB`, aucune inference ;
- cadence non entiere (23.976, 29.97, 59.94) : `sourcePosition` doit rester
  exactement entier quand source et sortie ont la meme cadence ;
- media trimme : verifier `sourceFrameRange`, `timelineOrigin` et `anchor=` dans
  le log. Si le temps de rendu tombe hors de la plage source annoncee, le clamp
  est desactive et signale : c'est volontaire, une plage media-locale ne doit pas
  borner un temps de composition global.
