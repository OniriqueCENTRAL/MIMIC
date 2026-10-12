// MIMIC — le plugin : paramètres, MIDI, sidechain ou fichier déposé, état sauvegardé.
// Tout le son est dans dsp/ ; ce fichier le branche sur l'hôte.
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "License/LicenseGuard.h"
#include "audio_file.h"
#include "dsp/mimic_dsp.h"

namespace mimic
{

class MimicProcessor final : public juce::AudioProcessor,
                             private juce::VST3ClientExtensions
{
public:
    MimicProcessor();
    ~MimicProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "MIMIC"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 1.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::VST3ClientExtensions* getVST3ClientExtensions() override { return this; }

    //==============================================================================
    /** Le fichier déposé dans la gueule, tel que l'interface le montre. */
    struct ClipInfo
    {
        bool loaded = false;
        juce::String name, error;
        double seconds = 0.0;
        std::array<float, dsp::AudioClip::kPeaks> peaks {};
    };

    /** Charge un fichier audio comme modulateur et passe sur l'entrée « fichier ». Thread de l'interface. */
    bool loadFile (const juce::File& file);
    ClipInfo getClipInfo() const;
    /** Change à chaque fichier chargé ou refusé. */
    int getClipSerial() const noexcept { return clipSerial.load(); }

    /** La dernière trame envoyée par le moteur : spectres, énergie, vumètres. */
    dsp::VisFrame getVisual() const;
    int getActiveVoices() const noexcept { return activeVoices.load (std::memory_order_relaxed); }

    juce::AudioProcessorValueTreeState apvts;
    onirique::license::Guard license { "onirique-x-abrasion-mimic" };

    static constexpr int kSourceVoice = 0, kSourcePercussion = 1, kSourceTexture = 2;
    static constexpr int kInputSidechain = 0, kInputFile = 1;

private:
    // Le sidechain est la seule entrée, et c'est un bus auxiliaire : un instrument n'a pas d'entrée principale.
    bool getPluginHasMainInput() const override { return false; }

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void readParameters();
    void applyPendingClip();
    bool loadClip (const juce::String& path, bool report);
    void publishClip (const dsp::AudioClip* clip, const juce::String& error);

    static constexpr int kMaxEvents = 512;

    std::atomic<float>* morph = nullptr;
    std::atomic<float>* carve = nullptr;
    std::atomic<float>* groove = nullptr;
    std::atomic<float>* wet = nullptr;
    std::atomic<float>* output = nullptr;
    std::atomic<float>* blend = nullptr;
    std::atomic<float>* freeze = nullptr;
    std::atomic<float>* source = nullptr;
    std::atomic<float>* input = nullptr;
    std::atomic<float>* wave = nullptr;

    dsp::Engine engine;
    dsp::FilePlayer player;
    std::array<dsp::NoteEvent, kMaxEvents> events {}, chunkEvents {};
    std::vector<float> scratchL, scratchR;
    bool prepared = false;

    // Le clip passe de l'interface au thread audio sans verrou : en attente, actif, puis retiré.
    std::atomic<dsp::AudioClip*> pendingClip { nullptr };
    std::array<std::atomic<dsp::AudioClip*>, 2> retiredClip { { { nullptr }, { nullptr } } };
    std::atomic<bool> hasPending { false };
    dsp::AudioClip* activeClip = nullptr;

    juce::CriticalSection clipLock;          // hors thread audio : chargement et lecture de l'aperçu
    juce::String filePath;
    ClipInfo clipInfo;
    std::atomic<int> clipSerial { 0 };

    mutable juce::SpinLock visualLock;       // le thread audio ne fait que tenter de le prendre
    dsp::VisFrame visual;
    std::atomic<int> activeVoices { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MimicProcessor)
};

} // namespace mimic
