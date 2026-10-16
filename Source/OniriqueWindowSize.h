#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
//  La taille de la fenêtre des plugins ONIRIQUE, la même règle partout.
//
//  - À l'ouverture : la taille gardée dans le projet ; sinon la dernière taille choisie pour ce plugin sur cet
//    ordinateur ; sinon une taille confortable, au plus 62 % de la largeur et 75 % de la hauteur de l'écran.
//  - Jamais plus grande que l'écran où se trouve la souris (un projet rouvert sur un écran plus petit).
//  - remember() garde la taille dans le projet (l'état du plugin, enregistré par le DAW) ;
//    rememberOnThisComputer() la garde pour les prochaines ouvertures du plugin, nouveaux projets compris
//    (fichier Onirique/<plugin>.settings dans les réglages de l'utilisateur).
//
//  Ce fichier se copie tel quel à côté d'OniriqueKnob.h dans chaque plugin ONIRIQUE, et ne se modifie qu'ici
//  (plugin-ui/ du dépôt ONIRIQUE CENTRAL).
//
//      const auto s = onirique::window::openingSize (apvts.state, "ONIRIQUE TUNER", { 1200, 700 }, { 900, 520 }, { 2400, 1400 });
//      setSize (s.x, s.y);
//      ...  resized() : onirique::window::remember (apvts.state, getWidth(), getHeight());
//      ...  ~Editor() : onirique::window::rememberOnThisComputer ("ONIRIQUE TUNER", getWidth(), getHeight());
//==============================================================================
namespace onirique::window
{
inline juce::PropertiesFile::Options settingsFile (const juce::String& plugin)
{
    juce::PropertiesFile::Options o;
    o.applicationName = plugin;
    o.filenameSuffix = ".settings";
    o.folderName = "Onirique";
    o.osxLibrarySubFolder = "Application Support";
    o.storageFormat = juce::PropertiesFile::storeAsXML;
    return o;
}

/** preferred : la taille dessinée par défaut ; minimum / maximum : les limites de redimensionnement du plugin.
    state : l'état enregistré dans le projet (un ValueTree vide si le plugin n'y garde pas sa taille). Seule une taille
    écrite par remember() compte : les anciens projets gardaient une largeur seule, celle par défaut, souvent trop grande. */
inline juce::Point<int> openingSize (const juce::ValueTree& state, const juce::String& plugin,
                                     juce::Point<int> preferred, juce::Point<int> minimum, juce::Point<int> maximum)
{
    juce::Point<int> size { (int) state.getProperty ("uiWidth", 0), (int) state.getProperty ("uiHeight", 0) };
    bool chosen = true;   // une taille choisie par l'utilisateur, et pas celle par défaut
    if (size.x <= 0 || size.y <= 0)
    {
        juce::PropertiesFile prefs (settingsFile (plugin));
        size = { prefs.getIntValue ("uiWidth"), prefs.getIntValue ("uiHeight") };
        if (size.x <= 0 || size.y <= 0) { size = preferred; chosen = false; }
    }

    // l'écran du DAW : celui où se trouve la souris, qui vient d'ouvrir le plugin
    if (const auto* d = juce::Desktop::getInstance().getDisplays().getDisplayForPoint (juce::Desktop::getMousePosition()))
    {
        const auto area = d->userArea;   // sans la barre des tâches ni le Dock
        const float k = juce::jmin (1.0f, (chosen ? 0.94f : 0.62f) * (float) area.getWidth() / (float) size.x,
                                          (chosen ? 0.94f : 0.75f) * (float) area.getHeight() / (float) size.y);
        size = { juce::roundToInt ((float) size.x * k), juce::roundToInt ((float) size.y * k) };
    }
    return { juce::jlimit (minimum.x, juce::jmax (minimum.x, maximum.x), size.x), juce::jlimit (minimum.y, juce::jmax (minimum.y, maximum.y), size.y) };
}

inline void remember (juce::ValueTree state, int width, int height)
{
    state.setProperty ("uiWidth", width, nullptr);
    state.setProperty ("uiHeight", height, nullptr);
}

inline void rememberOnThisComputer (const juce::String& plugin, int width, int height)
{
    if (width <= 0 || height <= 0) return;
    juce::PropertiesFile prefs (settingsFile (plugin));
    prefs.setValue ("uiWidth", width);
    prefs.setValue ("uiHeight", height);
    prefs.saveIfNeeded();
}
} // namespace onirique::window
