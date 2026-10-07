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

## Tester sur PC (émulateur)

Le dossier `emulator/` permet de lancer JellySync dans une fenêtre sur PC, sans liseuse.
Il remplace InkView (la bibliothèque graphique PocketBook) par une version SDL2 qui
simule l'écran de l'InkPad 3 (1404×1872, 16 niveaux de gris). Le code de `src/` est
compilé tel quel ; le build liseuse n'est pas modifié.

### Installation des dépendances (une seule fois)

**Mac** (avec [Homebrew](https://brew.sh)) :
```bash
brew install cmake pkg-config sdl2 sdl2_ttf
```

**Windows** : dans Ubuntu sous WSL2 (les fenêtres Linux s'affichent directement sous
Windows 11 et Windows 10 à jour ; si besoin, `wsl --update` dans PowerShell) :
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config libsdl2-dev libsdl2-ttf-dev libcurl4-openssl-dev python3
```

### Lancer

Depuis la racine du projet :
```bash
./emulator/run.sh          # avec votre vrai serveur Jellyfin
./emulator/run.sh --mock   # avec un faux serveur Jellyfin de démonstration
```

Avec `--mock`, saisissez `http://127.0.0.1:8096` comme serveur et n'importe quel
utilisateur / mot de passe (le mot de passe `bad` simule une erreur d'authentification).

### Commandes

| Liseuse | Émulateur |
|---|---|
| Toucher l'écran | Clic gauche |
| Bouton Retour | Échap ou Retour arrière |
| Page précédente / suivante | ← / → , Page ↑ / Page ↓ , molette |
| Clavier virtuel | Boîte de saisie : Entrée valide, Échap annule, Ctrl/Cmd+V colle |
| — | F5 : redessiner l'écran · F12 : capture `jellysync-screenshot.bmp` |

La mémoire de la liseuse (`/mnt/ext1`) est simulée par `build-emu/ext1/` : la
configuration est dans `build-emu/ext1/system/config/jellysync.cfg` et les livres
téléchargés dans `build-emu/ext1/books/Jellyfin/`.

Options (variables d'environnement) :
- `JELLYSYNC_SCALE=0.5` : taille de la fenêtre (ajustée à l'écran par défaut)
- `JELLYSYNC_SCREEN=1072x1448` : simuler une autre résolution de liseuse
- `JELLYSYNC_SHOT=ecran.bmp` : enregistrer l'écran après chaque rafraîchissement

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
├── emulator/             # Build PC : InkView simulé (SDL2), faux Jellyfin
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
2. **Réglages** : saisissez l'adresse du serveur Jellyfin, puis choisissez
   **Identifiants** (utilisateur + mot de passe) ou **Clé API**
3. La bibliothèque se charge au démarrage ; **Actualiser** la recharge
4. **Accueil** : la liste des dossiers, avec le nombre de nouveaux livres de chacun.
   Touchez un dossier pour l'ouvrir
5. **Dans un dossier** :
   - Le bouton à droite d'un livre le télécharge tout de suite
   - Touchez la ligne d'un livre pour le cocher ; la barre du bas propose alors
     **Télécharger** pour toute la sélection
   - **Tout télécharger** (bandeau du haut) récupère tous les nouveaux livres du dossier
6. Onglets : **Tous**, **Nouveaux** (absents ou mis à jour sur le serveur),
   **Sur la liseuse**
7. Les listes sont paginées : boutons **Précédent / Suivant** ou touches de page
   de la liseuse. La touche Retour remonte d'un niveau
8. Pendant un téléchargement, le bas de l'écran affiche le journal détaillé
   (requêtes, tailles, vitesses, erreurs). Le journal complet est aussi écrit
   dans `/mnt/ext1/jellysync.log`

JellySync ne fait que télécharger : il ne supprime jamais de livre de la liseuse.
