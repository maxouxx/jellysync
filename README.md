# JellySync — Vivlio Inkpad 3 / PocketBook iMX6

Synchroniseur de bibliothèque Jellyfin pour liseuse Vivlio Inkpad 3.

---

## Compilation sur Windows 10 (WSL2)

### Étape 1 — Installer WSL2

Dans PowerShell en **administrateur** :
```powershell
wsl --install
```
Redémarrez. Ubuntu s'installe automatiquement.

### Étape 2 — Installer les dépendances dans WSL2

```bash
sudo apt-get update
sudo apt-get install -y cmake make git file build-essential
```

### Étape 3 — Cloner le SDK PocketBook

```bash
mkdir -p ~/pocketbook
git clone --depth=1 --branch 5.19 \
    https://github.com/pocketbook/SDK_6.3.0.git \
    ~/pocketbook/sdk

# Vérification (doit afficher : ELF 64-bit LSB executable, x86-64)
file ~/pocketbook/sdk/SDK-iMX6/usr/bin/arm-obreey-linux-gnueabi-gcc
```

### Étape 4 — Compiler le projet

```bash
export PBSDK=~/pocketbook/sdk/SDK-iMX6/usr

cd ~/JellySync
mkdir -p build && cd build

cmake .. -DTOOLCHAIN_PATH=$PBSDK -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Étape 5 — Vérifier l'architecture

```bash
readelf -h build/jellysync.app | grep -E "Machine|Class"
# Attendu :
#   Class:   ELF32
#   Machine: ARM
```

### Étape 6 — Déployer sur la liseuse (USB)

Branchez la liseuse en USB. Elle apparaît dans Windows comme un lecteur (ex: `E:\`).
Dans WSL2, accédez-y via `/mnt/e/` :

```bash
mkdir -p /mnt/e/applications/jellysync.app
cp build/jellysync.app /mnt/e/applications/jellysync.app/jellysync.app
```

### Étape 6b — Déployer via SSH (recommandé)

La Vivlio Inkpad 3 expose SSH sur le port 2222 (IP : `192.168.1.44`).
Mot de passe par défaut : `root` (vérifiez dans Paramètres > Wi-Fi > SSH).

```bash
# Depuis le dossier build/ après compilation
scp -P 2222 jellysync.app root@192.168.1.44:/mnt/ext1/applications/jellysync.app/jellysync.app
```

Pour créer le dossier si nécessaire :
```bash
ssh -p 2222 root@192.168.1.44 "mkdir -p /mnt/ext1/applications/jellysync.app"
scp -P 2222 jellysync.app root@192.168.1.44:/mnt/ext1/applications/jellysync.app/jellysync.app
```

Script de déploiement rapide (à lancer depuis la racine du projet) :
```bash
#!/bin/bash
set -e
export PBSDK=~/pocketbook/sdk/SDK-iMX6/usr
cd build
cmake .. -DTOOLCHAIN_PATH=$PBSDK -DCMAKE_BUILD_TYPE=Release -DDEVICE=/tmp/unused
make -j$(nproc)
ssh -p 2222 root@192.168.1.44 "mkdir -p /mnt/ext1/applications/jellysync.app"
scp -P 2222 jellysync.app root@192.168.1.44:/mnt/ext1/applications/jellysync.app/jellysync.app
echo "Déployé avec succès !"
```

Redémarrez l'app depuis le lanceur ou via SSH :
```bash
ssh -p 2222 root@192.168.1.44 "killall jellysync.app; /mnt/ext1/applications/jellysync.app/jellysync.app &"
```

---

## Compilation via GitHub Actions (sans WSL2)

Poussez le code sur GitHub, la compilation se déclenche automatiquement.
Téléchargez le binaire dans l'onglet **Actions → Artifacts → jellysync-imx6**.

---

## Débogage (log fichier)

Si l'app plante, ajoutez dans `main.cpp` :
```cpp
#include "log.h"
// Au début de main() :
log_init();
LOG("démarrage");
```
Le fichier `/mnt/ext1/jellysync.log` sera lisible via USB.

---

## Structure

```
JellySync/
├── CMakeLists.txt
├── .github/workflows/build.yml
└── src/
    ├── main.cpp          # Point d'entrée + boucle événements
    ├── config.h          # Config persistée + structures BookEntry
    ├── jellyfin_api.h    # Client REST Jellyfin (libcurl)
    ├── sync.h            # Moteur sync (catalogue + téléchargement)
    ├── ui.h              # Interface e-ink (liste, filtres, scroll)
    └── cJSON.h           # Parser JSON (MIT)
```

---

## Utilisation

1. Lancez **JellySync** depuis le lanceur d'applications
2. Écran **Configuration** : saisissez l'URL Jellyfin + identifiants ou clé API
3. Appuyez **Refresh** — la liste de tous vos dossiers s'affiche
4. **Vue dossiers** :
   - Appuyez sur un dossier pour voir les livres qu'il contient
   - Appuyez **⬇ Tout** pour télécharger tous les nouveaux livres du dossier
5. **Vue livres** (dans un dossier) :
   - Case à cocher à gauche : sélectionnez plusieurs livres
   - Bouton **⬇ DL** sur la ligne : télécharge ce livre immédiatement
   - Quand des livres sont cochés, le bouton **DL (N)** apparaît en haut à droite
   - **Annuler** efface la sélection
6. **Sync** télécharge tous les nouveaux/mis à jour de toute la bibliothèque
7. Statuts :
   - **N** = Nouveau sur le serveur
   - **M** = Mis à jour
   - **✓** = Déjà présent
   - **?** = Local seulement (pas sur le serveur)
