// MIMIC — l'interface, dessinée par le plugin lui-même.
// Le décor vient des images de Resources/ui (voir tools/bake_ui.py) ; tout ce qui bouge est dessiné ici :
// le visualiseur de la gueule, les aiguilles et les tirets des sceaux, les vumètres, les boutons allumés.
#pragma once

#include "License/LicenseOverlay.h"
#include "PluginProcessor.h"

namespace mimic
{

class MimicEditor final : public juce::AudioProcessorEditor,
                          public juce::FileDragAndDropTarget,
                          private juce::Timer
{
public:
    explicit MimicEditor (MimicProcessor&);
    ~MimicEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int x, int y) override;
    void fileDragMove (const juce::StringArray&, int x, int y) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    static constexpr int kBaseWidth = 943, kBaseHeight = 512;
    static constexpr int kNumKnobs = 6;

private:
    struct Knob
    {
        const char* id;
        float defaultValue;
        juce::RangedAudioParameter* parameter = nullptr;
        float shown = -1.0f;
    };

    void timerCallback() override;

    float scale() const noexcept { return (float) getWidth() / (float) kBaseWidth; }
    juce::Point<float> toBase (juce::Point<float> p) const noexcept { return p / scale(); }
    void repaintBase (juce::Rectangle<float> area);

    int knobAt (juce::Point<float> base) const;
    int buttonAt (juce::Point<float> base) const;     // 0..2 source, 3..6 forme d'onde, 7 freeze, -1 rien
    bool buttonIsOn (int button) const;
    void pressButton (int button);
    void setKnob (int index, float value);
    void setChoice (const char* id, int index);
    void toggleFreeze();

    void rebuildLayers();
    void drawCavity (juce::Graphics&);
    void drawKnob (juce::Graphics&, int index) const;
    void drawMeters (juce::Graphics&) const;

    MimicProcessor& processor;
    std::array<Knob, kNumKnobs> knobs;
    juce::RangedAudioParameter* freezeParameter = nullptr;
    juce::RangedAudioParameter* sourceParameter = nullptr;
    juce::RangedAudioParameter* inputParameter = nullptr;
    juce::RangedAudioParameter* waveParameter = nullptr;

    // Le décor, ramené une fois à la taille de la fenêtre (en pixels réels) : le dessiner ne coûte qu'une copie.
    juce::Image underSource, overSource, onSource, under, over, on, smoke;
    float layerScale = 0.0f;

    // Ce que le moteur montre, lissé comme dans la page d'origine.
    static constexpr int kBands = 72;
    std::array<float, kBands> live {}, frozenBands {}, timbre {}, synth {};
    std::vector<float> peakHold;
    float energy = 0.0f, flash = 0.0f, hover = 0.0f, bite = 0.0f, clipHold = 0.0f;
    float meterIn[2] = {}, meterOut[2] = {};
    bool frozen = false, dragOver = false;
    int buttonStates = -1, clipSerial = -1;
    double time = 0.0;
    juce::uint32 lastTick = 0;
    MimicProcessor::ClipInfo clip;
    juce::String message;
    float messageTime = 0.0f;

    struct Ember { float x, y, speed, radius, phase; };
    std::array<Ember, 38> embers;
    juce::Random random { 12345 };

    int dragging = -1, hovered = -1;
    float dragStartValue = 0.0f;
    juce::Font serif, mono;

    std::unique_ptr<onirique::license::Overlay> licenseOverlay;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MimicEditor)
};

} // namespace mimic
