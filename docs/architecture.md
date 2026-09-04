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

### Cadence conformee

Resolve reechantillonne le clip sur la cadence de la timeline avant que l'effet
ne le voie. Le plugin adresse donc les images originales **dans le flux
conforme** : la phase du conform est mesuree une fois par comparaison de
signatures, puis le mapping est entierement arithmetique sur un ratio rationnel.
Voir [cadence.md](cadence.md).

C'est le seul mode. Deux politiques alternatives ont existe, `Source Frames` et
`Timeline Frames`, construites sur l'hypothese que l'hote exposerait un axe
temporel source distinct. Elles ont ete retirees : Resolve n'expose nulle part
le debut du clip sur l'axe de rendu, donc elles etaient soit inertes, soit
actives avec une origine erronee, ce qui ralentissait silencieusement le clip.
L'historique de cette verification reste dans [ofx-audit.md](ofx-audit.md).

### Bords

Les bords sont traites dans la couche cadence : voir la section « Cas ou la
calibration echoue » de [cadence.md](cadence.md). Quand la phase ne peut pas
etre determinee, le rendu est un hold sur le temps courant, jamais une
supposition.

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
`SourceTime` est alors defini et lu. Resultat mesure : Resolve n'appelle jamais
`describeInContext` avec ce contexte, et `SourceTime` est inconnu dans tous les
contextes. Voir [ofx-audit.md](ofx-audit.md).

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
