<div align="center">

<img src="docs/images/mirror_dock_logo.svg" width="120" alt="MirrorDock">

# MirrorDock

**Recopiez l’écran de votre iPhone ou iPad sur un PC Windows, en Wi-Fi ou par le câble USB.**

[![Release](https://img.shields.io/github/v/release/bestkakkoii/MirrorDock?label=release&color=126BDB)](https://github.com/bestkakkoii/MirrorDock/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/bestkakkoii/MirrorDock/total?color=33B8FF)](https://github.com/bestkakkoii/MirrorDock/releases)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011%20x64-0078D4)](https://github.com/bestkakkoii/MirrorDock/releases/latest)
[![C++26](https://img.shields.io/badge/C%2B%2B-26%20modules-00599C)](README_AI.md)
[![Licence](https://img.shields.io/badge/licence-GPL--3.0-blue)](LICENSE)
[![Stars](https://img.shields.io/github/stars/bestkakkoii/MirrorDock?style=social)](https://github.com/bestkakkoii/MirrorDock/stargazers)

[English](README.md) · [繁體中文](README.zh-TW.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Español](README.es.md) · [Deutsch](README.de.md) · **Français** · [Tiếng Việt](README.vi.md) · [ไทย](README.th.md)

</div>

---

MirrorDock est un récepteur AirPlay pour Windows. Choisissez-le dans *Recopie de l’écran* sur l’iPhone : l’image et le son du téléphone arrivent sur le PC, dans une fenêtre qui se comporte comme un lecteur multimédia. Le tout tient en un seul fichier, `mirror_dock.exe` : pas d’installeur, pas de GStreamer, pas de service Bonjour, aucun environnement d’exécution à mettre en place.

Il repose sur le cœur du protocole AirPlay d’[UxPlay](https://github.com/FDH2/UxPlay), associé à un moteur multimédia écrit pour Windows (Direct3D 11, FFmpeg, WASAPI). Jusqu’à la version 1.6, il était publié sous le nom *UxPlay for Windows (native)*.

## ✨ Points forts

| | |
|---|---|
| 📶 **Recopie en Wi-Fi** | Le téléphone et le PC sur le même réseau, rien à installer ni d’un côté ni de l’autre. |
| 🔌 **Recopie par câble USB** | Une fois le *Partage de connexion* activé, AirPlay passe par le câble de charge : c’est local, stable, et cela ne consomme pas de données mobiles. |
| 🎬 **Une image nette et fluide** | H.265 / H.264 décodés par le GPU qui pilote votre écran (Intel, AMD ou NVIDIA), affichés sans copie, au rythme de l’écran. Le décodage logiciel prend le relais quand le GPU n’en est pas capable. |
| 🔊 **Un son synchronisé** | Sortie WASAPI à faible latence, sur la même ligne de temps que l’image : les lèvres et la voix concordent. |
| 🎛️ **Trois modes** | *Qualité*, *Équilibré* (par défaut) et *Performances* : à vous de choisir entre l’image la plus nette et la latence la plus faible. |
| ✂️ **Rognage des bandes noires** | Supprime automatiquement les bandes noires autour d’un film, ou vous laisse cadrer à la souris n’importe quelle partie de l’image. |
| 🪟 **Une vraie fenêtre** | S’ouvre à une taille raisonnable, conserve les proportions quand vous la redimensionnez, mémorise sa taille et sa position, passe en plein écran d’une seule touche et peut rester toujours au premier plan. |
| 🌐 **Dix langues** | L’interface suit la langue d’affichage de Windows. |
| 🧳 **Un seul fichier** | Un exécutable unique d’environ 10 Mo, qui n’a besoin que de Windows. |

## 🚀 Démarrage rapide

1. Téléchargez `mirror_dock_win64.zip` depuis les [Releases](https://github.com/bestkakkoii/MirrorDock/releases/latest) et décompressez-le où vous voulez.
2. Lancez `mirror_dock.exe`. Si le Pare-feu Windows vous pose la question, choisissez **Autoriser** (un réseau domestique est un réseau *privé*).
3. Sur l’iPhone : **Centre de contrôle → Recopie de l’écran → MirrorDock@votre-PC**.

Le programme se loge dans la zone de notification (à côté de l’horloge). La fenêtre vidéo apparaît quand le téléphone se connecte ; la fermer met fin à cette session, et **Quitter**, dans le menu, arrête le programme.

> [!TIP]
> Configuration requise : Windows 10 ou 11 (x64) et un GPU Direct3D 11. À défaut, c’est le moteur de rendu logiciel intégré qui est utilisé.

## 🔌 Recopie par le câble USB

1. Installez une fois pour toutes le pilote USB d’Apple : lancez `install_apple_usb_support.cmd`, fourni dans le zip, ou installez **Appareils Apple** depuis le Microsoft Store.
2. Sur l’iPhone, activez **Réglages → Partage de connexion → Autoriser d’autres utilisateurs**, puis branchez le câble.
3. Choisissez MirrorDock dans *Recopie de l’écran*. La barre de contrôle indique **USB** ou **Wi-Fi**, selon la liaison utilisée.

- **USB en priorité / Wi-Fi uniquement** change de liaison sans toucher au périphérique USB : le téléphone continue donc de se charger. *Wi-Fi uniquement* est réellement appliqué : une connexion qui arriverait malgré tout par le câble est refusée.
- **Vos données mobiles ne risquent rien.** La recopie elle-même est locale. Si Windows se met à faire passer par le téléphone le trafic Internet du PC lui-même, MirrorDock s’en aperçoit et propose d’y remédier en un clic (**Économiser les données mobiles**).

## 🎛️ Modes

| Mode | Ce qu’il fait | Idéal pour |
|---|---|---|
| **Qualité** | Pleine résolution, un peu plus de mise en tampon, aucune image sautée. | Films, présentations, texte |
| **Équilibré** *(par défaut)* | S’adapte à l’écran, au GPU et au réseau. | Usage quotidien |
| **Performances** | Un flux allégé (jusqu’à 1920×1080), une mise en tampon minimale, toujours l’image la plus récente. | Jeux, PC anciens |

Choisissez le mode dans le menu ou avec `-mode`. La latence s’ajuste aussitôt ; la résolution du flux change à partir de la connexion suivante.

## ✂️ Rogner les bandes noires

Un film en 16:9 lu sur un téléphone à l’écran plus large est recopié avec des bandes noires, car le téléphone recopie la totalité de son écran.

- **Automatique** (touche **C**) : les bandes sont rognées et la fenêtre épouse l’image. Le rognage est conservé d’un film au suivant ; quand autre chose apparaît dans une bande et y reste, les commandes du lecteur par exemple, l’image entière revient en une seconde environ.
- **À la main** (touche **X**) : tracez un cadre autour de la partie que vous voulez voir. **Esc** annule.

## ⌨️ Clavier et souris

| Action | Touches |
|---|---|
| Activer / quitter le plein écran | **F**, **F11**, **Alt+Enter** ou double-clic ; **Esc** pour quitter |
| Rogner automatiquement les bandes noires | **C** |
| Encadrer la zone à afficher | **X** |
| Toujours au premier plan | **T** |
| Couper le son (côté PC uniquement) | **M** |
| Barre de contrôle | Déplacez la souris sur l’image |
| Menu | Faites un clic droit n’importe où sur l’image |

## ⚙️ Options

Ajoutez les options à un raccourci vers `mirror_dock.exe`, ou écrivez-les à raison d’une par ligne (sans le `-` initial) dans `%USERPROFILE%\.uxplayrc`. `mirror_dock.exe -h` en donne la liste complète.

| Option | Effet |
|---|---|
| `-n name` | Nom affiché sur l’iPhone |
| `-fs` | Démarrer en plein écran |
| `-mode quality\|balanced\|performance` | Ce qu’il faut privilégier |
| `-crop auto\|off` | Rogner les bandes noires / afficher l’image entière |
| `-gpu auto\|integrated\|discrete\|software` | GPU utilisé pour le décodage et l’affichage |
| `-h264` / `-h265` | Ne proposer que H.264 / toujours proposer H.265 |
| `-ui language` | Langue de l’interface (voir plus bas) |
| `-vsync ms` | Décaler le son par rapport à l’image |
| `-pin`, `-pw` | Exiger un code PIN ou un mot de passe |

## 🌐 Langues

L’interface suit la langue d’affichage de Windows : English, 繁體中文, 简体中文, 日本語, 한국어, Español, Deutsch, Français, Tiếng Việt et ไทย. Dans toute autre langue, l’interface s’affiche en anglais. Pour en imposer une, utilisez `-ui en|zh-TW|zh-CN|ja|ko|es|de|fr|vi|th`.

Les guides d’utilisation dans les dix langues se trouvent dans le zip (dossier `user_guide`).

## 🧩 Fonctionnement

```mermaid
flowchart LR
    phone["📱 iPhone / iPad"] -- "AirPlay en Wi-Fi<br/>ou par la liaison réseau USB" --> core["Cœur du protocole<br/>RTSP · jumelage · synchronisation"]
    core --> video["Vidéo<br/>décodage FFmpeg + Direct3D 11"]
    core --> audio["Audio<br/>décodage AAC / ALAC"]
    video --> clock["Une seule ligne de temps de présentation"]
    audio --> clock
    clock --> screen["🖥️ Fenêtre<br/>swap chain en mode flip"]
    clock --> speaker["🔊 WASAPI<br/>sortie à faible latence"]
```

L’image et le son sont planifiés sur une même ligne de temps, avec un retard qui s’adapte au réseau et à la vitesse de décodage du PC : le plus court possible, sans saccades.

## 📊 Mesures

Un portable de 2016 (Intel Core i7-6700HQ, Intel HD Graphics 530), un flux de test H.265 en 2872×1320 à 60 fps, mode *Équilibré*, version 1.6 :

| | |
|---|---|
| Images du flux de test affichées | 480 sur 480 |
| Décalage audio / vidéo, une fois stabilisé | environ 18 ms |
| Mémoire au repos | 46 Mo |
| Fermeture | 0,04 s |

La recopie en Wi-Fi et par USB a été vérifiée avec un iPhone 16 Pro Max (iOS 26) jusqu’à la version 1.5, et la version 1.7 a tourné avec ce même téléphone en Wi-Fi : son flux en 3200×1536 a occupé environ 310 Mo de mémoire et 200 Mo de mémoire GPU. [CHANGELOG.md](CHANGELOG.md) précise, pour chaque version, ce qui a été testé avec un téléphone et ce qui a seulement été compilé.

## ❓ FAQ

**L’iPhone n’affiche pas mon PC dans la liste.**
Les deux appareils doivent être sur le même réseau, et le Pare-feu Windows doit autoriser `mirror_dock.exe`. Désactiver puis réactiver le Wi-Fi de l’iPhone actualise la liste.

**Le câble est branché, mais la recopie passe toujours par le Wi-Fi.**
Le Partage de connexion doit être activé, et le choix de liaison doit être *USB en priorité*. La deuxième ligne du menu indique l’état de la liaison USB.

**Il n’y a pas de son.**
Vérifiez le bouton haut-parleur de la barre de contrôle, puis le mélangeur de volume de Windows.

**Les lèvres et la voix sont décalées.**
Affinez le réglage avec `-vsync` suivi d’un nombre de millisecondes (une valeur positive retarde le son).

**Où se trouve le journal ?**
`%LOCALAPPDATA%\MirrorDock\mirror_dock.log`, ou **Ouvrir le fichier journal** dans le menu.

## 🛠️ Compiler depuis les sources

MSYS2 CLANG64 (clang 23, libc++ avec le module de la bibliothèque standard C++26, CMake 4.4, Ninja) et le compilateur de shaders du SDK Windows. FFmpeg, OpenSSL et libplist sont compilés en bibliothèques statiques à partir de commits épinglés.

```sh
bash win_native/build_scripts/fetch_deps.sh <src_root>
MSYSTEM=CLANG64 bash -lc 'bash win_native/build_scripts/build_deps.sh <src_root> <prefix> libplist openssl ffmpeg'
MSYSTEM=CLANG64 bash -lc 'bash win_native/build_scripts/build_uxplay_native.sh <this_repo> <build_dir> <prefix> Release'
```

[README_AI.md](README_AI.md) décrit l’architecture, les invariants, les dépendances épinglées et ce qui a été vérifié. [CHANGELOG.md](CHANGELOG.md) recense les versions publiées.

## 📜 Licence

MirrorDock est distribué sous la **GNU General Public License v3** ([LICENSE](LICENSE)), car il repose sur le code GPL-3.0 d’UxPlay. Le moteur multimédia Windows de [`win_native/`](win_native) est une œuvre originale placée sous **licence MIT** ([win_native/LICENSE](win_native/LICENSE)). Le zip de chaque version contient les textes de licence de toutes les bibliothèques embarquées ainsi que le code source complet correspondant.

## 🙏 Remerciements

- [UxPlay](https://github.com/FDH2/UxPlay), de F. Duncanh et de ses contributeurs : le serveur AirPlay sur lequel MirrorDock est bâti.
- [RPiPlay](https://github.com/FD-/RPiPlay), de Florian Draschbacher, et [shairplay](https://github.com/juhovh/shairplay), de Juho Vähä-Herttua : les projets dont provient ce cœur de protocole.
- playfair, llhttp, csrp, FFmpeg, OpenSSL et libplist.

AirPlay, iPhone et iPad sont des marques d’Apple Inc. MirrorDock est un projet open source indépendant, sans lien avec Apple.
