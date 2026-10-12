#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace mimic
{
using namespace juce;

namespace
{
String percent (float v, int) { return String (roundToInt (v * 100.0f)) + " %"; }
String carveText (float v, int) { return v <= 0.0f ? String ("0.0 dB") : "-" + String (v * 24.0f, 1) + " dB"; }

String outputText (float v, int)
{
    const auto db = dsp::outputDb (v);
    return (db >= 0.0f ? "+" : "") + String (db, 1) + " dB";
}

/** Une trame de 1024 échantillons à 44,1 et 48 kHz, doublée à chaque doublement de la fréquence :
    l'analyse garde la même finesse en hertz et la même réactivité en millisecondes. */
int frameSizeFor (double sampleRate)
{
    return sampleRate > 100000.0 ? 4096 : (sampleRate > 50000.0 ? 2048 : 1024);
}
}

//==============================================================================
AudioProcessorValueTreeState::ParameterLayout MimicProcessor::createLayout()
{
    AudioProcessorValueTreeState::ParameterLayout layout;
    const dsp::ParamValues defaults;
    const NormalisableRange<float> unit { 0.0f, 1.0f };

    auto knob = [&] (const char* id, const char* name, float value, String (*text) (float, int))
    {
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, unit, value,
                                                           AudioParameterFloatAttributes().withStringFromValueFunction (text)));
    };

    knob ("morph", "Morph", defaults.morph, percent);
    knob ("carve", "Carve", defaults.carve, carveText);
    knob ("groove", "Groove", defaults.groove, percent);
    knob ("wet", "Dry/Wet", defaults.wet, percent);
    knob ("output", "Output", defaults.output, outputText);
    knob ("blend", "Blend", defaults.blend, percent);

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "freeze", 1 }, "Lock/Freeze", false));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "source", 1 }, "Source",
                                                        StringArray { "Voix", "Percu", "Texture" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "input", 1 }, "Input",
                                                        StringArray { "Sidechain", "Fichier" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "wave", 1 }, "Wave",
                                                        StringArray { "Saw", "Square", "Triangle", "Sine" }, 0));
    return layout;
}

MimicProcessor::MimicProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Sidechain", AudioChannelSet::stereo(), true)
                          .withOutput ("Output", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "MIMIC", createLayout())
{
    morph = apvts.getRawParameterValue ("morph");
    carve = apvts.getRawParameterValue ("carve");
    groove = apvts.getRawParameterValue ("groove");
    wet = apvts.getRawParameterValue ("wet");
    output = apvts.getRawParameterValue ("output");
    blend = apvts.getRawParameterValue ("blend");
    freeze = apvts.getRawParameterValue ("freeze");
    source = apvts.getRawParameterValue ("source");
    input = apvts.getRawParameterValue ("input");
    wave = apvts.getRawParameterValue ("wave");
}

MimicProcessor::~MimicProcessor()
{
    delete pendingClip.exchange (nullptr);
    for (auto& retired : retiredClip)
        delete retired.exchange (nullptr);
    delete activeClip;
}

//==============================================================================
bool MimicProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != AudioChannelSet::stereo())
        return false;

    const auto in = layouts.getMainInputChannelSet();
    return in.isDisabled() || in == AudioChannelSet::mono() || in == AudioChannelSet::stereo();
}

void MimicProcessor::readParameters()
{
    dsp::ParamValues p;
    p.morph = morph->load (std::memory_order_relaxed);
    p.carve = carve->load (std::memory_order_relaxed);
    p.groove = groove->load (std::memory_order_relaxed);
    p.wet = wet->load (std::memory_order_relaxed);
    p.output = output->load (std::memory_order_relaxed);
    p.blend = blend->load (std::memory_order_relaxed);
    p.freeze = freeze->load (std::memory_order_relaxed) > 0.5f;
    p.source = jlimit (0, 2, roundToInt (source->load (std::memory_order_relaxed)));
    p.wave = jlimit (0, 3, roundToInt (wave->load (std::memory_order_relaxed)));
    engine.setParams (p);
}

void MimicProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    prepared = false;
    readParameters();
    engine.prepare (sampleRate, frameSizeFor (sampleRate)); // seule allocation du moteur : hors thread audio
    readParameters();

    const auto capacity = (size_t) jmax (256, samplesPerBlock);
    scratchL.assign (capacity, 0.0f);
    scratchR.assign (capacity, 0.0f);

    setLatencySamples (engine.latencySamples());
    license.refresh(); // les DAW repassent par ici juste avant un export

    // Un projet rouvert avant que le moteur soit prêt : le fichier est rechargé ici.
    String path;
    {
        const ScopedLock lock (clipLock);
        path = filePath;
    }

    if (path.isNotEmpty() && activeClip == nullptr && ! hasPending.load())
        loadClip (path, true);

    prepared = true;
}

//==============================================================================
void MimicProcessor::applyPendingClip()
{
    if (! hasPending.load (std::memory_order_acquire))
        return;

    hasPending.store (false, std::memory_order_relaxed);
    auto* next = pendingClip.exchange (nullptr, std::memory_order_acq_rel);

    if (next == nullptr && activeClip == nullptr)
        return;

    auto* old = activeClip;
    activeClip = next;
    player.setClip (activeClip);

    // Libéré hors du thread audio, au prochain chargement ou à la fermeture. Deux places : entre deux passages de
    // l'interface, le thread audio ne peut retirer que deux clips (celui d'avant et celui qui attendait encore).
    if (old != nullptr)
        if (auto* displaced = retiredClip[0].exchange (old, std::memory_order_acq_rel))
            retiredClip[1].store (displaced, std::memory_order_release);
}

void MimicProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    const ScopedNoDenormals noDenormals;
    const auto numSamples = buffer.getNumSamples();

    auto out = getBusBuffer (buffer, false, 0);

    if (! prepared || numSamples <= 0 || out.getNumChannels() < 2)
    {
        buffer.clear();
        return;
    }

    readParameters();

    // Notes, pédale et molette. La pédale et la molette prennent effet au début du bloc.
    int numEvents = 0;

    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();

        if (m.isNoteOn() && numEvents < kMaxEvents)
            events[(size_t) numEvents++] = { meta.samplePosition, true, m.getNoteNumber(), m.getFloatVelocity() };
        else if (m.isNoteOff() && numEvents < kMaxEvents)
            events[(size_t) numEvents++] = { meta.samplePosition, false, m.getNoteNumber(), 0.0f };
        else if (m.isSustainPedalOn())
            engine.setSustain (true);
        else if (m.isSustainPedalOff())
            engine.setSustain (false);
        else if (m.isPitchWheel())
            engine.setPitchBend (2.0f * (float) (m.getPitchWheelValue() - 8192) / 8192.0f);
        else if (m.isAllNotesOff() || m.isAllSoundOff())
            engine.allNotesOff();
    }

    midi.clear();
    applyPendingClip();

    // Le sidechain de l'hôte. Il partage sa mémoire avec la sortie : il est recopié avant que le moteur n'écrive.
    auto in = getBusBuffer (buffer, true, 0);
    const auto inChannels = in.getNumChannels();
    const auto fileMode = input->load (std::memory_order_relaxed) > 0.5f;
    const auto useFile = fileMode && player.hasClip();
    engine.setForceLive (useFile);

    auto* outL = out.getWritePointer (0);
    auto* outR = out.getWritePointer (1);
    const auto capacity = (int) scratchL.size();
    const auto sampleRate = getSampleRate();

    int done = 0, firstEvent = 0;

    while (done < numSamples)
    {
        auto length = jmin (numSamples - done, capacity);
        const float* scL = nullptr;
        const float* scR = nullptr;

        if (useFile)
        {
            // Chaque nouvelle note relance le fichier depuis le début, à l'échantillon près. Le do du milieu (60)
            // le joue à sa hauteur d'origine ; les autres notes le transposent sans en changer la durée.
            for (int k = firstEvent; k < numEvents && events[(size_t) k].offset <= done; ++k)
                if (events[(size_t) k].on)
                    player.trigger (std::pow (2.0, (jlimit (36, 84, events[(size_t) k].note) - 60) / 12.0));

            for (int k = firstEvent; k < numEvents; ++k)
                if (events[(size_t) k].on && events[(size_t) k].offset > done)
                {
                    length = jmin (length, events[(size_t) k].offset - done);
                    break;
                }

            player.fill (scratchL.data(), scratchR.data(), length, sampleRate);
            scL = scratchL.data();
            scR = scratchR.data();
        }
        else if (! fileMode && inChannels > 0)
        {
            FloatVectorOperations::copy (scratchL.data(), in.getReadPointer (0) + done, length);
            FloatVectorOperations::copy (scratchR.data(), in.getReadPointer (inChannels > 1 ? 1 : 0) + done, length);
            scL = scratchL.data();
            scR = scratchR.data();
        }

        int numChunk = 0;

        while (firstEvent < numEvents && events[(size_t) firstEvent].offset < done + length)
        {
            chunkEvents[(size_t) numChunk] = events[(size_t) firstEvent];
            chunkEvents[(size_t) numChunk].offset = jmax (0, events[(size_t) firstEvent].offset - done);
            ++numChunk;
            ++firstEvent;
        }

        engine.process (scL, scR, outL + done, outR + done, length, chunkEvents.data(), numChunk);
        done += length;
    }

    // Un hôte peut donner plus de deux canaux : les autres restent muets.
    for (int c = 2; c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, numSamples);

    license.process (buffer, sampleRate, isNonRealtime());

    activeVoices.store (engine.activeVoices(), std::memory_order_relaxed);

    if (const SpinLock::ScopedTryLockType lock (visualLock); lock.isLocked())
        visual = engine.visual();
}

dsp::VisFrame MimicProcessor::getVisual() const
{
    const SpinLock::ScopedLockType lock (visualLock);
    return visual;
}

//==============================================================================
void MimicProcessor::publishClip (const dsp::AudioClip* clip, const String& error)
{
    {
        const ScopedLock lock (clipLock);
        clipInfo = {};
        clipInfo.loaded = clip != nullptr;
        clipInfo.error = error;

        if (clip != nullptr)
        {
            clipInfo.name = String::fromUTF8 (clip->name.c_str());
            clipInfo.seconds = clip->seconds();
            std::copy (std::begin (clip->peaks), std::end (clip->peaks), clipInfo.peaks.begin());
        }
    }

    ++clipSerial;
}

/** Hors du thread audio (le décodage lit le disque). Le clip est remis au thread audio sans verrou. */
bool MimicProcessor::loadClip (const String& path, bool report)
{
    std::string error;
    std::unique_ptr<dsp::AudioClip> clip;

    if (path.isNotEmpty())
        clip = loadAudioClip (std::filesystem::u8path (path.toStdString()), error);

    for (auto& retired : retiredClip)
        delete retired.exchange (nullptr); // un clip retiré n'est plus lu par le thread audio

    if (clip == nullptr && path.isNotEmpty())
    {
        // Le fichier d'avant, s'il y en a un, reste en place : seule la raison du refus est montrée.
        if (report)
        {
            {
                const ScopedLock lock (clipLock);
                clipInfo.error = String::fromUTF8 (error.c_str());
            }

            ++clipSerial;
        }

        return false;
    }

    if (clip != nullptr)
        clip->name = File (path).getFileName().toStdString();

    auto* raw = clip.release();

    {
        const ScopedLock lock (clipLock);
        filePath = path;
    }

    if (report)
        publishClip (raw, {});

    delete pendingClip.exchange (raw, std::memory_order_acq_rel); // un clip en attente n'a jamais été vu par le thread audio
    hasPending.store (true, std::memory_order_release);
    return true;
}

bool MimicProcessor::loadFile (const File& file)
{
    if (! loadClip (file.getFullPathName(), true))
        return false;

    if (auto* p = apvts.getParameter ("input"))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (1.0f);
        p->endChangeGesture();
    }

    return true;
}

MimicProcessor::ClipInfo MimicProcessor::getClipInfo() const
{
    const ScopedLock lock (clipLock);
    return clipInfo;
}

//==============================================================================
void MimicProcessor::getStateInformation (MemoryBlock& destData)
{
    auto state = apvts.copyState();

    {
        const ScopedLock lock (clipLock);
        state.setProperty ("file", filePath, nullptr);
    }

    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void MimicProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto state = ValueTree::fromXml (*xml);
    const auto path = state.getProperty ("file").toString();
    state.removeProperty ("file", nullptr);
    apvts.replaceState (state);

    if (path.isNotEmpty())
    {
        // Un fichier déplacé ou supprimé depuis : le projet s'ouvre quand même, sur le sidechain.
        if (! loadClip (path, true))
        {
            const ScopedLock lock (clipLock);
            filePath = path;
        }
    }
    else
    {
        loadClip ({}, false);
        publishClip (nullptr, {});
    }
}

AudioProcessorEditor* MimicProcessor::createEditor() { return new MimicEditor (*this); }

} // namespace mimic

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new mimic::MimicProcessor(); }
