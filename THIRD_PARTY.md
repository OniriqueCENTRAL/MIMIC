# Composants tiers embarqués
- **Steinberg VST3 SDK** — récupéré par CMake (licence VST3 / GPLv3, voir le dépôt vst3sdk).
- **dr_wav, dr_flac, dr_mp3** (David Reid, mackron/dr_libs) — domaine public / MIT-0. `source/third_party/`.
- **three.js r128** (mrdoob) — MIT. Embarqué dans `ui/mimic_ui.html` (généré par `tools/make_ui.py`).
- **Microsoft Edge WebView2 SDK** — récupéré par CMake depuis NuGet (Windows). Le *Runtime* WebView2 doit être présent sur la machine (livré avec Windows 11 / Edge).
