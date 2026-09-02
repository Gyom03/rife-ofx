# Audit OpenFX 1.5.1 et Resolve

Sources consultees :

- https://github.com/AcademySoftwareFoundation/openfx (tag `OFX_Release_1.5.1`)
- https://openfx.readthedocs.io/en/main/Reference/api/file/ofxImageEffect_8h.html
- https://openfx.readthedocs.io/en/latest/Reference/ofxImageClip.html

## Acces temporel

Le host annonce sa capacite via `kOfxImageEffectPropTemporalClipAccess`. Le plugin
doit egalement positionner cette propriete a `1` sur son descripteur et sur le clip
qui sera lu hors du temps courant.

Lorsque cette capacite est active, le host appelle
`kOfxImageEffectActionGetFramesNeeded`. Le plugin remplit une propriete nommee
`OfxImageClipPropFrameRange_Source`. Cette propriete contient une liste de paires
`[debut, fin]`, donc elle peut decrire une plage continue ou plusieurs plages
discontinues. Une fenetre `F[n-1]..F[n+2]` peut etre demandee comme une plage
continue si le pas temporel est connu.

Pendant `kOfxImageEffectActionRender`, le plugin peut ensuite appeler
`OfxImageEffectSuiteV1::clipGetImage(clip, time, region, &image)` avec un autre
`time`. Le handle retourne doit etre libere avec `clipReleaseImage` avant la fin de
l'action. Une image ne peut pas etre conservee apres le retour de l'action.

OFX ne fournit pas une API semantique `previousFrame`/`nextFrame`. Le futur
`TemporalFrameProvider` devra convertir un offset en timestamp, tenir compte du
frame rate/frame step et des bornes `kOfxImageEffectPropFrameRange`. Une image peut
ne pas exister aux bornes : le statut retourne par le host doit etre propage, pas
remplace silencieusement par une autre frame.

## Frame ranges

`kOfxImageEffectActionGetTimeDomain` concerne le domaine temporel produit par un
effet et n'est appelable que dans le contexte General. Ce n'est pas le mecanisme
principal pour declarer les frames d'entree necessaires a un filtre temporel.

`kOfxImageEffectActionGetFramesNeeded` est le mecanisme approprie pour le filtre.
Le host initialise normalement la plage au temps courant ; le plugin doit repondre
avec les plages additionnelles requises.

## Formats pixels

Le prototype declare uniquement `kOfxImageComponentRGBA` et `kOfxBitDepthFloat` sur
le plugin, Source et Output. Il ne declare pas le support de profondeurs mixtes.
Le rendu verifie les proprietes d'image, le row stride et les bounds, puis copie
uniquement l'intersection de la render window avec les deux images.

Les formats byte et short necessitent des chemins de copie ou de conversion
explicites. Ils ne sont pas ajoutes avant d'avoir observe le format que Resolve
envoie effectivement a ce plugin.

## Threading et lifecycle

Le prototype annonce `kOfxImageEffectRenderInstanceSafe` et permet le frame
threading du host. Les handles d'images sont acquis/liberes dans Render. Les clips
et le parametre sont acquis dans CreateInstance, puis l'etat d'instance est libere
dans DestroyInstance.

Pour RIFE, cette politique restera conservatrice tant que le contexte NCNN/Vulkan
et les buffers partages n'auront pas ete proteges. `BeginSequenceRender` et
`EndSequenceRender` seront les points de creation/liberation des ressources de
sequence si l'implementation RIFE le justifie.

## GPU et Vulkan

Les suites GPU standard OpenFX concernent OpenCL, CUDA, Metal et OpenGL selon les
versions. OFX 1.5.1 ne definit pas de suite Vulkan standard. Un moteur NCNN/Vulkan
devra donc gerer son propre device/context et ses synchronisations, avec des copies
explicites depuis/vers les images OFX tant qu'une interop specifique a Resolve n'est
pas demontree.

## Resolve Free

La documentation publique de Resolve ne fournit pas de matrice complete des
capacites OFX Free/Studio, en particulier pour `TemporalClipAccess` et le GPU.
Il n'est donc pas possible de declarer cette compatibilite a partir du SDK seul.
Le test de l'etape 1 valide l'enregistrement d'un OFX tiers dans Resolve Free ;
l'etape 2 validera separement les appels temporels. Une limitation propre a Free
sera signalee si Resolve refuse le bundle, le contexte, ou les appels temporels.
