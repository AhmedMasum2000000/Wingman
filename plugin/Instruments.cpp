#include "Instruments.h"
#include <map>
#include <mutex>

namespace instruments
{
const char* const builtInName = "Synth";

namespace
{
constexpr double maxSampleSeconds = 12.0, endFadeSeconds = 0.01;

//==============================================================================
// Built-in synth: two detuned saws (chords) or sine plus a little saw (bass) through a one-pole low-pass.
struct SynthSound : juce::SynthesiserSound
{
    bool appliesToNote (int) override { return true; }
    bool appliesToChannel (int) override { return true; }
};

class SynthVoice : public juce::SynthesiserVoice
{
public:
    explicit SynthVoice (bool isBass) : bass (isBass) {}

    bool canPlaySound (juce::SynthesiserSound* sound) override { return dynamic_cast<SynthSound*> (sound) != nullptr; }

    void startNote (int midiNote, float velocity, juce::SynthesiserSound*, int) override
    {
        const double sr = getSampleRate();
        const double freq = juce::MidiMessage::getMidiNoteInHertz (midiNote);
        inc1 = freq / sr;
        inc2 = freq * 1.006 / sr;
        phase1 = 0.0; phase2 = 0.3;
        level = velocity * (bass ? 0.35f : 0.12f);
        coeff = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * (bass ? 500.0f : 2200.0f) / (float) sr);
        filterState = 0.0f;
        adsr.setSampleRate (sr);
        adsr.setParameters ({ 0.005f, bass ? 0.2f : 0.4f, bass ? 0.8f : 0.6f, bass ? 0.08f : 0.25f });
        adsr.noteOn();
    }

    void stopNote (float, bool allowTailOff) override
    {
        if (allowTailOff) { adsr.noteOff(); return; }
        adsr.reset();
        clearCurrentNote();
    }

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}
    using juce::SynthesiserVoice::renderNextBlock;

    void renderNextBlock (juce::AudioBuffer<float>& output, int startSample, int numSamples) override
    {
        if (! isVoiceActive())
            return;
        for (int i = 0; i < numSamples; ++i)
        {
            const float saw1 = (float) (2.0 * phase1 - 1.0), saw2 = (float) (2.0 * phase2 - 1.0);
            const float raw = bass ? std::sin (juce::MathConstants<float>::twoPi * (float) phase1) + 0.3f * saw1
                                   : 0.5f * (saw1 + saw2);
            filterState += coeff * (raw - filterState);
            const float sample = filterState * level * adsr.getNextSample();
            for (int ch = 0; ch < output.getNumChannels(); ++ch)
                output.addSample (ch, startSample + i, sample);
            if ((phase1 += inc1) >= 1.0) phase1 -= 1.0;
            if ((phase2 += inc2) >= 1.0) phase2 -= 1.0;
            if (! adsr.isActive()) { clearCurrentNote(); break; }
        }
    }

private:
    const bool bass;
    double phase1 = 0, phase2 = 0, inc1 = 0, inc2 = 0;
    float level = 0, coeff = 0, filterState = 0;
    juce::ADSR adsr;
};

//==============================================================================
// Sampled instrument: the zone nearest to the note, resampled with 4-point Hermite interpolation.
struct SampledSound : juce::SynthesiserSound
{
    explicit SampledSound (InstrumentPtr i) : instrument (std::move (i)) {}
    bool appliesToNote (int note) override { return note >= 0 && note < 128; }
    bool appliesToChannel (int) override { return true; }
    const Zone* zoneFor (int note) const
    {
        if (note < 0 || note > 127)
            return nullptr;
        const int index = instrument->zoneForNote[(size_t) note];
        return index >= 0 && index < (int) instrument->zones.size() ? &instrument->zones[(size_t) index] : nullptr;
    }
    const InstrumentPtr instrument;
};

class SampledVoice : public juce::SynthesiserVoice
{
public:
    explicit SampledVoice (bool isBass) : bass (isBass) {}

    bool canPlaySound (juce::SynthesiserSound* sound) override { return dynamic_cast<SampledSound*> (sound) != nullptr; }

    void startNote (int midiNote, float velocity, juce::SynthesiserSound* sound, int) override
    {
        auto* sampled = dynamic_cast<SampledSound*> (sound);
        zone = sampled != nullptr ? sampled->zoneFor (midiNote) : nullptr;
        if (zone == nullptr || zone->audio.getNumSamples() == 0)
        {
            zone = nullptr;
            clearCurrentNote();
            return;
        }
        position = 0.0;
        increment = std::pow (2.0, (midiNote - zone->rootNote) / 12.0) * zone->sampleRate / getSampleRate();
        gain = velocity * (bass ? 0.5f : 0.2f);
        adsr.setSampleRate (getSampleRate());
        adsr.setParameters ({ 0.002f, 0.05f, 1.0f, bass ? 0.12f : 0.3f });
        adsr.noteOn();
    }

    void stopNote (float, bool allowTailOff) override
    {
        if (allowTailOff) { adsr.noteOff(); return; }
        adsr.reset();
        zone = nullptr;
        clearCurrentNote();
    }

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}
    using juce::SynthesiserVoice::renderNextBlock;

    void renderNextBlock (juce::AudioBuffer<float>& output, int startSample, int numSamples) override
    {
        if (! isVoiceActive() || zone == nullptr)
            return;
        const int length = zone->audio.getNumSamples(), channels = zone->audio.getNumChannels();
        for (int i = 0; i < numSamples; ++i)
        {
            if (position >= (double) length || ! adsr.isActive())
            {
                adsr.reset();
                zone = nullptr;
                clearCurrentNote();
                break;
            }
            const float env = gain * adsr.getNextSample();
            for (int ch = 0; ch < output.getNumChannels(); ++ch)
                output.addSample (ch, startSample + i,
                                  env * interpolate (zone->audio.getReadPointer (juce::jmin (ch, channels - 1)), length, position));
            position += increment;
        }
    }

private:
    const bool bass;
    const Zone* zone = nullptr;
    double position = 0.0, increment = 1.0;
    float gain = 0.0f;
    juce::ADSR adsr;
};

bool isLetter (juce::juce_wchar c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit (juce::juce_wchar c)  { return c >= '0' && c <= '9'; }
}

//==============================================================================
float interpolate (const float* data, int length, double position) noexcept
{
    const double floorPos = std::floor (position);
    const int i = (int) floorPos;
    const float t = (float) (position - floorPos);
    auto at = [data, length] (int k) { return k >= 0 && k < length ? data[k] : 0.0f; };
    const float xm1 = at (i - 1), x0 = at (i), x1 = at (i + 1), x2 = at (i + 2);
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

InstrumentPtr builtIn()
{
    static const InstrumentPtr synth = []
    {
        auto i = std::make_shared<Instrument>();
        i->name = builtInName;
        i->zoneForNote.fill (-1);
        return InstrumentPtr (std::move (i));
    }();
    return synth;
}

juce::File defaultSoundsFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Backline").getChildFile ("Sounds");
}

int noteFromFileName (const juce::String& name)
{
    const int length = name.length();
    static const int pitchClasses[7] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
    int found = -1;
    for (int i = 0; i < length; ++i)
    {
        const juce::juce_wchar letter = juce::CharacterFunctions::toUpperCase (name[i]);
        if (letter < 'A' || letter > 'G' || (i > 0 && isLetter (name[i - 1])))
            continue;
        int j = i + 1, shift = 0;
        if (j < length && (name[j] == '#' || name[j] == 's')) { shift = 1; ++j; }
        else if (j < length && name[j] == 'b') { shift = -1; ++j; }
        bool negative = false;
        if (j < length && name[j] == '-') { negative = true; ++j; }
        if (j >= length || ! isDigit (name[j]) || (j + 1 < length && isDigit (name[j + 1])))
            continue;
        const int octave = (int) (name[j] - '0') * (negative ? -1 : 1);
        const int note = (octave + 1) * 12 + pitchClasses[letter - 'A'] + shift;
        if (note >= 0 && note <= 127)
            found = note;
    }
    if (found >= 0)
        return found;

    // No note name: a number, preferring one written with two or more digits.
    int firstShort = -1;
    for (int i = 0; i < length;)
    {
        if (! isDigit (name[i])) { ++i; continue; }
        int j = i, value = 0;
        while (j < length && isDigit (name[j]) && value < 1000)
            value = value * 10 + (int) (name[j++] - '0');
        while (j < length && isDigit (name[j]))
            ++j;
        if (value <= 127)
        {
            if (j - i >= 2)
                return value;
            if (firstShort < 0)
                firstShort = value;
        }
        i = j;
    }
    return firstShort;
}

bool isSampleFile (const juce::File& file)
{
    return file.existsAsFile() && file.hasFileExtension ("wav;aif;aiff;flac");
}

static juce::Array<juce::File> sampleFilesIn (const juce::File& folder)
{
    juce::Array<juce::File> result;
    for (const auto& entry : juce::RangedDirectoryIterator (folder, false, "*", juce::File::findFiles))
    {
        const auto file = entry.getFile();
        if (isSampleFile (file) && ! file.isHidden() && noteFromFileName (file.getFileNameWithoutExtension()) >= 0)
            result.add (file);
    }
    juce::File::NaturalFileComparator comparator { false };
    result.sort (comparator);
    return result;
}

static bool isValidInstrumentName (const juce::File& root, const juce::String& name)
{
    return name.isNotEmpty() && ! name.equalsIgnoreCase (builtInName) && ! name.containsAnyOf ("/\\:")
        && name != "." && name != ".." && root.getChildFile (name).getParentDirectory() == root;
}

juce::StringArray scanInstruments (const juce::File& root)
{
    juce::StringArray names;
    if (! root.isDirectory())
        return names;
    for (const auto& entry : juce::RangedDirectoryIterator (root, false, "*", juce::File::findDirectories))
    {
        const auto folder = entry.getFile();
        if (! folder.isHidden() && isValidInstrumentName (root, folder.getFileName()) && ! sampleFilesIn (folder).isEmpty())
            names.add (folder.getFileName());
    }
    names.sortNatural();
    return names;
}

InstrumentPtr loadInstrument (const juce::File& root, const juce::String& name)
{
    if (! isValidInstrumentName (root, name))
        return nullptr;
    const auto folder = root.getChildFile (name);
    if (! folder.isDirectory())
        return nullptr;
    const auto files = sampleFilesIn (folder);

    // Reuse an instrument still in use if the folder holds the same files as when it was loaded.
    juce::String signature = folder.getFullPathName();
    for (const auto& file : files)
        signature << "|" << file.getFileName() << ":" << file.getSize() << ":" << file.getLastModificationTime().toMilliseconds();
    static std::mutex cacheLock;
    static std::map<juce::String, std::weak_ptr<const Instrument>> cache;
    {
        const std::lock_guard<std::mutex> lock (cacheLock);
        for (auto it = cache.begin(); it != cache.end();)
            it = it->second.expired() ? cache.erase (it) : std::next (it);
        const auto found = cache.find (signature);
        if (found != cache.end())
            if (auto cached = found->second.lock())
                return cached;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    auto instrument = std::make_shared<Instrument>();
    instrument->name = name;
    std::array<bool, 128> used {};
    float peak = 0.0f;
    for (const auto& file : files)
    {
        const int note = noteFromFileName (file.getFileNameWithoutExtension());
        if (note < 0 || note > 127 || used[(size_t) note]) // first file for each note wins (e.g. one velocity layer)
            continue;
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0 || reader->numChannels == 0)
            continue;
        const auto length = (int) juce::jmin (reader->lengthInSamples, (juce::int64) (reader->sampleRate * maxSampleSeconds));
        Zone zone;
        zone.rootNote = note;
        zone.sampleRate = reader->sampleRate;
        zone.audio.setSize (reader->numChannels > 1 ? 2 : 1, length);
        if (! reader->read (&zone.audio, 0, length, 0, true, reader->numChannels > 1))
            continue;
        const int fade = juce::jmin (length, (int) (endFadeSeconds * reader->sampleRate) + 1);
        zone.audio.applyGainRamp (length - fade, fade, 1.0f, 0.0f); // no click where a held note runs out
        peak = juce::jmax (peak, zone.audio.getMagnitude (0, length));
        used[(size_t) note] = true;
        instrument->zones.push_back (std::move (zone));
    }
    if (instrument->zones.empty() || peak <= 1.0e-6f)
        return nullptr;

    std::sort (instrument->zones.begin(), instrument->zones.end(),
               [] (const Zone& a, const Zone& b) { return a.rootNote < b.rootNote; });
    const size_t count = instrument->zones.size();
    for (size_t i = 0; i < count; ++i)
    {
        auto& zone = instrument->zones[i];
        zone.audio.applyGain (1.0f / peak);
        // Each sample covers the notes nearer to it than to its neighbours (ties go to the higher
        // sample, which is shifted down rather than up).
        zone.lowNote = i == 0 ? 0 : (instrument->zones[i - 1].rootNote + zone.rootNote) / 2 + 1;
        zone.highNote = i + 1 == count ? 127 : (zone.rootNote + instrument->zones[i + 1].rootNote) / 2;
        for (int n = zone.lowNote; n <= zone.highNote; ++n)
            instrument->zoneForNote[(size_t) n] = (int) i;
    }
    InstrumentPtr result (std::move (instrument));
    const std::lock_guard<std::mutex> lock (cacheLock);
    cache[signature] = result;
    return result;
}

std::unique_ptr<juce::Synthesiser> createSynth (const InstrumentPtr& instrument, bool isBass, double sampleRate)
{
    auto synth = std::make_unique<juce::Synthesiser>();
    const bool sampled = instrument != nullptr && instrument->isSampled();
    const int voices = isBass ? 4 : 16;
    for (int i = 0; i < voices; ++i)
    {
        if (sampled)
            synth->addVoice (new SampledVoice (isBass));
        else
            synth->addVoice (new SynthVoice (isBass));
    }
    if (sampled)
        synth->addSound (new SampledSound (instrument));
    else
        synth->addSound (new SynthSound());
    synth->setCurrentPlaybackSampleRate (sampleRate);
    return synth;
}

juce::AudioBuffer<float> renderNotes (const std::vector<backline::Note>& notes, double loopBeats, double bpm,
                                      double sampleRate, const InstrumentPtr& instrument, bool isBass,
                                      double maxTailSeconds)
{
    const double samplesPerBeat = sampleRate * 60.0 / juce::jmax (1.0, bpm);
    const int loopSamples = juce::jmax (1, (int) std::llround (juce::jmax (0.0, loopBeats) * samplesPerBeat));
    const int tailSamples = juce::jmax (0, (int) (maxTailSeconds * sampleRate));
    juce::AudioBuffer<float> out (2, loopSamples + tailSamples);
    out.clear();

    juce::MidiBuffer midi;
    auto sampleAt = [&] (double beat) { return juce::jlimit (0, loopSamples - 1, (int) std::llround (beat * samplesPerBeat)); };
    for (const auto& n : notes) // note-offs first, so a repeated pitch retriggers cleanly
        if (n.start < loopBeats)
            midi.addEvent (juce::MidiMessage::noteOff (1, n.pitch), sampleAt (juce::jmin (n.start + n.length, loopBeats)));
    for (const auto& n : notes)
        if (n.start < loopBeats && n.start + n.length > n.start)
            midi.addEvent (juce::MidiMessage::noteOn (1, n.pitch, (juce::uint8) juce::jlimit (1, 127, n.velocity)), sampleAt (n.start));

    auto synth = createSynth (instrument, isBass, sampleRate);
    const int block = 512;
    for (int start = 0; start < out.getNumSamples(); start += block)
    {
        const int count = juce::jmin (block, out.getNumSamples() - start);
        juce::MidiBuffer events;
        events.addEvents (midi, start, count, -start);
        juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, start, count);
        synth->renderNextBlock (view, events, 0, count);
    }

    // Keep the release tail only as long as it is audible.
    int end = loopSamples;
    for (int i = out.getNumSamples() - 1; i >= loopSamples; --i)
        if (std::abs (out.getSample (0, i)) > 1.0e-4f || std::abs (out.getSample (1, i)) > 1.0e-4f)
        {
            end = juce::jmin (out.getNumSamples(), i + 1 + (int) (0.01 * sampleRate));
            break;
        }
    out.setSize (2, end, true);

    const float peakLevel = out.getMagnitude (0, end);
    if (peakLevel > 0.98f)
        out.applyGain (0.98f / peakLevel);
    return out;
}
}
