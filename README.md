# MIMIC — Parasitic Reactive Synthesizer (VST3)

Compilation (PowerShell, Visual Studio 2026, depuis ce dossier) :

    cmake -S . -B build -G "Visual Studio 18 2026" -A x64
    cmake --build build --config Release

Résultat : `build\VST3\Release\MIMIC.vst3` (à copier dans `C:\Program Files\Common Files\VST3\`).
Compiler en **Release** (le chargeur WebView2 statique est en /MT).

Interface : `ui/mimic_ui.html` est la page d'interface autonome (déjà générée) ; elle est embarquée dans la DLL.
Pour la régénérer depuis l'artifact : `python tools/make_ui.py mimic-3d.html three.min.js ui/mimic_ui.html`.
