// Variations of a progression: new colours for the same chords, and at higher amounts
// substitutions. Timing is never touched.
#include "Backline.h"
#include "Random.h"
#include <algorithm>
#include <cmath>

namespace backline
{
namespace
{
int wrap (int pc) { return ((pc % 12) + 12) % 12; }

bool fitsKey (int pitchClass, Key key)
{
    return isInScale (pitchClass, key) || (key.minor && wrap (pitchClass - key.tonic) == 11);
}

bool allInKey (Chord chord, Key key)
{
    for (int pc : chordPitchClasses (chord))
        if (! fitsKey (pc, key))
            return false;
    return true;
}

// Keeps a slash bass only while it is still one of the chord's notes.
Chord keepBassIfValid (Chord chord)
{
    if (chord.bass >= 0)
    {
        const auto pcs = chordPitchClasses (chord);
        if (std::find (pcs.begin(), pcs.end(), wrap (chord.bass)) == pcs.end() || wrap (chord.bass) == wrap (chord.root))
            chord.bass = -1;
    }
    return chord;
}

bool isMajorFamily (Quality q)
{
    return q == Quality::Major || q == Quality::Major7 || q == Quality::Dominant7 || q == Quality::Add9
        || q == Quality::Major9 || q == Quality::Dominant9;
}

bool isMinorFamily (Quality q)
{
    return q == Quality::Minor || q == Quality::Minor7 || q == Quality::MinorAdd9 || q == Quality::Minor9;
}

// ---- Colour operations: the root stays ----

Chord invert (Chord chord, Random& random)
{
    const auto pcs = chordPitchClasses (chord);
    if (pcs.size() < 3 || chord.quality == Quality::Sus2 || chord.quality == Quality::Sus4)
        return chord;
    if (chord.bass >= 0 && random.chance (0.4))
    {
        chord.bass = -1; // back to root position
        return chord;
    }
    chord.bass = random.chance (0.65) ? pcs[1] : pcs[2];
    return chord;
}

Chord addSeventh (Chord chord, Key key)
{
    Chord result = seventhChord (triadChord (chord), key);
    if (result.quality == triadChord (chord).quality && isMajorFamily (chord.quality) && wrap (chord.root - key.tonic) == 7)
        result.quality = Quality::Dominant7; // the dominant of a minor key
    result.bass = chord.bass;
    return keepBassIfValid (result);
}

Chord addNinth (Chord chord, Key key)
{
    const int ninth = wrap (chord.root + 2);
    if (! fitsKey (ninth, key))
        return chord;
    Chord result = chord;
    switch (chord.quality)
    {
        case Quality::Major:     result.quality = fitsKey (chord.root + 11, key) && wrap (chord.root - key.tonic) != 7 ? Quality::Major9 : Quality::Add9; break;
        case Quality::Major7:    result.quality = Quality::Major9; break;
        case Quality::Dominant7: result.quality = Quality::Dominant9; break;
        case Quality::Minor:     result.quality = fitsKey (chord.root + 10, key) ? Quality::Minor9 : Quality::MinorAdd9; break;
        case Quality::Minor7:    result.quality = Quality::Minor9; break;
        case Quality::Diminished:
        case Quality::Sus2:
        case Quality::Sus4:
        case Quality::Add9:
        case Quality::MinorAdd9:
        case Quality::Major9:
        case Quality::Minor9:
        case Quality::Dominant9: return chord;
    }
    return keepBassIfValid (result);
}

Chord suspend (Chord chord, Key key, Random& random)
{
    if (! isMajorFamily (chord.quality) && ! isMinorFamily (chord.quality))
        return chord;
    Chord four = chord, two = chord;
    four.quality = Quality::Sus4;
    two.quality = Quality::Sus2;
    four = keepBassIfValid (four);
    two = keepBassIfValid (two);
    const bool fourFits = allInKey (four, key), twoFits = allInKey (two, key);
    if (fourFits && (! twoFits || random.chance (0.6))) return four;
    if (twoFits) return two;
    return chord;
}

// Back to a plain triad: extensions taken off, a suspension resolved to the third the key has.
Chord plain (Chord chord, Key key)
{
    if (chord.quality == Quality::Sus2 || chord.quality == Quality::Sus4)
    {
        Chord result { chord.root, Quality::Major, -1, chord.sharpRoot };
        if (fitsKey (chord.root + 4, key)) return result;
        result.quality = Quality::Minor;
        return fitsKey (chord.root + 3, key) ? result : chord;
    }
    return keepBassIfValid (triadChord (chord));
}

// ---- Substitutions: the root may change ----

Chord relative (Chord chord, Key key)
{
    Chord result;
    if (isMajorFamily (chord.quality) && chord.quality != Quality::Dominant7 && chord.quality != Quality::Dominant9)
        result = { wrap (chord.root + 9), Quality::Minor };
    else if (isMinorFamily (chord.quality))
        result = { wrap (chord.root + 3), Quality::Major };
    else
        return chord;
    if (hasSeventh (chord.quality))
        result = seventhChord (result, key);
    return allInKey (result, key) ? result : chord;
}

Chord tritone (Chord chord, Key key)
{
    const bool dominant = chord.quality == Quality::Dominant7 || chord.quality == Quality::Dominant9
                       || (isMajorFamily (chord.quality) && wrap (chord.root - key.tonic) == 7);
    if (! dominant)
        return chord;
    return { wrap (chord.root + 6), Quality::Dominant7, -1, false };
}

Chord secondaryDominant (Chord chord, Chord next, Key key)
{
    const int target = wrap (next.root);
    if (target == wrap (key.tonic) && wrap (chord.root - key.tonic) == 7)
        return chord; // already the dominant
    if (next.quality == Quality::Diminished)
        return chord;
    Chord dominant { wrap (target + 7), Quality::Dominant7 };
    return dominant == chord ? chord : dominant;
}

Chord borrowed (Chord chord, Key key)
{
    const int degree = wrap (chord.root - key.tonic);
    Chord result = chord;
    result.bass = -1;
    if (! key.minor)
    {
        if (degree == 5 && isMajorFamily (chord.quality))       result.quality = Quality::Minor;           // iv
        else if (degree == 9 && isMinorFamily (chord.quality))  result = { wrap (key.tonic + 8), Quality::Major };  // bVI
        else if (degree == 7 && isMajorFamily (chord.quality))  result = { wrap (key.tonic + 10), Quality::Major }; // bVII
        else if (degree == 4 && isMinorFamily (chord.quality))  result = { wrap (key.tonic + 3), Quality::Major };  // bIII
        else if (degree == 2 && isMinorFamily (chord.quality))  result = { wrap (key.tonic + 5), Quality::Minor };  // iv for ii
        else if (degree == 0 && isMajorFamily (chord.quality))  result.quality = Quality::Minor;           // i
        else return chord;
    }
    else
    {
        if (degree == 7 && isMinorFamily (chord.quality))       result.quality = Quality::Dominant7;       // V7
        else if (degree == 5 && isMinorFamily (chord.quality))  result.quality = Quality::Major;           // IV (dorian)
        else if (degree == 3 && isMajorFamily (chord.quality))  result = { wrap (key.tonic + 5), Quality::Minor };  // iv for III
        else if (degree == 10 && isMajorFamily (chord.quality)) result = { wrap (key.tonic + 7), Quality::Major };  // V for VII
        else if (degree == 8 && isMajorFamily (chord.quality))  result = { wrap (key.tonic + 1), Quality::Major };  // bII (neapolitan)
        else return chord;
    }
    return result;
}

enum class Op { Invert, Seventh, Ninth, Sus, Plain, Relative, Tritone, Secondary, Borrowed };

Chord apply (Op op, const Progression& progression, size_t index, Key key, Random& random)
{
    const Chord chord = progression[index].chord;
    const Chord next = progression[(index + 1) % progression.size()].chord;
    switch (op)
    {
        case Op::Invert:    return invert (chord, random);
        case Op::Seventh:   return addSeventh (chord, key);
        case Op::Ninth:     return addNinth (chord, key);
        case Op::Sus:       return suspend (chord, key, random);
        case Op::Plain:     return plain (chord, key);
        case Op::Relative:  return relative (chord, key);
        case Op::Tritone:   return tritone (chord, key);
        case Op::Secondary: return progression.size() > 1 ? secondaryDominant (chord, next, key) : chord;
        case Op::Borrowed:  return borrowed (chord, key);
    }
    return chord;
}
}

bool isValidChord (Chord chord)
{
    if (chord.root < 0 || chord.root > 11 || (int) chord.quality < 0 || (int) chord.quality >= numQualities)
        return false;
    if (chord.bass < 0)
        return true;
    const auto pcs = chordPitchClasses (chord);
    return chord.bass <= 11 && std::find (pcs.begin(), pcs.end(), chord.bass) != pcs.end();
}

Progression varyProgression (const Progression& progression, Key key, double amount, std::uint32_t seed)
{
    const double a = std::min (std::max (amount, 0.0), 1.0);
    if (a <= 0.0 || progression.empty())
        return progression;

    Random random (mixSeed (seed, 0x5EED0001u));
    const bool substitute = a >= 0.4;
    const double changeChance = 0.25 + 0.65 * a;

    auto weightsFor = [&] (const Chord& chord)
    {
        const bool sus = chord.quality == Quality::Sus2 || chord.quality == Quality::Sus4;
        const bool extended = chord.quality != triadChord (chord).quality || sus;
        std::vector<double> w (9, 0.0);
        w[(size_t) Op::Invert] = 1.0;
        w[(size_t) Op::Seventh] = extended ? 0.2 : 1.2;
        w[(size_t) Op::Ninth] = 0.4 + 0.8 * a;
        w[(size_t) Op::Sus] = 0.6;
        w[(size_t) Op::Plain] = extended ? 0.6 : 0.0;
        if (substitute)
        {
            const double s = (a - 0.4) / 0.6;
            w[(size_t) Op::Relative] = 0.4 + 1.2 * s;
            w[(size_t) Op::Tritone] = 0.2 + 0.8 * s;
            w[(size_t) Op::Secondary] = 0.3 + 1.0 * s;
            w[(size_t) Op::Borrowed] = 0.3 + 1.0 * s;
        }
        return w;
    };

    auto changeOne = [&] (Progression& p, size_t i)
    {
        auto weights = weightsFor (p[i].chord);
        for (int attempt = 0; attempt < 9; ++attempt)
        {
            bool any = false;
            for (double w : weights) any = any || w > 0.0;
            if (! any)
                break;
            const size_t op = random.weighted (weights);
            Chord changed = apply ((Op) op, p, i, key, random);
            changed.sharpRoot = changed.root != p[i].chord.root ? false : p[i].chord.sharpRoot;
            if (changed != p[i].chord && isValidChord (changed))
            {
                p[i].chord = changed;
                p[i].coloured = false;
                return true;
            }
            weights[op] = 0.0; // no use here: try another
        }
        return false;
    };

    Progression result = progression;
    bool changedAny = false;
    for (size_t i = 0; i < result.size(); ++i)
        if (random.chance (changeChance))
            changedAny = changeOne (result, i) || changedAny;

    // Any amount above nothing changes something.
    for (size_t k = 0; ! changedAny && k < result.size(); ++k)
        changedAny = changeOne (result, (size_t) ((seed + k) % result.size()));
    return result;
}
}
