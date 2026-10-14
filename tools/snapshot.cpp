// Renders the editor to a PNG without a host, with a voice in the sidechain and a chord held,
// so a change to the interface can be looked at straight from the build.
//
//   mimic_snapshot out.png [width height] [--freeze] [--file some.wav] [--licence] [--set id=value ...]
//       --set   a parameter in its own units before the picture (source=2 wave=1 input=0 …), to see a button lit
//   mimic_snapshot --selftest [some.wav]    the plugin as a host drives it, without a window
#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{
constexpr double kSr = 48000.0;
constexpr int kBlock = 512;

std::unique_ptr<mimic::MimicProcessor> makeProcessor()
{
    return std::unique_ptr<mimic::MimicProcessor> (static_cast<mimic::MimicProcessor*> (createPluginFilter()));
}

/** A vowel: a pulse train through two resonances, with a slow tremolo so GROOVE has something to follow. */
std::vector<float> makeVowel (double seconds)
{
    const auto n = (size_t) (seconds * kSr);
    std::vector<float> out (n);
    double phase = 0.0, y1a = 0, y2a = 0, y1b = 0, y2b = 0;

    auto coefficients = [] (double f, double bw, double& a1, double& a2)
    {
        const auto r = std::exp (-juce::MathConstants<double>::pi * bw / kSr);
        a1 = 2.0 * r * std::cos (juce::MathConstants<double>::twoPi * f / kSr);
        a2 = -r * r;
    };

    double a1a, a2a, a1b, a2b;
    coefficients (700.0, 90.0, a1a, a2a);
    coefficients (1200.0, 130.0, a1b, a2b);

    for (size_t i = 0; i < n; ++i)
    {
        phase += 140.0 / kSr;
        const auto pulse = phase >= 1.0 ? 1.0 : 0.0;
        if (phase >= 1.0) phase -= 1.0;
        const auto ya = pulse + a1a * y1a + a2a * y2a; y2a = y1a; y1a = ya;
        const auto yb = pulse + a1b * y1b + a2b * y2b; y2b = y1b; y1b = yb;
        const auto tremolo = 0.6 + 0.4 * std::sin (juce::MathConstants<double>::twoPi * 3.0 * (double) i / kSr);
        out[i] = (float) ((ya + 0.7 * yb) * 0.05 * tremolo);
    }

    return out;
}

void play (mimic::MimicProcessor& processor, const std::vector<float>& sidechain, size_t& position, int blocks,
           const std::vector<int>& notesOn, float* peak = nullptr, bool* finite = nullptr)
{
    juce::AudioBuffer<float> buffer (2, kBlock);

    for (int b = 0; b < blocks; ++b)
    {
        juce::MidiBuffer midi;

        if (b == 0)
            for (const auto note : notesOn)
                midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 0);

        for (int i = 0; i < kBlock; ++i)
        {
            const auto s = sidechain.empty() ? 0.0f : sidechain[(position + (size_t) i) % sidechain.size()];
            buffer.setSample (0, i, s);
            buffer.setSample (1, i, s);
        }

        position += kBlock;
        processor.processBlock (buffer, midi);

        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < kBlock; ++i)
            {
                const auto v = buffer.getSample (c, i);
                if (finite != nullptr && ! std::isfinite (v)) *finite = false;
                if (peak != nullptr) *peak = juce::jmax (*peak, std::abs (v));
            }
    }
}

struct Director final : juce::Timer
{
    Director (juce::File destination, int width, int height, bool freeze, const juce::File& clip, bool showLicence,
              const juce::StringArray& settings = {})
        : file (std::move (destination)), freezeAtEnd (freeze)
    {
        processor = makeProcessor();
        processor->setPlayConfigDetails (2, 2, kSr, kBlock);
        processor->prepareToPlay (kSr, kBlock);
        for (const auto& setting : settings)
            if (auto* parameter = processor->apvts.getParameter (setting.upToFirstOccurrenceOf ("=", false, false)))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (setting.fromFirstOccurrenceOf ("=", false, false).getFloatValue()));
        signal = makeVowel (4.0);

        if (clip.existsAsFile())
            processor->loadFile (clip);

        editor.reset (processor->createEditor());
        editor->setSize (width, height);

        // The licence page covers a plugin that is not activated on this machine: set aside, to see the interface.
        if (! showLicence)
            for (auto* child : editor->getChildren())
                child->setVisible (false);

        startTimer (30);
    }

    void timerCallback() override
    {
        play (*processor, signal, position, 3, ticks == 0 ? std::vector<int> { 45, 52, 57 } : std::vector<int> {});

        if (freezeAtEnd && ticks == 40)
            processor->apvts.getParameter ("freeze")->setValueNotifyingHost (1.0f);

        if (++ticks > 60)
        {
            stopTimer();
            const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
            file.deleteFile();

            if (auto stream = file.createOutputStream())
                juce::PNGImageFormat().writeImageToStream (image, *stream);

            juce::MessageManager::getInstance()->stopDispatchLoop();
        }
    }

    juce::File file;
    std::unique_ptr<mimic::MimicProcessor> processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::vector<float> signal;
    size_t position = 0;
    int ticks = 0;
    bool freezeAtEnd = false;
};

/** The plugin as a host drives it. Returns the number of things found wrong. */
int selfTest (const juce::File& clip)
{
    int failures = 0;

    auto check = [&failures] (bool ok, const juce::String& what)
    {
        std::printf ("  %s  %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
        failures += ok ? 0 : 1;
    };

    const auto vowel = makeVowel (2.0);

    auto set = [] (mimic::MimicProcessor& p, const char* id, float value)
    {
        auto* parameter = p.apvts.getParameter (id);
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    };

    for (const auto rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        auto processor = makeProcessor();
        processor->setPlayConfigDetails (2, 2, rate, kBlock);
        processor->prepareToPlay (rate, kBlock);
        size_t position = 0;
        float peak = 0.0f;
        bool finite = true;
        play (*processor, vowel, position, 200, { 45, 52, 57, 60 }, &peak, &finite);
        check (finite && peak > 0.01f && peak <= 1.0f,
               juce::String (rate / 1000.0, 1) + " kHz: a chord sounds, stays finite and under full scale (peak " + juce::String (peak, 3)
                   + ", latency " + juce::String (processor->getLatencySamples()) + ")");
    }

    // Sixteen voices and more, every seal at its maximum: the output still never passes full scale.
    {
        auto processor = makeProcessor();
        processor->setPlayConfigDetails (2, 2, kSr, kBlock);
        processor->prepareToPlay (kSr, kBlock);
        set (*processor, "output", 1.0f);
        set (*processor, "morph", 1.0f);
        std::vector<int> notes;
        for (int n = 30; n < 54; ++n) notes.push_back (n);
        size_t position = 0;
        float peak = 0.0f;
        bool finite = true;
        play (*processor, vowel, position, 200, notes, &peak, &finite);
        check (finite && peak <= 1.0f, "24 notes at +6 dB stay under full scale (peak " + juce::String (peak, 3) + ")");
    }

    // FREEZE on silence must not mute the synth.
    {
        auto processor = makeProcessor();
        processor->setPlayConfigDetails (2, 2, kSr, kBlock);
        processor->prepareToPlay (kSr, kBlock);
        set (*processor, "freeze", 1.0f);
        size_t position = 0;
        float peak = 0.0f;
        play (*processor, {}, position, 150, { 57 }, &peak);
        check (peak > 0.01f, "FREEZE pressed on silence leaves the oscillators playing (peak " + juce::String (peak, 3) + ")");
    }

    // The state a host saves, with the dropped file.
    {
        auto first = makeProcessor();
        first->setPlayConfigDetails (2, 2, kSr, kBlock);
        first->prepareToPlay (kSr, kBlock);
        set (*first, "morph", 0.25f);
        set (*first, "wave", 2.0f);
        set (*first, "source", 1.0f);

        if (clip.existsAsFile())
        {
            check (first->loadFile (clip), "a dropped file is accepted");
            check (first->apvts.getRawParameterValue ("input")->load() > 0.5f && first->getClipInfo().loaded, "and the input switches to the file");

            size_t position = 0;
            float peak = 0.0f;
            bool finite = true;
            play (*first, {}, position, 100, { 60 }, &peak, &finite);
            check (finite && peak > 0.005f, "a note plays the file through the synth (peak " + juce::String (peak, 3) + ")");

            check (! first->loadFile (clip.getSiblingFile ("does-not-exist.wav")) && first->getClipInfo().error.isNotEmpty(),
                   "a file that cannot be read is refused with a reason");
        }

        juce::MemoryBlock saved;
        first->getStateInformation (saved);

        auto second = makeProcessor();
        second->setStateInformation (saved.getData(), (int) saved.getSize());
        second->setPlayConfigDetails (2, 2, kSr, kBlock);
        second->prepareToPlay (kSr, kBlock);

        auto get = [&] (const char* id) { return second->apvts.getRawParameterValue (id)->load(); };
        check (std::abs (get ("morph") - 0.25f) < 1.0e-4f && juce::roundToInt (get ("wave")) == 2 && juce::roundToInt (get ("source")) == 1,
               "a restored state has the same settings");

        if (clip.existsAsFile())
        {
            size_t position = 0;
            float peak = 0.0f;
            play (*second, {}, position, 100, { 60 }, &peak);
            check (peak > 0.005f, "and plays the same file");
        }
    }

    // Mono sidechain, no sidechain at all, and a block size that changes.
    for (const auto channels : { 0, 1 })
    {
        auto processor = makeProcessor();
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add (channels == 0 ? juce::AudioChannelSet::disabled() : juce::AudioChannelSet::mono());
        layout.outputBuses.add (juce::AudioChannelSet::stereo());
        const auto accepted = processor->setBusesLayout (layout);
        processor->setRateAndBufferSizeDetails (kSr, kBlock);
        processor->prepareToPlay (kSr, kBlock);

        bool finite = true;
        float peak = 0.0f;

        for (const auto size : { 1, 7, 64, 512, 2048, 33 })
        {
            juce::AudioBuffer<float> buffer (2, size);
            buffer.clear();
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 50, 0.9f), 0);
            processor->processBlock (buffer, midi);

            for (int i = 0; i < size; ++i)
            {
                finite = finite && std::isfinite (buffer.getSample (0, i)) && std::isfinite (buffer.getSample (1, i));
                peak = juce::jmax (peak, std::abs (buffer.getSample (0, i)));
            }
        }

        check (accepted && finite, juce::String (channels == 0 ? "no sidechain bus" : "a mono sidechain") + ", blocks of 1 to 2048 samples: finite");
    }

    std::printf (failures == 0 ? "all good\n" : "%d failure(s)\n", failures);
    return failures;
}
}

int main (int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI gui;
    const juce::StringArray args (argv + 1, argc - 1);

    if (args.contains ("--selftest"))
        return selfTest (args.size() > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (args[1]) : juce::File());

    if (args.isEmpty())
    {
        std::printf ("usage: mimic_snapshot out.png [width height] [--freeze] [--file some.wav]\n");
        return 1;
    }

    const auto fileAt = args.indexOf ("--file");
    const auto clip = fileAt >= 0 && fileAt + 1 < args.size() ? juce::File::getCurrentWorkingDirectory().getChildFile (args[fileAt + 1]) : juce::File();
    const auto sized = args.size() >= 3 && args[1].containsOnly ("0123456789") && args[2].containsOnly ("0123456789");

    Director director (juce::File::getCurrentWorkingDirectory().getChildFile (args[0]),
                       sized ? args[1].getIntValue() : mimic::MimicEditor::kBaseWidth,
                       sized ? args[2].getIntValue() : mimic::MimicEditor::kBaseHeight,
                       args.contains ("--freeze"), clip, args.contains ("--licence"), [&args]
                       {
                           juce::StringArray settings;
                           for (int i = 0; i + 1 < args.size(); ++i)
                               if (args[i] == "--set")
                                   settings.add (args[i + 1]);
                           return settings;
                       }());

    juce::MessageManager::getInstance()->runDispatchLoop();
    return 0;
}
