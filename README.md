# MIMIC — ONIRIQUE × Abrasion

Synthé parasite : il joue les notes qu'on lui donne en MIDI, et prend le timbre et le mouvement d'un autre son —
le sidechain du DAW, ou un fichier déposé dans sa gueule.

VST3 (Windows, macOS) et Audio Unit (macOS). Instrument, entrée sidechain mono ou stéréo, sortie stéréo.
Latence d'une trame d'analyse, déclarée au DAW (1024 échantillons à 44,1 et 48 kHz).

## Ce qu'il fait

| Contrôle | Rôle |
|---|---|
| **MORPH** | Imprime l'enveloppe spectrale de la source (ses formants) sur les oscillateurs. |
| **CARVE** | Creuse dans le synthé les formants dominants de la source, jusqu'à −24 dB. |
| **GROOVE** | La dynamique de la source pilote le volume et l'ouverture du filtre du synthé. |
| **DRY/WET** | Du synthé brut au synthé transformé. |
| **BLEND** | Du son de base (sidechain ou fichier) au synthé seul. |
| **OUTPUT** | Niveau de sortie, de −48 à +6 dB. La sortie ne dépasse jamais 0 dBFS (genou doux). |
| **VOIX / PERCU / TEXTURE** | Façon de lire la source : formants fins, enveloppe large et réactive, détail lent. |
| **Forme d'onde** | Dent de scie, carré, triangle, sinus. |
| **LOCK / FREEZE** (Espace) | Fige le timbre capturé : il reste jouable sans la source. |

Un fichier WAV, FLAC ou MP3 déposé dans la gueule remplace le sidechain : chaque note le relance, le do du milieu
à sa hauteur d'origine, les autres notes transposées sans changer sa durée. Choisir VOIX, PERCU ou TEXTURE rend
la main au sidechain.

Sceaux : glisser verticalement (Maj pour le réglage fin), molette, double clic pour la valeur d'origine,
flèches du clavier sur le sceau survolé. Pédale de sustain et molette de hauteur (±2 demi-tons) sont suivies.

## Construire

JUCE 9.0.3 est cherché dans `JUCE/`, puis dans `../PitchBox/JUCE` : les plugins de la gamme partagent la même copie.

```bash
cmake -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Le plugin sort dans `build/Mimic_artefacts/Release/VST3/MIMIC.vst3`. Un envoi sur `main` le compile pour macOS et
Windows (`.github/workflows/build.yml`) ; les fichiers sont laissés sur les branches `ci-build-macOS` et
`ci-build-Windows`, les journaux sur `ci-logs-…`.

Outil de développement, sans hôte :

```bash
cmake --build build --config Release --target mimic_snapshot
build/Release/mimic_snapshot.exe snapshot.png                    # l'interface, une voix dans le sidechain
build/Release/mimic_snapshot.exe --selftest demo/sidechain.wav   # le plugin tel qu'un hôte le pilote
```

## Sources

| Fichier | Contenu |
|---|---|
| `Source/dsp/` | Tout le son, sans dépendance : analyse, oscillateurs, suiveur, lecteur de fichier. |
| `Source/audio_file.*`, `Source/third_party/` | Décodage WAV, FLAC, MP3 (dr_libs, voir `THIRD_PARTY.md`). |
| `Source/PluginProcessor.*` | Paramètres, MIDI, sidechain ou fichier, état sauvegardé. |
| `Source/PluginEditor.*` | L'interface, dessinée par le plugin. |
| `License/` | Le module de licence ONIRIQUE, le même dans tous les plugins : ne se modifie que dans ONIRIQUE CENTRAL. |
| `ui/mimic_ui.html` | La page d'origine de l'interface. Elle n'est plus embarquée : elle sert de modèle au décor. |
| `tools/bake_ui.py` | Refait les images du décor (`Resources/ui/`) à partir de cette page. |
| `tests/dsp_test.cpp` | Le moteur entier, hors ligne. |
