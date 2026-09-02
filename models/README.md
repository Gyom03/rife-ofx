# Modèles RIFE

Les poids ne sont pas téléchargés ni redistribués par ce prototype sans
vérification séparée de leur licence et de leur provenance.

`registry.csv` décrit les modèles attendus par le plugin. Un modèle est
utilisable seulement si son dossier contient `flownet.param` et `flownet.bin`.
Les dossiers doivent être placés à côté du manifeste :

```text
Contents/Resources/models/registry.csv
Contents/Resources/models/rife-v4.25/flownet.param
Contents/Resources/models/rife-v4.25/flownet.bin
```

Les valeurs `external-install-required` indiquent que les poids doivent être
installés séparément par l'utilisateur. Le champ ne constitue pas une licence
de redistribution.
