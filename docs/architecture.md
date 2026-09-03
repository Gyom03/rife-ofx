# Architecture par etapes

## Chaine temporelle

Le plugin est un effet temporel OpenFX : il ne se contente pas de traiter la
frame que Resolve lui presente au temps courant. Pour chaque temps de sortie il
recalcule la position correspondante dans le media source, puis demande
explicitement a l'hote les deux frames source qui l'encadrent.

```text
Media source 50 fps
  frame 0 ---- frame 1 ---- frame 2 ---- frame 3
   0 ms         20 ms        40 ms        60 ms
                  ^
                  |  clipGetImage(sourceTimeA / sourceTimeB)
                  |
        computeTemporalMapping
                  |
                  v
Timeline 60 fps
  0 ms      16.667      33.333      50 ms
```

Le decoupage suit trois couches, une par responsabilite :

```text
buildTemporalMapping(instance, outputTime)  -> TemporalMapping
    src/TemporalMapping.{h,cpp}   maths pur, sans suite OFX, teste hors hote

fetchTemporalInputs(mapping)                -> TemporalInputs
    src/TemporalFrameProvider.{h,cpp}   clipGetImage aux temps demandes

runRifeInterpolation(frameA, frameB, timestep)
    src/RifeEngine.{h,cpp}        inference, inchangee par ce refactor
```

`render()` ne fait plus que enchainer ces trois couches puis recopier le
resultat dans l'image de sortie.

## TemporalMapping

```cpp
struct TemporalMapping {
  OfxTime      outputTime;
  int64_t      outputFrame;
  double       sourceFrameRate, outputFrameRate;
  double       sourceSeconds;      // (outputTime - origine) / outputFPS
  double       sourcePosition;     // sourceSeconds * sourceFPS
  int64_t      sourceFrameA, sourceFrameB;
  OfxTime      sourceTimeA, sourceTimeB;
  float        timestep;           // sourcePosition - floor(sourcePosition)
  BlendPolicy  policy;             // holdA | holdB | interpolate
  ...
};
```

`sourcePosition` est calcule a partir de la cadence reelle des deux cotes,
jamais d'une valeur codee en dur. `sourceFrameA = floor(sourcePosition)`,
`sourceFrameB = sourceFrameA + 1`, `timestep = sourcePosition - sourceFrameA`.

### Cadence conformee (mode par defaut)

Le mode par defaut est `Conformed Cadence (auto phase)`. Il constate que l'hote
a deja reechantillonne le clip et adresse les images originales dans le flux
conforme, au lieu de supposer un axe temporel source separe. La phase du conform
est mesuree une fois par comparaison de signatures, puis le mapping est
entierement arithmetique sur un ratio rationnel. Voir [cadence.md](cadence.md).

Les deux modes ci-dessous restent disponibles pour le diagnostic.

### Index de frame vers OfxTime

OpenFX ne definit pas de correspondance universelle entre un index de frame
source et un `OfxTime`. La conversion est donc une politique explicite,
`SourceTimeBase`, exposee dans l'interface :

- `Source Frames (OFX standard)` : une unite de temps par frame source, comme
  l'exemple retimer du SDK (`Support/Plugins/Retimer/retimer.cpp` fait
  `fetchImage(floor(sourceTime))`). Les requetes sont toujours entieres.
- `Timeline Frames (host conformed)` : la frame source `N` est a
  `origine + N * outputFPS / sourceFPS`. Les requetes deviennent
  fractionnaires. Ce mode sert a tester un hote qui reechantillonne le clip
  d'entree sur la cadence de sortie avant que l'effet ne le voie.

L'origine (`timelineOrigin`) est le temps OFX du debut du clip. Elle est choisie
en testant le **temps de rendu** contre les plages annoncees, jamais une valeur
derivee : `kOfxImageEffectPropFrameRange` du clip Source si `kOfxPropTime` tombe
dedans (le clamp de bord est alors legitime), sinon celle du clip Output, sinon
ancrage sur le temps de rendu lui-meme (degradation en passthrough, signalee par
`anchor=unresolved`). Resolve peut annoncer une plage Source media-locale
(`0..1999`) alors que `kOfxPropTime` est global a la composition (`6816`) :
utiliser une plage venant d'un autre axe figerait chaque frame sur la meme
image.

### Bords

`computeTemporalMapping` traite explicitement : position avant la premiere frame
media (`clampedAtStart`), voisine apres la derniere (`clampedAtEnd`), `timestep`
proche de 0 ou 1 (`holdA` / `holdB`, aucune inference), cadences non entieres
(23.976, 29.97, 59.94) via un snap a tolerance relative. Le clamp n'est applique
que si la plage source annoncee est sur le meme axe que le temps de rendu ; une
plage media-locale ne borne jamais un temps de composition global.

`src/TemporalMappingTest.cpp` couvre ces cas sans hote OFX ni Vulkan
(`ctest --test-dir build-rife -C Release`).

## Acces temporel declare

- `kOfxImageEffectPropTemporalClipAccess = 1` sur le descripteur du plugin dans
  `describe()`, avec relecture de la propriete et log du resultat ;
- meme propriete sur le clip `Source` dans `describeInContext()` ;
- `kOfxImageEffectActionGetFramesNeeded` annonce `[sourceTimeA, sourceTimeB]`
  dans `OfxImageClipPropFrameRange_Source`, reduit a une seule valeur quand la
  politique est `holdA` / `holdB` ou pour un rendu vignette
  (`kOfxImageEffectPropThumbnailRender`).

## Contexte Retimer

`kOfxImageEffectContextRetimer` existe dans le SDK. Il n'est pas declare par
defaut : la variable d'environnement `RIFEOFX_PROBE_RETIMER=1` l'ajoute comme
contexte secondaire pour observer si l'hote le propose. Le parametre mandate
`SourceTime` est alors defini, et `buildTemporalMapping` l'utilise comme
position source a la place du calcul de cadence (`hostProvidedPosition=1` dans
la trace). Voir `docs/ofx-audit.md`.

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

`runRifeInterpolation()` est le seul point d'entree du moteur depuis le plugin :
ajouter un modele ou une strategie d'interpolation ne touche pas la couche
temporelle.

## Adaptateur valide

RIFE 4.25 reutilise le pipeline NCNN/Vulkan RIFE v4, mais son alignment est
different de celui de v4.6. `RifeNcnnBackend` applique l'alignement du
descriptor avant `RIFE::process`, puis recadre la sortie. Cette logique ne
depend pas d'un chemin de modele ou d'un nom de version hardcode.

Le prototype valide 4.6, 4.25, 4.25 Lite et 4.22 Lite. Les variantes 4.26 et
4.26 Large sont activees pour validation apres smoke test; les choix non
valides restent refuses avec un message `model adapter not validated yet`.

## Diagnostics

`src/DebugLog.{h,cpp}` centralise la trace. Tout passe par `OutputDebugStringA`
avec le prefixe `[RifeOFX]`, et est double dans
`%TEMP%\RifeOFX-temporal.log`. `RIFEOFX_DEBUG=1` force la trace avant meme que
le parametre `Debug` ne soit accessible.

La trace contient, par frame de sortie : le bloc `TemporalMapping` complet, les
deux `fetchImage ... requestedTime=`, le statut et les bounds renvoyes par
`clipGetImage`, et une signature echantillonnee des deux images
(`frameA_debug_signature`, `frameB_debug_signature`, `identicalImages`). La
signature est volontairement partielle (4096 echantillons maximum) : elle sert a
detecter une repetition renvoyee par l'hote, pas a hacher l'image.
