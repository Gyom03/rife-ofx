# Architecture cadence

## Le probleme

Resolve reechantillonne un clip dont la cadence differe de la timeline **avant**
que l'effet ne le voie. Pour 50 fps sur une timeline 60 fps, six frames de
timeline portent cinq images originales, dont une repetee.

Mesure sur le chemin de rendu (log Resolve, clip 50 fps / timeline 60 fps) :

```text
7003  sigA=0xa33e63a56f06
7004  sigA=0xa33e63a56f06   <- doublon
7009  sigA=0xe832df075eee
7010  sigA=0xe832df075eee   <- doublon
```

Les cinq images originales sont toutes presentes. Il ne s'agit donc pas de les
reconstruire, mais de les **adresser** : quel temps de timeline porte l'image
originale `k`.

Les autres pistes sont fermees :

- `kOfxImageEffectContextRetimer` : Resolve n'appelle jamais `describeInContext`
  avec ce contexte, et `SourceTime` n'existe dans aucun contexte
  (`status=3`, `kOfxStatErrUnknown`). Voir `docs/ofx-audit.md`.
- Metadonnees d'hote : toutes les plages sont clip-locales (`0..1999`) alors que
  `kOfxPropTime` est global a la composition (`6975`, `7001`). Le debut du clip
  sur la timeline n'est exposable par aucune propriete.

## Le ratio

`cadenceRatio(sourceFPS, timelineFPS)` reduit `sourceFPS / timelineFPS` en une
fraction irreductible `num/den` :

| source | timeline | ratio | cycle |
|--------|----------|-------|-------|
| 50 | 60 | 5/6 | 5 originales pour 6 frames timeline |
| 23.976 | 59.94 | 2/5 | 2 pour 5 |
| 29.97 | 59.94 | 1/2 | 1 pour 2 |
| 25 | 60 | 5/12 | 5 pour 12 |
| 60 | 60 | 1/1 | aucun reechantillonnage |

Les cadences broadcast sont reconnues comme `k*1000/1001`, y compris quand
l'utilisateur saisit la valeur arrondie `23.976`. Rien n'est code en dur : ni
50, ni 60, ni la periode.

## La calibration de phase

Seule inconnue restante : la **phase** du conform, c'est a dire la position du
doublon dans le cycle. C'est la seule quantite mesuree, et elle l'est une fois.

`CadenceCalibrator` collecte, pendant que l'etat est `pending`, les signatures
des images que le chemin de rendu recoit aux temps consecutifs. Le plugin est
alors en hold sur le temps de rendu, donc la signature observee est bien celle
que l'hote renvoie pour cette frame de timeline.

Avec `u = t - phase` et `m(u) = floor(num*u/den)`, un temps `t` est un doublon
si `m(u) == m(u-1)`. Pour chaque phase candidate de `0` a `den-1`, le
calibrateur compare le motif predit au motif observe et ne retient que les
phases qui correspondent **exactement**.

- une seule phase correspond -> `calibration=calibrated`
- aucune ou plusieurs -> `calibration=ambiguous`, aucune supposition

Il faut `2*den + 1` echantillons consecutifs (13 pour 50 -> 60). Tant que le
verdict n'est pas rendu, le plugin reste en passthrough.

La phase est mise en cache dans l'instance. Elle n'est remesuree que si le
ratio change (modification de `Source Framerate` ou `Target Framerate`) ou si le
clip source change.

## Le mapping

Une fois la phase connue, **plus aucune comparaison d'images**. Tout est de
l'arithmetique entiere sur `num/den` :

```text
u          = timelineTime - phase
numerator  = num * u
frameA     = floor(numerator / den)
remainder  = numerator - frameA * den        (0 <= remainder < den)
timestep   = remainder / den                 exact, rationnel
frameB     = frameA + 1

timelineTimeA = phase + ceil(den * frameA / num)
timelineTimeB = phase + ceil(den * frameB / num)
```

`timestep` est un rapport d'entiers : aucune derive flottante, meme sur une
timeline a plusieurs milliers de frames.

### Pourquoi ce calcul des temps A et B

`m(u) == k` est vrai pour `den*k/num <= u < den*(k+1)/num`. Le premier entier de
cet intervalle est `ceil(den*k/num)`. En prenant systematiquement **le premier
temps de chaque bloc**, on tombe toujours sur la frame que l'hote n'a pas
dupliquee. C'est ce qui garantit que RIFE ne recoit jamais deux fois la meme
image.

### Cycle 50 -> 60 verifie

Sortie de `CadenceMappingTest --dump` (phase 0) :

```text
timelineTime cyclePos sourcePosition frameA frameB timelineA timelineB timestep rife
7002  0  5835     5835  5836  7002  7004  0         0
7003  1  5835.83  5835  5836  7002  7004  0.833333  1
7004  2  5836.67  5836  5837  7004  7005  0.666667  1
7005  3  5837.5   5837  5838  7005  7006  0.5       1
7006  4  5838.33  5838  5839  7006  7007  0.333333  1
7007  5  5839.17  5839  5840  7007  7008  0.166667  1
7008  0  5840     5840  5841  7008  7010  0         0
```

Le cycle des timestep est exactement `0, 0.8333, 0.6667, 0.5, 0.3333, 0.1667`.

Le point cle est sur les deux premieres lignes : aux positions de cycle 0 et 1,
la paire est `A@7002 / B@7004`. Le temps `7003`, qui porte le doublon, **n'est
jamais demande**.

## Cas ou la calibration echoue

Aucun n'entraine de supposition : le plugin passe en hold sur le temps de rendu
et l'annonce.

| Cas | Etat | Detection |
|-----|------|-----------|
| Clip statique, noir, image figee | `ambiguous` | une seule signature distincte dans la fenetre |
| Fondu, ou aucun doublon la ou le cycle en attend | `ambiguous` | aucune phase ne correspond |
| Doublons reels dans le media | `ambiguous` | doublons en trop, aucune phase ne correspond |
| Cadences identiques | `notApplicable` | `num == den` |
| Source plus rapide que la timeline | `notApplicable` | `num > den`, l'hote a supprime des frames, elles ne sont pas recuperables |
| Cadence absente | `notApplicable` | ratio nul |
| Cycle superieur a 30 frames | `ambiguous` | cadence probablement mal lue |

La fenetre glisse : une fenetre polluee par un doublon reel finit par sortir, et
la calibration peut aboutir plus loin dans le clip sans intervention.

`Phase Override` (parametre debug, `-1` = automatique) force une phase quand la
calibration reste ambigue sur un plan difficile.

## Logs

Bloc par frame de sortie, filtre DebugView `[RifeOFX][Cadence]` :

```text
[RifeOFX][Cadence]
timelineTime=7003
phase=2
cyclePosition=1
periodSource=5 periodTimeline=6
sourcePosition=5835.833333
sourceFrameA=5835
sourceFrameB=5836
timelineTimeA=7002.000
timelineTimeB=7004.000
timestep=0.833333
signatureA=0x00a33e63a56f0612
signatureB=0x00a41132f6355680
rifeInference=1
calibration=calibrated policy=interpolate identicalInputs=0
```

Et l'etat de calibration, a chaque changement :

```text
[RifeOFX] calibration=pending phase=0 manual=0 periodSource=5 periodTimeline=6 samples=7/13 reason=collecting consecutive samples (7/13)
[RifeOFX] calibration=calibrated phase=2 manual=0 periodSource=5 periodTimeline=6 samples=13/13 reason=run=13 from=6991 distinct=11 repeats=2
```

Points a verifier sur 60 frames consecutives :

1. `timestep` suit le cycle `0, 0.8333, 0.6667, 0.5, 0.3333, 0.1667` ;
2. `timelineTimeA != timelineTimeB` des que `rifeInference=1` ;
3. `identicalInputs=0` sur toutes les frames interpolees ;
4. `sourceFrameA` avance de 50 sur 60 frames de timeline ;
5. `calibration=calibrated` apparait une fois et ne revient pas.
