# Support des modeles

## Etat valide dans ce build

Le registre et l'Inspector exposent six choix, mais seuls ces adaptateurs sont
actuellement valides :

- `rife-v4.6` : valide avec les poids deja fournis par la source NCNN/Vulkan.
- `rife-v4.25` : valide avec les poids NCNN testes dans ce projet.
- `rife-v4.25-lite` : valide avec les poids NCNN testes dans ce projet.
- `rife-v4.22-lite` : valide avec les poids NCNN testes dans ce projet.

Les variantes 4.26 et 4.26 Large sont activees pour validation dans Resolve
apres le smoke test.
l'UI, mais le backend les refuse explicitement tant qu'elles n'ont pas subi un
test equivalent. Elles ne sont pas des alias silencieux vers 4.6 ou 4.25.

## RIFE 4.25 teste

Le prototype utilise le format NCNN classique :

```text
rife-v4.25/flownet.param
rife-v4.25/flownet.bin
```

Les fichiers de test proviennent du dossier `rife-v4.25_ensembleFalse` du
projet VapourSynth-RIFE-ncnn-Vulkan de styler00dollar. Le code du plugin reste
base sur `nihui/rife-ncnn-vulkan`; le smoke test a charge le modele 4.25 avec
Vulkan sur la RTX 3080 et a reussi une inference a timestep 0.5.

Le descriptor 4.25 declare un alignement de 64. L'adaptateur repete les pixels
de bord pour atteindre l'alignement, execute le modele, puis recadre la sortie
a la resolution OFX originale. Le smoke test utilise 130x66 : 4.25 standard
produit 192x128 en entree RIFE.

## Poids externes et licence

La licence MIT des depots de code ne constitue pas une licence de redistribution
des poids. Aucune licence separee et explicite des poids 4.25 n'a ete verifiee.
Ils ne sont donc pas copies dans le bundle ni dans le package distributable.
Ils doivent etre installes par l'utilisateur depuis leur source, selon les
conditions applicables.

Le plugin lit le chemin externe depuis la variable utilisateur Windows
`RIFEOFX_MODELS_ROOT`. Le manifeste `registry.csv` reste dans le bundle.

Exemple local :

```powershell
$env:RIFEOFX_MODELS_ROOT = "C:\Users\guill\Desktop\RIFE\external-models"
```

Ou, pour le conserver pour les prochaines sessions :

```powershell
Start-Process ".\install_models.bat" -Wait
```

Le script ne telecharge pas et ne redistribue pas les poids.

## Differences connues

- `rife-v4.25` : padding 64, timestep arbitraire, chemin NCNN `flownet.*`.
- `rife-v4.25-lite` : padding 128 selon l'implementation de reference; le
  smoke test 130x66 produit 256x128 en entree RIFE.
- `rife-v4.22-lite` : padding 128 selon l'implementation de reference; le
  backend NCNN/Vulkan est active pour validation.
- `rife-v4.26` et `rife-v4.26-large` : padding 64 selon l'implementation de
  reference; backend active pour validation.
- `rife-v4.26` et `rife-v4.26-large` : padding 64 selon la meme reference,
  mais poids et compatibilite backend encore a valider.
- les modes ensemble ne sont pas actives dans ce prototype.

Sources techniques consultees :

- https://github.com/nihui/rife-ncnn-vulkan
- https://github.com/TNTwise/rife-ncnn-vulkan
- https://github.com/styler00dollar/VapourSynth-RIFE-ncnn-Vulkan
