#include "WebEngine.h"

namespace m1d1
{
namespace
{
    // The preview sounds match the interface's: a round bass, a soft pad, a plucked lead.
    struct Sound { int wave; float gain, cutoff, attack, release; };   // wave: 0 saw, 1 triangle, 2 square
    constexpr std::array<Sound, 3> sounds { { { 0, 0.17f, 650.0f, 0.005f, 0.06f },
                                              { 1, 0.10f, 2400.0f, 0.03f, 0.25f },
                                              { 2, 0.06f, 2800.0f, 0.005f, 0.12f } } };

    double positiveMod (double x, double m) { const double r = std::fmod (x, m); return r < 0 ? r + m : r; }
}

std::shared_ptr<Song> parseSong (const juce::String& json)
{
    const auto v = juce::JSON::parse (json);
    if (! v.isObject())
        return nullptr;
    auto song = std::make_shared<Song>();
    song->loopBeats = juce::jlimit (1.0, 256.0, (double) v.getProperty ("loopBeats", 16.0));
    song->tempo = juce::jlimit (20.0, 999.0, (double) v.getProperty ("tempo", 120.0));
    const auto* parts = v.getProperty ("parts", juce::var()).getArray();
    if (parts == nullptr)
        return song;
    for (int i = 0; i < juce::jmin (3, parts->size()); ++i)
    {
        const auto& p = parts->getReference (i);
        auto& part = song->parts[(size_t) i];
        part.channel = juce::jlimit (1, 16, (int) p.getProperty ("ch", i == 1 ? 1 : i == 0 ? 2 : 3));
        part.sound = (bool) p.getProperty ("sound", false);
        part.midi = (bool) p.getProperty ("midi", true);
        if (const auto* notes = p.getProperty ("notes", juce::var()).getArray())
            for (const auto& n : *notes)
            {
                const auto* f = n.getArray();
                if (f == nullptr || f->size() < 4 || part.notes.size() >= 4096)
                    continue;
                Note note;
                note.start = (double) f->getReference (0);
                note.length = (double) f->getReference (1);
                note.pitch = (int) f->getReference (2);
                note.velocity = (int) f->getReference (3);
                if (note.start < 0 || note.start >= song->loopBeats || note.length <= 0 || note.pitch < 0 || note.pitch > 127)
                    continue;
                note.velocity = juce::jlimit (1, 127, note.velocity);
                part.notes.push_back (note);
            }
    }
    return song;
}

Engine::Engine() { ring.assign ((size_t) (captureSeconds * 24000.0), 0.0f); }

void Engine::prepare (double newRate, int)
{
    sampleRate = newRate > 0 ? newRate : 44100.0;
    const juce::SpinLock::ScopedLockType lock (captureLock);
    decim = juce::jmax (1, (int) std::round (sampleRate / 22050.0));
    ring.assign ((size_t) (captureSeconds * sampleRate / decim) + 1, 0.0f);
    ringWrite = ringCount = ringTotal = 0;
    decimCount = 0; decimSum = 0;
    for (auto& v : voices) v.part = -1;
}

void Engine::setSong (std::shared_ptr<Song> song)
{
    if (song == nullptr)
        return;
    keepAlive.push_back (song);
    current.store (song.get());
    while (keepAlive.size() > 16)   // the audio thread only ever holds the newest; a few older stay for safety
        keepAlive.pop_front();
}

void Engine::setPreview (bool run, double beat, double tempo)
{
    previewRequest = run;
    previewStartBeat = beat;
    previewTempo = juce::jlimit (20.0, 999.0, tempo);
    ++previewTicket;
}

void Engine::collectEvents (const Song& song, double from, double to, int numSamples)
{
    const double L = song.loopBeats, span = to - from;
    if (span <= 0 || L <= 0)
        return;
    auto add = [&] (double rel, int part, int pitch, int velocity, bool on)
    {
        // every time `rel` (a position in the loop) falls in [from, to)
        double t = rel + std::ceil ((from - rel) / L) * L;
        for (; t < to && numEvents < (int) events.size(); t += L)
            events[(size_t) numEvents++] = { juce::jlimit (0, numSamples - 1, (int) ((t - from) / span * numSamples)), part, pitch, velocity, on };
    };
    for (int i = 0; i < 3; ++i)
        for (const auto& n : song.parts[(size_t) i].notes)
        {
            const double end = juce::jmax (n.start + 0.01, juce::jmin (n.start + n.length, L) - 0.002);
            add (end, i, n.pitch, 0, false);
            add (n.start, i, n.pitch, n.velocity, true);
        }
    // in time order; at the same sample, note-offs first so a repeated pitch retriggers
    std::sort (events.begin(), events.begin() + numEvents, [] (const Event& a, const Event& b)
               { return a.offset != b.offset ? a.offset < b.offset : (! a.on && b.on); });
}

void Engine::startVoice (int part, int pitch, int velocity)
{
    Voice* v = nullptr;
    for (auto& c : voices) if (c.part < 0) { v = &c; break; }
    if (v == nullptr)   // all busy: take the quietest
    {
        v = &voices[0];
        for (auto& c : voices) if (c.level < v->level) v = &c;
    }
    const auto& s = sounds[(size_t) part];
    *v = {};
    v->part = part; v->pitch = pitch;
    v->inc = 440.0 * std::pow (2.0, (pitch - 69) / 12.0) / sampleRate;
    v->gain = s.gain * (0.35f + 0.65f * (float) velocity / 127.0f);
}

void Engine::stopVoice (int part, int pitch)
{
    for (auto& v : voices)
        if (v.part == part && v.pitch == pitch && ! v.releasing)
            v.releasing = true;
}

void Engine::render (juce::AudioBuffer<float>& buffer, int start, int end)
{
    const int channels = buffer.getNumChannels();
    for (auto& v : voices)
    {
        if (v.part < 0)
            continue;
        const auto& s = sounds[(size_t) v.part];
        const float a = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * s.cutoff / (float) sampleRate);
        const float up = 1.0f / juce::jmax (1.0f, s.attack * (float) sampleRate);
        const float down = std::exp (-1.0f / juce::jmax (1.0f, s.release * 0.6f * (float) sampleRate));
        for (int i = start; i < end; ++i)
        {
            if (v.releasing) { v.level *= down; if (v.level < 1.0e-4f) { v.part = -1; break; } }
            else v.level = juce::jmin (1.0f, v.level + up);
            const float ph = (float) v.phase;
            float x = s.wave == 0 ? 2.0f * ph - 1.0f : s.wave == 1 ? 1.0f - 4.0f * std::abs (ph - 0.5f) : (ph < 0.5f ? 0.6f : -0.6f);
            v.phase += v.inc; if (v.phase >= 1.0) v.phase -= 1.0;
            v.z1 += a * (x - v.z1); v.z2 += a * (v.z1 - v.z2);
            const float out = v.z2 * v.gain * v.level;
            for (int ch = 0; ch < channels; ++ch)
                buffer.addSample (ch, i, out);
        }
    }
}

void Engine::releaseHeld (juce::MidiBuffer& midi, int offset)
{
    for (int p = 0; p < 3; ++p)
        for (int n = 0; n < 128; ++n)
            if (held[(size_t) p][(size_t) n])
            {
                midi.addEvent (juce::MidiMessage::noteOff (heldChannel[(size_t) p], n), offset);
                held[(size_t) p][(size_t) n] = false;
            }
    for (auto& v : voices)
        if (v.part >= 0) v.releasing = true;
}

void Engine::stopAll (juce::MidiBuffer& midi)
{
    releaseHeld (midi, 0);
    for (auto& v : voices) v.part = -1;
    wasPlaying = false;
    expectedBeat = -1.0;
}

void Engine::process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, juce::AudioPlayHead* playHead, int numInputChannels)
{
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    bool hostPlaying = false, hasPpq = false;
    double ppq = 0, bpm = 120, beatsPerBar = 4;
    if (playHead != nullptr)
        if (auto pos = playHead->getPosition())
        {
            hostPlaying = pos->getIsPlaying();
            if (auto b = pos->getBpm()) bpm = juce::jlimit (20.0, 999.0, *b);
            if (auto p = pos->getPpqPosition()) { ppq = *p; hasPpq = true; }
            if (auto sig = pos->getTimeSignature()) beatsPerBar = juce::jmax (1.0, sig->numerator * 4.0 / juce::jmax (1, sig->denominator));
        }
    hostPlaying = hostPlaying && hasPpq;

    // the track's audio, while the host plays (a seek starts a new recording)
    {
        const double blockBeats = numSamples * bpm / (60.0 * sampleRate);
        const juce::SpinLock::ScopedTryLockType lock (captureLock);
        if (lock.isLocked() && ! ring.empty())
        {
            bool jumped = hostPlaying && captureWasPlaying && std::abs (ppq - captureExpectedPpq) > std::max (0.01, 0.5 * blockBeats);
            if (jumped)
            {
                // A loop in the host (FL Studio's pattern loop, a looped playlist range) jumps back to a bar line from a
                // bar line: the music carries on, so the recording does too, as if the song went on.
                const auto onBar = [&] (double b) { const double r = positiveMod (b, beatsPerBar); return r < 0.05 || r > beatsPerBar - 0.05; };
                const double span = captureExpectedPpq - ppq;
                if (span > 0.5 && onBar (ppq) && onBar (captureExpectedPpq) && onBar (span))
                    jumped = false;
            }
            if (hostPlaying && (! captureWasPlaying || jumped || std::abs (bpm - captureBpm) > 0.01))
            {
                ringWrite = ringCount = ringTotal = 0; decimCount = 0; decimSum = 0;
                captureStartPpq = ppq; captureBpm = bpm; captureBeatsPerBar = beatsPerBar;
            }
            if (hostPlaying)
            {
                const int inputs = juce::jmin (numInputChannels, buffer.getNumChannels());
                for (int i = 0; i < numSamples; ++i)
                {
                    float mix = 0;
                    for (int ch = 0; ch < inputs; ++ch) mix += buffer.getSample (ch, i);
                    decimSum += mix / (float) juce::jmax (1, inputs);
                    if (++decimCount == decim)
                    {
                        ring[ringWrite] = decimSum / (float) decim;
                        ringWrite = (ringWrite + 1) % ring.size();
                        ringCount = juce::jmin (ringCount + 1, ring.size());
                        ++ringTotal;
                        decimCount = 0; decimSum = 0;
                    }
                }
                captureExpectedPpq = ppq + blockBeats;
                const double rate = sampleRate / decim;
                capturedFromBeat = captureStartPpq + (double) (ringTotal - ringCount) / rate * captureBpm / 60.0;
                capturedBeats = (double) ringCount / rate * captureBpm / 60.0;
            }
            captureWasPlaying = hostPlaying;
        }
    }

    // the interface's Play while the host is stopped
    if (const int ticket = previewTicket.load(); ticket != previewSeen)
    {
        previewSeen = ticket;
        previewRunning = previewRequest.load();
        previewBeat = previewStartBeat.load();
    }
    const auto* song = current.load();
    bool playing = false;
    double tempo = bpm, b0 = 0;
    if (hostPlaying) { playing = true; b0 = ppq; }
    else if (previewRunning) { playing = true; tempo = previewTempo.load(); b0 = previewBeat; }
    const double blockBeats = numSamples * tempo / (60.0 * sampleRate);

    numEvents = 0;
    if (song != lastSong)
    {
        releaseHeld (midi, 0);   // new notes: start clean
        lastSong = song;
    }
    if (song != nullptr)
        for (int p = 0; p < 3; ++p)
        {
            heldChannel[(size_t) p] = song->parts[(size_t) p].channel;
            if (! song->parts[(size_t) p].sound)
                for (auto& v : voices) if (v.part == p) v.releasing = true;
        }

    if (playing && song != nullptr)
    {
        if (! wasPlaying || std::abs (b0 - expectedBeat) > std::max (0.01, 0.5 * blockBeats))
            releaseHeld (midi, 0);   // a start, a seek or a loop jump in the host
        collectEvents (*song, b0, b0 + blockBeats, numSamples);
        expectedBeat = b0 + blockBeats;
        if (! hostPlaying) previewBeat += blockBeats;
    }
    else if (wasPlaying)
    {
        releaseHeld (midi, 0);
        expectedBeat = -1.0;
    }
    wasPlaying = playing && song != nullptr;

    int at = 0;
    for (int e = 0; e < numEvents; ++e)
    {
        const auto& ev = events[(size_t) e];
        render (buffer, at, ev.offset);
        at = ev.offset;
        auto& isHeld = held[(size_t) ev.part][(size_t) ev.pitch];
        const int ch = song->parts[(size_t) ev.part].channel;
        if (ev.on)
        {
            if (isHeld) midi.addEvent (juce::MidiMessage::noteOff (ch, ev.pitch), ev.offset);
            isHeld = song->parts[(size_t) ev.part].midi;   // a part switched off in MIDI Out sends nothing
            if (isHeld) midi.addEvent (juce::MidiMessage::noteOn (ch, ev.pitch, (juce::uint8) ev.velocity), ev.offset);
            if (song->parts[(size_t) ev.part].sound)
            {
                stopVoice (ev.part, ev.pitch);
                startVoice (ev.part, ev.pitch, ev.velocity);
            }
        }
        else if (isHeld)
        {
            midi.addEvent (juce::MidiMessage::noteOff (ch, ev.pitch), ev.offset);
            isHeld = false;
            stopVoice (ev.part, ev.pitch);
        }
    }
    render (buffer, at, numSamples);

    reportHostPlaying = hostPlaying;
    reportPreview = playing && ! hostPlaying;
    reportBpm = tempo;
    if (playing) reportBeat = b0 + blockBeats;
}

Engine::Capture Engine::getCapture()
{
    Capture c;
    const juce::SpinLock::ScopedLockType lock (captureLock);
    const double rate = sampleRate / decim;
    if (ringCount < (size_t) (rate * 4.0))
        return c;
    // where the oldest kept sample sits on the host's timeline, then skip to the next bar line
    const double firstPpq = captureStartPpq + (double) (ringTotal - ringCount) / rate * captureBpm / 60.0;
    double skip = positiveMod (-firstPpq, captureBeatsPerBar);
    if (skip > captureBeatsPerBar - 0.02) skip = 0;
    const auto skipSamples = (size_t) std::llround (skip * 60.0 / captureBpm * rate);
    if (ringCount <= skipSamples + (size_t) (rate * 4.0))
        return c;
    const size_t n = ringCount - skipSamples;
    const size_t first = (ringWrite + ring.size() - ringCount + skipSamples) % ring.size();
    c.samples.resize (n);
    double sum = 0;
    for (size_t i = 0; i < n; ++i)
    {
        c.samples[i] = ring[(first + i) % ring.size()];
        sum += (double) c.samples[i] * c.samples[i];
    }
    c.sampleRate = rate;
    c.startBeat = std::round (firstPpq + skip);
    c.bpm = captureBpm;
    c.beatsPerBar = captureBeatsPerBar;
    c.level = std::sqrt (sum / (double) n);
    return c;
}
} // namespace m1d1
