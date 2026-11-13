// Tests for the 0.3 engine: detailed chord timing, variations and patterns.
#include "Backline.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>

using namespace backline;

static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { std::printf ("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (false)

static const double pi = 3.14159265358979323846;

static int pcOf (int pitch) { return ((pitch % 12) + 12) % 12; }

static std::string describe (const Progression& p, Key key)
{
    std::string s;
    for (const auto& e : p)
    {
        char buf[64];
        std::snprintf (buf, sizeof (buf), "%s@%.2g(%.2g) ", chordName (e.chord, key).c_str(), e.start, e.length);
        s += buf;
    }
    return s;
}

// A tone with a few harmonics and short fades, added into `out`.
static void addTone (std::vector<float>& out, double rate, double startSec, double lengthSec, int midi, float gain)
{
    const double f = 440.0 * std::pow (2.0, (midi - 69) / 12.0);
    const auto start = (size_t) (startSec * rate), n = (size_t) (lengthSec * rate);
    for (size_t i = 0; i < n && start + i < out.size(); ++i)
    {
        const double t = (double) i / rate;
        const double env = std::min (1.0, std::min (t * 200.0, (lengthSec - t) * 200.0)) * (0.75 + 0.25 * std::exp (-t * 3.0));
        out[start + i] += gain * (float) (env * (std::sin (2 * pi * f * t) + 0.5 * std::sin (4 * pi * f * t)
                                                 + 0.3 * std::sin (6 * pi * f * t) + 0.15 * std::sin (8 * pi * f * t)));
    }
}

// Plays a progression as audio: the chord tones close voiced around middle C and the bass note
// (the slash bass for inversions) in the second octave.
static std::vector<float> playProgression (const Progression& p, double bpm, double rate)
{
    const double spb = 60.0 / bpm;
    std::vector<float> out ((size_t) ((progressionLength (p) * spb + 0.5) * rate));
    for (const auto& e : p)
    {
        const double start = e.start * spb, length = e.length * spb;
        for (int pc : chordPitchClasses (e.chord))
        {
            int pitch = 57;
            while (pcOf (pitch) != pc) ++pitch;
            addTone (out, rate, start, length, pitch, 0.08f);
        }
        addTone (out, rate, start, length, 36 + e.chord.bassPitchClass(), 0.12f);
    }
    return out;
}

static Progression timed (const std::vector<std::pair<Chord, double>>& chords)
{
    Progression p;
    double t = 0.0;
    for (const auto& c : chords)
    {
        ChordEvent e;
        e.chord = c.first;
        e.start = t;
        e.length = c.second;
        p.push_back (e);
        t += c.second;
    }
    return p;
}

// Fraction of beats on which the detected chord (bass included) is the true one.
static double beatAccuracy (const Progression& truth, const Progression& found)
{
    const double length = progressionLength (truth);
    int right = 0, total = 0;
    for (double beat = 0.0; beat < length - 1.0e-9; beat += 1.0)
    {
        const int a = chordIndexAt (truth, beat + 0.5), b = chordIndexAt (found, beat + 0.5);
        ++total;
        if (a >= 0 && b >= 0 && truth[(size_t) a].chord == found[(size_t) b].chord)
            ++right;
    }
    return total > 0 ? (double) right / total : 0.0;
}

static void testDetailedDetection()
{
    const Key cMajor { 0, false, 1.0f }, aMinor { 9, true, 1.0f };
    using Q = Quality;
    // One-beat passing chords, inversions, a sus chord and a dominant 7th.
    const Progression popTruth = timed ({ { { 0, Q::Major }, 3.0 }, { { 7, Q::Major, 11 }, 1.0 }, { { 9, Q::Minor }, 4.0 },
                                          { { 5, Q::Major }, 2.0 }, { { 0, Q::Major, 4 }, 1.0 }, { { 2, Q::Minor }, 1.0 },
                                          { { 7, Q::Sus4 }, 2.0 }, { { 7, Q::Dominant7 }, 2.0 } });
    const Progression minorTruth = timed ({ { { 9, Q::Minor }, 2.0 }, { { 9, Q::Minor, 4 }, 1.0 }, { { 2, Q::Minor }, 1.0 },
                                            { { 5, Q::Major }, 3.0 }, { { 4, Q::Major }, 1.0 }, { { 0, Q::Major }, 2.0 },
                                            { { 7, Q::Major, 2 }, 2.0 }, { { 4, Q::Dominant7 }, 4.0 } });
    for (double bpm : { 75.0, 100.0, 124.0, 150.0 })
        for (int which = 0; which < 2; ++which)
        {
            const Progression& truth = which == 0 ? popTruth : minorTruth;
            const Key key = which == 0 ? cMajor : aMinor;
            const double rate = bpm > 110.0 ? 48000.0 : 44100.0;
            const auto audio = playProgression (truth, bpm, rate);
            const auto beats = analyseBeats (audio.data(), audio.size(), rate, bpm, progressionLength (truth));
            CHECK (std::abs (beats.lengthBeats - progressionLength (truth)) < 1.0e-9);
            CHECK (beats.stepBeats == (bpm < 90.0 ? 0.5 : 1.0));
            const auto found = detectChordTimeline (beats, key, defaultDetail);
            const double accuracy = beatAccuracy (truth, found);
            std::printf ("detail test: %.0f BPM %s: %.0f%% of beats -> %s\n", bpm, which == 0 ? "major" : "minor",
                         accuracy * 100.0, describe (found, key).c_str());
            CHECK (accuracy >= 0.9);
            CHECK (isValidProgression (found));
            CHECK (std::abs (progressionLength (found) - progressionLength (truth)) < 1.0e-9);
            for (size_t i = 0; i < found.size(); ++i)
            {
                CHECK (found[i].confidence >= 0.0f && found[i].confidence <= 1.0f);
                CHECK (found[i].length >= beats.stepBeats - 1.0e-9);
                // Starts lie on the analysis steps; neighbours always differ (repeats are merged).
                const double r = found[i].start / beats.stepBeats;
                CHECK (std::abs (r - std::round (r)) < 1.0e-9);
                if (i > 0) CHECK (found[i].chord != found[i - 1].chord);
            }

            // More detail never gives fewer chords, and the extremes differ.
            size_t previous = 0;
            std::vector<size_t> counts;
            for (double detail = 0.0; detail <= 1.0 + 1.0e-9; detail += 0.1)
            {
                const size_t n = detectChordTimeline (beats, key, detail).size();
                counts.push_back (n);
                CHECK (n >= previous);
                previous = n;
            }
            CHECK (counts.front() < counts.back());

            // Ideas: the first is the detection, the rest differ from it and from each other and
            // keep its timing; another seed gives other alternatives.
            const auto ideas = suggestTimedIdeas (beats, key, defaultDetail, 4, 0);
            CHECK (ideas.size() == 4);
            CHECK (! ideas.empty() && ideas[0] == found);
            for (size_t a = 0; a < ideas.size(); ++a)
            {
                CHECK (ideas[a].size() == found.size());
                for (size_t i = 0; i < ideas[a].size() && i < found.size(); ++i)
                    CHECK (std::abs (ideas[a][i].start - found[i].start) < 1.0e-9 && std::abs (ideas[a][i].length - found[i].length) < 1.0e-9);
                for (size_t b = a + 1; b < ideas.size(); ++b)
                    CHECK (chordsOf (ideas[a]) != chordsOf (ideas[b]));
            }
            const auto again = suggestTimedIdeas (beats, key, defaultDetail, 4, 0);
            CHECK (again == ideas);
            bool differs = false;
            for (std::uint32_t seed = 1; seed <= 4 && ! differs; ++seed)
            {
                const auto other = suggestTimedIdeas (beats, key, defaultDetail, 4, seed);
                CHECK (other.size() == 4 && other[0] == found);
                differs = other != ideas;
            }
            CHECK (differs);
        }

    // With drums and noise on top the chords are still found on most beats.
    {
        const double bpm = 126.0, rate = 44100.0;
        auto audio = playProgression (popTruth, bpm, rate);
        std::uint32_t state = 99u;
        for (size_t i = 0; i < audio.size(); ++i)
        {
            const double t = (double) i / rate, beat = t * bpm / 60.0;
            const double sinceBeat = (beat - std::floor (beat)) * 60.0 / bpm;
            const double sinceHat = (beat * 2.0 - std::floor (beat * 2.0)) * 30.0 / bpm;
            state = state * 1664525u + 1013904223u;
            const double white = (double) (state >> 8) / 8388608.0 - 1.0;
            audio[i] += (float) (0.5 * std::exp (-sinceBeat * 25.0) * std::sin (2.0 * pi * (50.0 + 80.0 * std::exp (-sinceBeat * 30.0)) * sinceBeat)
                                 + 0.08 * std::exp (-sinceHat * 60.0) * white + 0.01 * white);
        }
        const auto beats = analyseBeats (audio.data(), audio.size(), rate, bpm, 16.0);
        const auto found = detectChordTimeline (beats, cMajor, defaultDetail);
        const double accuracy = beatAccuracy (popTruth, found);
        std::printf ("detail test: with drums %.0f%% -> %s\n", accuracy * 100.0, describe (found, cMajor).c_str());
        CHECK (accuracy >= 0.75);
    }

    // Simple detail smooths a one-beat passing chord over; it sits on bar lines.
    {
        const auto audio = playProgression (popTruth, 120.0, 44100.0);
        const auto beats = analyseBeats (audio.data(), audio.size(), 44100.0, 120.0, 16.0);
        const auto simple = detectChordTimeline (beats, cMajor, 0.0);
        std::printf ("detail test: simple -> %s\n", describe (simple, cMajor).c_str());
        CHECK (simple.size() < detectChordTimeline (beats, cMajor, 1.0).size());
    }

    // Silence and nothing at all.
    {
        const std::vector<float> silence (44100 * 4, 0.0f);
        const auto beats = analyseBeats (silence.data(), silence.size(), 44100.0, 120.0, 8.0);
        const auto found = detectChordTimeline (beats, aMinor, 0.5);
        CHECK (found.size() == 1 && isValidProgression (found) && std::abs (progressionLength (found) - 8.0) < 1.0e-9);
        CHECK (! found.empty() && found[0].chord == Chord ({ 9, Quality::Minor }));
        const auto none = analyseBeats (nullptr, 0, 44100.0, 120.0, 4.0);
        CHECK (isValidProgression (detectChordTimeline (none, cMajor, 0.5)));
        CHECK (suggestTimedIdeas (beats, aMinor, 0.5, 3).size() == 3);
        CHECK (suggestTimedIdeas (beats, aMinor, 0.5, 0).empty());
    }
    // Audio shorter than the loop: the last chord runs to the loop end.
    {
        const Progression two = timed ({ { { 0, Q::Major }, 2.0 }, { { 5, Q::Major }, 2.0 } });
        const auto audio = playProgression (two, 120.0, 44100.0);
        const auto beats = analyseBeats (audio.data(), audio.size(), 44100.0, 120.0, 8.0);
        const auto found = detectChordTimeline (beats, cMajor, 0.8);
        CHECK (isValidProgression (found) && std::abs (progressionLength (found) - 8.0) < 1.0e-9);
        CHECK (found.size() == 2 && found[0].chord == two[0].chord && found[1].chord == two[1].chord);
    }
}

// ---- Variations ----

static bool sameTiming (const Progression& a, const Progression& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::abs (a[i].start - b[i].start) > 1.0e-9 || std::abs (a[i].length - b[i].length) > 1.0e-9)
            return false;
    return true;
}

static void testVariations()
{
    using Q = Quality;
    const Key cMajor { 0, false, 1.0f }, eMinor { 4, true, 1.0f };
    const Progression pop = timed ({ { { 0, Q::Major }, 4.0 }, { { 7, Q::Major }, 4.0 }, { { 9, Q::Minor }, 4.0 }, { { 5, Q::Major }, 4.0 } });
    const Progression odd = timed ({ { { 4, Q::Minor }, 3.0 }, { { 0, Q::Major7, 7 }, 1.5 }, { { 11, Q::Dominant7 }, 0.5 },
                                     { { 9, Q::Minor7 }, 2.0 }, { { 7, Q::Sus2 }, 1.0 } });
    for (int which = 0; which < 2; ++which)
    {
        const Progression& base = which == 0 ? pop : odd;
        const Key key = which == 0 ? cMajor : eMinor;
        CHECK (varyProgression (base, key, 0.0, 7) == base);
        for (double amount : { 0.1, 0.3, 0.5, 0.8, 1.0 })
        {
            std::set<std::string> distinct;
            for (std::uint32_t seed = 1; seed <= 12; ++seed)
            {
                const auto v = varyProgression (base, key, amount, seed);
                CHECK (isValidProgression (v));
                CHECK (sameTiming (v, base));
                CHECK (std::abs (progressionLength (v) - progressionLength (base)) < 1.0e-9);
                CHECK (v != base); // something always changes
                for (const auto& e : v)
                    CHECK (isValidChord (e.chord));
                if (amount < 0.4)
                    for (size_t i = 0; i < v.size(); ++i)
                        CHECK (v[i].chord.root == base[i].chord.root);
                CHECK (varyProgression (base, key, amount, seed) == v); // deterministic
                distinct.insert (describe (v, key));
            }
            if (distinct.size() < 6)
                std::printf ("variation test: only %zu different results at amount %.1f\n", distinct.size(), amount);
            CHECK (distinct.size() >= 6);
        }
        // High amounts substitute some roots.
        bool rootChanged = false;
        for (std::uint32_t seed = 1; seed <= 12 && ! rootChanged; ++seed)
        {
            const auto v = varyProgression (base, key, 1.0, seed);
            for (size_t i = 0; i < v.size(); ++i)
                rootChanged = rootChanged || v[i].chord.root != base[i].chord.root;
        }
        CHECK (rootChanged);
    }
    std::printf ("variation test: %s -> %s / %s\n", describe (pop, cMajor).c_str(),
                 describe (varyProgression (pop, cMajor, 0.3, 3), cMajor).c_str(),
                 describe (varyProgression (pop, cMajor, 0.9, 3), cMajor).c_str());
    // One chord, and chords with nothing to vary into, still give something valid.
    const Progression single = timed ({ { { 2, Q::Diminished }, 4.0 } });
    for (std::uint32_t seed = 0; seed < 5; ++seed)
    {
        const auto v = varyProgression (single, cMajor, 0.5, seed);
        CHECK (isValidProgression (v) && sameTiming (v, single) && isValidChord (v[0].chord));
    }
    CHECK (varyProgression (Progression(), cMajor, 0.5, 1).empty());

    CHECK (isValidChord ({ 0, Q::Major, 4 }) && ! isValidChord ({ 0, Q::Major, 5 }) && ! isValidChord ({ 12, Q::Major }));
    CHECK (chordName ({ 0, Q::Add9 }) == "Cadd9" && chordName ({ 9, Q::Minor9 }) == "Am9" && chordName ({ 7, Q::Dominant9 }) == "G9");
    CHECK (chordName ({ 5, Q::Major9 }) == "Fmaj9" && chordName ({ 2, Q::MinorAdd9 }) == "Dmadd9");
    CHECK (chordPitchClasses ({ 0, Q::Major9 }) == std::vector<int> ({ 0, 4, 7, 11, 2 }));
    CHECK (triadChord ({ 0, Q::Major9 }).quality == Q::Major && triadChord ({ 0, Q::Minor9 }).quality == Q::Minor);
    CHECK (hasMinorThird (Q::Minor9) && ! hasMinorThird (Q::Add9) && hasSeventh (Q::Dominant9) && ! hasSeventh (Q::Add9));
}

// ---- Patterns ----

// Every note inside the loop and inside the chord it starts in; every chord heard; sane values.
static bool checkRendered (const std::vector<Note>& notes, const Progression& p, int low, int high, std::string& why)
{
    std::vector<bool> heard (p.size(), false);
    const double loop = progressionLength (p);
    for (const auto& n : notes)
    {
        const int index = chordIndexAt (p, n.start);
        if (index < 0) { why = "note outside the loop"; return false; }
        const auto& e = p[(size_t) index];
        if (! (n.length > 0.0)) { why = "length not positive"; return false; }
        if (n.start + n.length > e.start + e.length + 1.0e-9) { why = "note crosses a chord boundary"; return false; }
        if (n.start + n.length > loop + 1.0e-9) { why = "note crosses the loop end"; return false; }
        if (n.velocity < 1 || n.velocity > 127) { why = "velocity out of range"; return false; }
        if (n.pitch < low || n.pitch > high) { why = "pitch " + std::to_string (n.pitch) + " out of register"; return false; }
        heard[(size_t) index] = true;
    }
    for (size_t i = 0; i < heard.size(); ++i)
        if (! heard[i]) { why = "chord " + std::to_string (i) + " never sounds"; return false; }
    return true;
}

static void testPatterns()
{
    using Q = Quality;
    const Key key { 9, true, 1.0f };
    // Odd lengths, a chord shorter than a step pattern's gaps, chords starting off the grid.
    const Progression odd = timed ({ { { 9, Q::Minor }, 2.5 }, { { 5, Q::Major7, 9 }, 1.25 }, { { 7, Q::Dominant7 }, 0.5 },
                                     { { 0, Q::Add9 }, 3.75 }, { { 4, Q::Major }, 0.25 }, { { 2, Q::Minor9 }, 5.0 },
                                     { { 11, Q::Diminished }, 1.0 }, { { 9, Q::Sus4 }, 1.75 } });
    const Progression plainBars = timed ({ { { 9, Q::Minor }, 4.0 }, { { 5, Q::Major }, 4.0 }, { { 0, Q::Major }, 4.0 }, { { 7, Q::Major }, 4.0 } });
    CHECK (chordPatterns().size() >= 15 && bassPatterns().size() >= 15);
    std::set<std::string> ids;
    for (const auto& d : chordPatterns()) ids.insert ("c" + d.id);
    for (const auto& d : bassPatterns()) ids.insert ("b" + d.id);
    CHECK (ids.size() == chordPatterns().size() + bassPatterns().size()); // unique ids
    CHECK (findChordPattern ("arpup16") >= 0 && findBassPattern ("walking") >= 0 && findBassPattern ("nope") == -1);

    for (const Progression* p : { &odd, &plainBars })
        for (std::uint32_t seed = 1; seed <= 3; ++seed)
        {
            for (size_t i = 0; i < chordPatterns().size(); ++i)
                for (int variant = 0; variant < 4; ++variant)
                {
                    PatternOptions o;
                    o.pattern = (int) i;
                    o.seed = seed;
                    o.voicing = (Voicing) (variant % 3);
                    o.swing = variant == 1 ? 1.0 : 0.0;
                    o.humanise = variant >= 2 ? 1.0 : 0.0;
                    o.density = variant == 3 ? 0.3 : 1.0;
                    o.octaveShift = variant == 2 ? 1 : (variant == 3 ? -1 : 0);
                    o.registerCentre = 48 + 8 * variant;
                    o.dynamics = variant * 0.33;
                    if (variant == 3) o.arpRate = 0.125;
                    const auto notes = renderChordPattern (*p, o);
                    std::string why;
                    const bool ok = checkRendered (notes, *p, 21, 108, why);
                    if (! ok) std::printf ("pattern test: chord pattern %s variant %d: %s\n", chordPatterns()[i].id.c_str(), variant, why.c_str());
                    CHECK (ok);
                    CHECK (renderChordPattern (*p, o).size() == notes.size()); // deterministic
                }
            for (size_t i = 0; i < bassPatterns().size(); ++i)
                for (int variant = 0; variant < 3; ++variant)
                {
                    PatternOptions o;
                    o.pattern = (int) i;
                    o.seed = seed;
                    o.swing = variant == 1 ? 0.8 : 0.0;
                    o.humanise = variant == 2 ? 1.0 : 0.0;
                    o.density = variant == 2 ? 0.4 : 1.0;
                    o.octaveShift = variant - 1;
                    const auto notes = renderBassPattern (*p, o, key);
                    std::string why;
                    const bool ok = checkRendered (notes, *p, 12, 76, why);
                    if (! ok) std::printf ("pattern test: bass pattern %s variant %d: %s\n", bassPatterns()[i].id.c_str(), variant, why.c_str());
                    CHECK (ok);
                    // A bass note struck on a chord start or a bar line is the chord's bass note.
                    for (const auto& n : notes)
                    {
                        const int index = chordIndexAt (*p, n.start);
                        if (index < 0) continue;
                        const auto& e = (*p)[(size_t) index];
                        if (std::abs (n.start - e.start) < 1.0e-9 || std::abs (n.start / 4.0 - std::round (n.start / 4.0)) < 1.0e-9)
                            CHECK (pcOf (n.pitch) == e.chord.bassPitchClass());
                    }
                    // Slides only on glide patterns, and only into a note joined to the one before.
                    for (size_t k = 0; k < notes.size(); ++k)
                        if (notes[k].slide)
                        {
                            CHECK (bassPatterns()[i].slides && k > 0);
                            if (k > 0) CHECK (notes[k - 1].start + notes[k - 1].length >= notes[k].start - 1.0e-6);
                        }
                }
        }

    // The pattern shapes are what they say.
    {
        PatternOptions o;
        o.pattern = findChordPattern ("block");
        const auto block = renderChordPattern (plainBars, o);
        CHECK (block.size() == 12); // four triads, once each
        o.pattern = findChordPattern ("offbeat");
        for (const auto& n : renderChordPattern (plainBars, o))
            CHECK (std::abs (n.start - std::floor (n.start) - 0.5) < 1.0e-9);
        o.pattern = findChordPattern ("arpup16");
        const auto arp = renderChordPattern (plainBars, o);
        CHECK (arp.size() == 64);
        for (size_t i = 0; i < arp.size(); ++i)
            CHECK (std::abs (arp[i].start - 0.25 * (double) i) < 1.0e-9);
        CHECK (arp[1].pitch > arp[0].pitch && arp[2].pitch > arp[1].pitch);
        o.arpRate = 0.5;
        CHECK (renderChordPattern (plainBars, o).size() == 32);
        o = PatternOptions();
        o.pattern = findChordPattern ("strum");
        const auto strum = renderChordPattern (plainBars, o);
        CHECK (strum.size() == 24 && strum[1].start > strum[0].start);
        o = PatternOptions();
        o.pattern = findChordPattern ("pulse4");
        o.swing = 1.0;
        CHECK (renderChordPattern (plainBars, o).size() == 48); // quarters do not swing
        o.pattern = findChordPattern ("eighths");
        const auto swung = renderChordPattern (plainBars, o);
        bool late = false;
        for (const auto& n : swung) late = late || std::abs (n.start - std::round (n.start * 2.0) / 2.0) > 1.0e-6;
        CHECK (late);
        // Octave shift moves everything by an octave; spread voicing is wider than close.
        o = PatternOptions();
        o.pattern = findChordPattern ("block");
        auto shifted = o;
        shifted.octaveShift = 1;
        const auto a = renderChordPattern (plainBars, o), b = renderChordPattern (plainBars, shifted);
        CHECK (a.size() == b.size());
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) CHECK (b[i].pitch == a[i].pitch + 12);
        auto spread = o;
        spread.voicing = Voicing::Spread;
        const auto s = renderChordPattern (plainBars, spread);
        auto width = [] (const std::vector<Note>& notes, double at)
        {
            int lo = 200, hi = -1;
            for (const auto& n : notes) if (std::abs (n.start - at) < 1e-9) { lo = std::min (lo, n.pitch); hi = std::max (hi, n.pitch); }
            return hi - lo;
        };
        CHECK (width (s, 0.0) > width (a, 0.0) && width (s, 0.0) >= 12);
        // Density thins the pattern; dynamics separate accents.
        o.pattern = findChordPattern ("gate16");
        auto thin = o;
        thin.density = 0.2;
        CHECK (renderChordPattern (plainBars, thin).size() < renderChordPattern (plainBars, o).size());
        auto flat = o, loud = o;
        flat.dynamics = 0.0;
        loud.dynamics = 1.0;
        auto spreadOf = [] (const std::vector<Note>& notes)
        {
            int lo = 128, hi = 0;
            for (const auto& n : notes) { lo = std::min (lo, n.velocity); hi = std::max (hi, n.velocity); }
            return hi - lo;
        };
        CHECK (spreadOf (renderChordPattern (plainBars, loud)) > spreadOf (renderChordPattern (plainBars, flat)));
        // Walking bass approaches the next chord; the sub pattern slides.
        PatternOptions w;
        w.pattern = findBassPattern ("walking");
        const auto walk = renderBassPattern (plainBars, w, key);
        CHECK (walk.size() == 16);
        if (walk.size() == 16)
            for (size_t c = 0; c < 4; ++c)
            {
                const int nextBass = plainBars[(c + 1) % 4].chord.bassPitchClass();
                const int d = std::abs (pcOf (walk[c * 4 + 3].pitch) - nextBass);
                CHECK (d == 1 || d == 11); // a semitone from the next chord's bass note
            }
        w.pattern = findBassPattern ("glide");
        bool slid = false;
        for (const auto& n : renderBassPattern (plainBars, w, key)) slid = slid || n.slide;
        CHECK (slid);
    }
    // Randomise: valid, deterministic, varied.
    std::set<int> picked;
    for (std::uint32_t seed = 1; seed <= 20; ++seed)
    {
        const auto c = randomChordOptions (seed), b = randomBassOptions (seed);
        CHECK (c.pattern >= 0 && c.pattern < (int) chordPatterns().size());
        CHECK (b.pattern >= 0 && b.pattern < (int) bassPatterns().size());
        CHECK (randomChordOptions (seed).pattern == c.pattern && randomBassOptions (seed).pattern == b.pattern);
        picked.insert (c.pattern);
        std::string why;
        CHECK (checkRendered (renderChordPattern (odd, c), odd, 21, 108, why));
        CHECK (checkRendered (renderBassPattern (odd, b, key), odd, 12, 76, why));
    }
    CHECK (picked.size() >= 8);
}

static void testRegenerate()
{
    // Fixed-grid ideas with a seed: seed 0 is the old behaviour, other seeds other sets.
    const Key key { 0, false, 1.0f };
    std::vector<Chroma> segments (4, Chroma {});
    const int roots[] = { 0, 7, 9, 5 };
    for (size_t s = 0; s < 4; ++s)
        for (int pc : chordPitchClasses ({ roots[s], s == 2 ? Quality::Minor : Quality::Major }))
            segments[s][(size_t) pc] = 1.0f;
    CHECK (suggestProgressions (segments, key, 4, 0) == suggestProgressions (segments, key, 4));
    std::set<std::string> sets;
    for (std::uint32_t seed = 0; seed < 8; ++seed)
    {
        const auto ideas = suggestProgressions (segments, key, 4, seed);
        CHECK (ideas.size() == 4);
        CHECK (suggestProgressions (segments, key, 4, seed) == ideas);
        std::string all;
        for (const auto& idea : ideas) for (auto c : idea) all += chordName (c) + " ";
        sets.insert (all);
    }
    CHECK (sets.size() >= 4);
}

int main()
{
    testDetailedDetection();
    testRegenerate();
    testVariations();
    testPatterns();
    std::printf (failures == 0 ? "All engine tests passed\n" : "%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
