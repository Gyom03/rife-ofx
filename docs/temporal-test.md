# Test de l'etape temporelle

## Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

Installer ensuite le bundle en administrateur :

```powershell
Start-Process -FilePath ".\install_plugin.bat" -Verb RunAs -Wait
```

Fermer et relancer Resolve apres chaque nouvelle installation.

## Test dans Resolve

1. Creer une timeline de test en 60 fps.
2. Importer une source connue en 50 fps.
3. Ajouter `RIFE Frame Interpolator` a la source.
4. Verifier les parametres `Enabled`, `Interpolation Amount` et `Debug`.
5. Placer la tete de lecture au milieu du clip, pas sur la premiere frame.
6. Mettre `Debug` a `true`.
7. Tester `Interpolation Amount` a `0.0`, `0.5`, puis `1.0`.

Le code demande la fenetre suivante pour le temps `t` :

```text
F[n-1] = t-1
F[n]   = t
F[n+1] = t+1
F[n+2] = t+2
```

Dans DebugView, filtrer les lignes commencant par `[RifeOFX]`. On doit voir des
appels `clipGetImage` pour les quatre timestamps. Les valeurs exactes dependent de
la convention temporelle de Resolve.

`Amount=0` rend la frame precedente, `Amount=1` la frame suivante et `Amount=0.5`
leur blend. Ce n'est pas encore une interpolation RIFE et cela ne change pas a lui
seul la cadence de la timeline. La timeline doit demander des temps intermediaires
pour produire davantage d'images.

Sur les premieres/dernieres frames, une voisine peut ne pas exister. Le prototype
retourne alors explicitement le statut d'erreur du host au lieu de dupliquer
silencieusement une autre frame.
