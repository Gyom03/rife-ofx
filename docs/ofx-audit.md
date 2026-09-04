# Audit OpenFX 1.5.1 et Resolve

SDK utilise par le projet : `AcademySoftwareFoundation/openfx`, tag
`OFX_Release_1.5.1`, commit `ab77951`. Il est recupere par `FetchContent` dans
`build-rife/_deps/openfx-src` (voir `CMakeLists.txt`). Seuls les headers C sont
consommes : la bibliotheque de support C++ (`Support/`) n'est pas liee, donc il
n'y a ni `OFX::ImageEffect`, ni `srcClip->fetchImage(time)`, ni
`FramesNeededSetter`. Tout passe par `OfxImageEffectSuiteV1` et
`OfxPropertySuiteV1`.

Correspondance entre l'API C++ souvent citee et ce que le projet utilise
reellement :

| API support C++                          | Equivalent C utilise ici |
|------------------------------------------|--------------------------|
| `desc.setTemporalClipAccess(true)`        | `propSetInt(effectProps, kOfxImageEffectPropTemporalClipAccess, 0, 1)` |
| `srcClip->setTemporalClipAccess(true)`    | meme propriete sur le property set retourne par `clipDefine` |
| `getFramesNeeded(args, frames)`           | action `kOfxImageEffectActionGetFramesNeeded` |
| `frames.setFramesNeeded(*srcClip, range)` | `propSetDoubleN(outArgs, "OfxImageClipPropFrameRange_Source", 2, range)` |
| `srcClip->fetchImage(time)`               | `gImageEffectSuite->clipGetImage(clip, time, nullptr, &image)` |
| `srcClip->getFrameRange()`                | `propGetDoubleN(clipProps, kOfxImageEffectPropFrameRange, 2, range)` |
| `srcClip->getFrameRate()`                 | `propGetDouble(clipProps, kOfxImageEffectPropFrameRate, 0, &rate)` |

## Acces temporel

Le host annonce sa capacite via `kOfxImageEffectPropTemporalClipAccess`. Le
plugin doit egalement positionner cette propriete a `1` sur son descripteur et
sur le clip qui sera lu hors du temps courant. Les deux sont faits, et relus
juste apres l'ecriture pour verifier que l'hote a bien accepte la valeur :

```text
[RifeOFX] plugin temporalClipAccess setStatus=0 readBackStatus=0 value=1
[RifeOFX] source clip temporalClipAccess setStatus=0 readBackStatus=0 value=1
```

Lorsque cette capacite est active, le host appelle
`kOfxImageEffectActionGetFramesNeeded` (`ofxImageEffect.h:284`). Le plugin
remplit une propriete nommee `OfxImageClipPropFrameRange_Source` : une liste de
paires `[debut, fin]`, donc une ou plusieurs plages continues. Ce nom litteral
est celui documente dans le header, il n'existe pas de constante `#define` pour
le prefixe.

Pendant `kOfxImageEffectActionRender`, le plugin appelle
`OfxImageEffectSuiteV1::clipGetImage(clip, time, region, &image)` avec un autre
`time`. Le handle retourne doit etre libere avec `clipReleaseImage` avant la fin
de l'action; une image ne peut pas etre conservee au dela.

OFX ne fournit pas d'API semantique `previousFrame`/`nextFrame`, et ne definit
pas non plus la correspondance entre un index de frame source et un `OfxTime`.
Le seul point de reference du SDK est l'exemple retimer
(`Support/Plugins/Retimer/retimer.cpp:147-152`), qui fait
`fetchImage(floor(sourceTime))` puis `fetchImage(floor(sourceTime) + 1)` : une
unite de temps par frame source.

Deux politiques ont ete implementees et testees sur cette base, `Source Frames`
et `Timeline Frames`. Les deux ont ete retirees : elles supposent que l'hote
expose le debut du clip sur l'axe de rendu, et Resolve ne le fait pas. Le plugin
adresse desormais les originales dans le flux conforme, voir
[cadence.md](cadence.md).

Une image peut ne pas exister aux bornes. Le statut retourne par l'hote est
propage et journalise (`fetchImage B unavailable ... status=...`), puis la
politique de bord tient la frame A. Aucune substitution silencieuse.

## Frame ranges

`kOfxImageEffectActionGetTimeDomain` concerne le domaine temporel produit par un
effet et n'est appelable que dans le contexte General. Ce n'est pas le mecanisme
pour declarer les frames d'entree necessaires a un filtre temporel.

`kOfxImageEffectActionGetFramesNeeded` est le mecanisme approprie. Le host
initialise la plage au temps courant; le plugin repond avec les plages requises.

Les proprietes de plage lues sur les clips :

- `kOfxImageEffectPropFrameRange` (Source et Output) ;
- `kOfxImageEffectPropUnmappedFrameRange` (Source), pour voir si l'hote a deja
  remappe la plage.

Resolve peut exposer une valeur sentinelle proche de `INT_MIN`/`INT_MAX` pour
une source Fusion non bornee. `isUsableFrameRange()` rejette ces valeurs : elles
ne doivent jamais servir a borner une requete temporelle.

## Frame rates

Proprietes lues et journalisees telles quelles, sans arbitrage cache :

| Propriete | Property set | Signification |
|-----------|--------------|---------------|
| `kOfxImageEffectPropUnmappedFrameRate` | clip Source | cadence originale du media |
| `kOfxImageEffectPropFrameRate` | clip Source | cadence apres mapping eventuel par l'hote |
| `kOfxImageEffectPropFrameRate` | clip Output | cadence de sortie mappee |
| `kOfxImageEffectPropFrameRate` | instance d'effet | cadence du projet |
| `kOfxImageClipPropContinuousSamples` | clip Source | le clip accepte-t-il un echantillonnage continu |
| `kOfxImageClipPropConnected` | clip Source | clip reellement connecte |

Il n'existe pas de propriete « project fps » distincte en OFX 1.5.1 : sur une
instance d'effet, `kOfxImageEffectPropFrameRate` **est** la cadence du projet
(`ofxImageEffect.h:1067`).

Pour la cadence du media, `kOfxImageEffectPropUnmappedFrameRate` est prioritaire
sur `kOfxImageEffectPropFrameRate`, car Resolve peut exposer la seconde deja
mappee sur la timeline. Si les deux valent la cadence timeline, le parametre
`Source Framerate` permet un override manuel explicite : le plugin ne pretend
pas deduire 50 fps a partir d'une valeur OFX qui vaut 60.

## Contexte Retimer

`kOfxImageEffectContextRetimer` est bien present dans le SDK :

- `include/ofxImageEffect.h:72` — `#define kOfxImageEffectContextRetimer "OfxImageEffectContextRetimer"`
- `include/ofxImageEffect.h:1478` — `#define kOfxImageEffectRetimerParamName "SourceTime"`
- `include/ofxImageEffect.h:137-139` — la RoD par defaut du contexte retimer est
  l'union de la RoD de `Source` a la frame precedant `SourceTime` et de celle de
  la frame suivante.
- `Support/Plugins/Retimer/retimer.cpp` — exemple complet.

Modele du contexte :

```text
temps de sortie
      |
      v
SourceTime (double, fourni par l'hote, exprime en frames source)
      |
      v
floor(SourceTime) et floor(SourceTime) + 1
      |
      v
RIFE(frameA, frameB, SourceTime - floor(SourceTime))
```

Le plugin **definit** le parametre `SourceTime` dans `describeInContext` mais ne
l'ecrit jamais : il n'apparait dans aucune UI, l'hote en est seul proprietaire.

### Etat de la verification cote Resolve

Le SDK contient le contexte. La documentation publique de Blackmagic ne dit pas
si Resolve l'expose aux plugins OFX tiers, et cela ne peut pas etre deduit des
headers. Le plugin ne le declare donc pas par defaut : `RIFEOFX_PROBE_RETIMER=1`
l'ajoute comme contexte secondaire, `Filter` restant a l'index 0 pour qu'un hote
qui supporte les deux continue d'utiliser le filtre.

La reponse experimentale est la ligne :

```text
[RifeOFX] describeInContext context=OfxImageEffectContextRetimer
```

Si elle n'apparait jamais, Resolve ne propose pas ce contexte. Si elle apparait
et qu'une instance est creee, `SourceTime paramGetHandle status=0 present=1` le
confirme.

Resultat mesure : la propriete est acceptee (`dimension=2 [0]=Filter
[1]=Retimer`), mais `describeInContext` n'est appele qu'avec `Filter`, et
`SourceTime` renvoie `status=3` (`kOfxStatErrUnknown`) dans tous les contextes.
Resolve ignore silencieusement l'entree Retimer.

Ce qu'il faudrait changer pour devenir un vrai retimer, si Resolve le supporte :

1. declarer `kOfxImageEffectContextRetimer` en premier dans
   `kOfxImageEffectPropSupportedContexts` ;
2. supprimer les parametres `Source Framerate`, `Target Framerate`,
   et `Use Timeline Framerate` du contexte retimer : la
   cadence n'est plus notre affaire, l'hote fournit directement la position ;
3. ne plus publier `kOfxImageEffectPropFrameRate` dans
   `getClipPreferences` — c'est l'hote qui decide de la duree de sortie ;
4. `getFramesNeeded` et `render` lisent `SourceTime` a `args.time` (le chemin est
   deja en place via `hostSourcePosition`) ;
5. implementer `kOfxImageEffectActionGetRegionOfDefinition` si la RoD par defaut
   du contexte ne convient pas.

Le point 4 est deja code, c'est pourquoi le probe suffit a trancher.

## Formats pixels

Le prototype declare uniquement `kOfxImageComponentRGBA` et `kOfxBitDepthFloat`
sur le plugin, Source et Output. Il ne declare pas le support de profondeurs
mixtes. Le rendu verifie les proprietes d'image, le row stride et les bounds,
puis copie uniquement l'intersection de la render window avec l'image source.

Les deux images temporelles doivent avoir la meme geometrie pour etre melangees.
Une divergence de bounds entre A et B est detectee, journalisee et resolue en
tenant la frame A, jamais en melangeant des buffers de tailles differentes.

Les formats byte et short necessitent des chemins de copie ou de conversion
explicites. Ils ne sont pas ajoutes avant d'avoir observe le format que Resolve
envoie effectivement a ce plugin.

## Threading et lifecycle

Le prototype annonce `kOfxImageEffectRenderInstanceSafe` et permet le frame
threading du host. Les handles d'images sont acquis/liberes dans Render. Les
clips et les parametres sont acquis dans CreateInstance, l'etat d'instance est
libere dans DestroyInstance.

`TemporalFrameProvider` possede un cache mutable; les rendus d'une meme instance
sont serialises par un mutex, les instances independantes restent paralleles.
`kOfxActionInstanceChanged` de type `kOfxTypeClip` relit la cadence et vide le
cache : un changement de source ne peut pas laisser une cadence perimee.

## GPU et Vulkan

Les suites GPU standard OpenFX concernent OpenCL, CUDA, Metal et OpenGL selon
les versions. OFX 1.5.1 ne definit pas de suite Vulkan standard. Un moteur
NCNN/Vulkan gere donc son propre device/context et ses synchronisations, avec
des copies explicites depuis/vers les images OFX.

## Resolve Free

La documentation publique de Resolve ne fournit pas de matrice complete des
capacites OFX Free/Studio, en particulier pour `TemporalClipAccess` et le GPU.
Cette compatibilite ne peut donc pas etre declaree a partir du SDK seul. La
ligne `host temporalClipAccess=...` journalisee dans `describe()` donne la
reponse de l'hote reellement utilise.
