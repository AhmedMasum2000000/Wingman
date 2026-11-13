// Data-driven rhythm patterns for the chord and bass parts, and their rendering to notes.
// Every pattern here is original, written from common rhythmic idioms.
#include "Backline.h"
#include "Random.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace backline
{
namespace
{
const double tiny = 1.0e-9;

int wrap (int pc) { return ((pc % 12) + 12) % 12; }

std::vector<ChordPatternDef> makeChordPatterns()
{
    using S = ChordStyle;
    using A = ArpOrder;
    std::vector<ChordPatternDef> p;
    auto add = [&p] (const char* id, const char* name, S style, const char* steps, double stepBeats, double gate,
                     A order = A::Up, double strum = 0.0)
    {
        ChordPatternDef d;
        d.id = id; d.name = name; d.style = style; d.steps = steps; d.stepBeats = stepBeats; d.gate = gate;
        d.order = order; d.strumBeats = strum;
        p.push_back (d);
    };
    add ("block",       "Block",              S::Block,     "",                 1.0,  1.0);
    add ("pulse4",      "Quarter pulse",      S::Rhythmic,  "X---x---x---x---", 0.25, 0.85);
    add ("offbeat",     "Offbeat stabs",      S::Rhythmic,  "..X...x...X...x.", 0.25, 0.7);
    add ("eighths",     "Eighth pulse",       S::Rhythmic,  "X-x-x-x-X-x-x-x-", 0.25, 0.75);
    add ("charleston",  "Charleston",         S::Rhythmic,  "X-----x---------", 0.25, 0.9);
    add ("dotted332",   "Dotted 3-3-2",       S::Rhythmic,  "X-----x-----x---", 0.25, 0.9);
    add ("syncstabs",   "Syncopated stabs",   S::Rhythmic,  "X..x..x...x..x..", 0.25, 0.5);
    add ("twostep",     "Two-step",           S::Rhythmic,  "x..X..x...x.X...", 0.25, 0.55);
    add ("halftime",    "Half-time hits",     S::Rhythmic,  "X-------x-----x-", 0.25, 0.85);
    add ("futurestabs", "Rising stabs",       S::Rhythmic,  "X--x--X-x--x--x-", 0.25, 0.8);
    add ("gate16",      "Gated sixteenths",   S::Rhythmic,  "X.xxX.xxX.xxX.x.", 0.25, 0.6);
    add ("laidback",    "Laid back",          S::Rhythmic,  "X------x--------", 0.25, 0.95);
    add ("pianogroove", "Piano groove",       S::Rhythmic,  "X..x..x.x..x..x.", 0.25, 0.6);
    add ("strum",       "Strum",              S::Strum,     "X-------x-------", 0.25, 0.95, A::Up, 0.04);
    add ("strumgroove", "Strum groove",       S::Strum,     "X-----x---x-x---", 0.25, 0.9,  A::Up, 0.035);
    add ("arpup8",      "Arp up 1/8",         S::Arpeggio,  "Xxxx",             0.5,  0.85, A::Up);
    add ("arpup16",     "Arp up 1/16",        S::Arpeggio,  "Xxxx",             0.25, 0.8,  A::Up);
    add ("arpupdown16", "Arp up-down 1/16",   S::Arpeggio,  "Xxxx",             0.25, 0.8,  A::UpDown);
    add ("arpdown8",    "Arp down 1/8",       S::Arpeggio,  "Xxxx",             0.5,  0.85, A::Down);
    add ("arprandom16", "Arp random 1/16",    S::Arpeggio,  "Xxxx",             0.25, 0.7,  A::Random);
    add ("arptriplet",  "Arp triplets",       S::Arpeggio,  "Xxx",              1.0 / 3.0, 0.85, A::Up);
    return p;
}

std::vector<BassPatternDef> makeBassPatterns()
{
    std::vector<BassPatternDef> p;
    auto add = [&p] (const char* id, const char* name, const char* steps, double stepBeats, double gate,
                     bool slides = false, int octave = 0)
    {
        BassPatternDef d;
        d.id = id; d.name = name; d.steps = steps; d.stepBeats = stepBeats; d.gate = gate; d.slides = slides; d.octave = octave;
        p.push_back (d);
    };
    add ("hold",       "Root hold",          "R---------------", 0.25, 0.95);
    add ("quarters",   "Root quarters",      "R---r---R---r---", 0.25, 0.9);
    add ("eighths",    "Root eighths",       "R-r-r-r-R-r-r-r-", 0.25, 0.8);
    add ("octaves",    "Octaves",            "R-o-r-o-R-o-r-o-", 0.25, 0.75);
    add ("offbeat",    "Offbeat",            "..R...r...R...r.", 0.25, 0.7);
    add ("rootfifth",  "Root-fifth",         "R---f---R---f---", 0.25, 0.9);
    add ("walking",    "Walking",            "R---w---w---a---", 0.25, 0.9);
    add ("syncopated", "Syncopated",         "R-----r---R---r-", 0.25, 0.85);
    add ("rolling",    "Rolling 1/16",       ".rrr.rrr.rrr.rrr", 0.25, 0.75);
    add ("gallop",     "Gallop",             "R.rrr.rrR.rrr.rr", 0.25, 0.7);
    add ("sub",        "Sub with slides",    "R-------------r-", 0.25, 1.0, true, -1);
    add ("glide",      "Octave glide",       "R-----------o---", 0.25, 1.0, true, -1);
    add ("twostep",    "Two-step",           "R-----r--r----r-", 0.25, 0.8);
    add ("logdrum",    "Log drum",           "R-.r-.o-r-.r-.f-", 0.25, 1.0, true, 0);
    add ("pulse16",    "Pulse 1/16",         "RrrrRrrrRrrrRrrr", 0.25, 0.6);
    add ("reese",      "Long and moving",    "R---------r--f--", 0.25, 0.95);
    add ("wobble",     "Half-time wobble",   "R-rr-r-rR-r-rrr-", 0.25, 0.7);
    add ("boombap",    "Boom bap",           "R------r--r-----", 0.25, 0.9);
    add ("afro",       "Afro syncopation",   "R..r..r...r.r...", 0.25, 0.7);
    add ("approach",   "Root and approach",  "R-----------r-a-", 0.25, 0.9);
    return p;
}

// One note of a pattern cycle: where it starts (in steps), how many steps it holds, its symbol.
struct CycleHit
{
    size_t step;
    size_t steps;
    char symbol;
};

bool isHit (char c) { return c != '-' && c != '.' && c != ' '; }

std::vector<CycleHit> parseCycle (const std::string& steps)
{
    std::vector<CycleHit> hits;
    const size_t n = steps.size();
    for (size_t i = 0; i < n; ++i)
    {
        if (! isHit (steps[i]))
            continue;
        size_t length = 1;
        // Holds run on past the end into the start of the next cycle.
        while (length < n && steps[(i + length) % n] == '-')
            ++length;
        hits.push_back ({ i, length, steps[i] });
    }
    return hits;
}

// A hit placed in a chord: times in beats from the loop start.
struct Hit
{
    double start = 0.0, length = 0.0;
    double grid = 0.0;         // where the pattern puts it, before swing and humanising
    char symbol = 'x';
    long long stepIndex = 0;   // absolute step number from the loop start
    bool atChordStart = false;
    bool restruck = false;
};

// The hits of a step pattern that fall in one chord (see the header for the rules).
std::vector<Hit> hitsInChord (const ChordEvent& event, const std::string& steps, double stepBeats, double gate)
{
    std::vector<Hit> out;
    const auto cycle = parseCycle (steps);
    const double from = event.start, to = event.start + event.length;
    if (cycle.empty() || stepBeats <= tiny)
    {
        Hit h;
        h.start = from; h.grid = from; h.length = std::min (event.length, 1.0) * std::min (gate, 1.0); h.atChordStart = true; h.symbol = 'R';
        if (h.length <= tiny) h.length = event.length;
        out.push_back (h);
        return out;
    }
    const double cycleBeats = (double) steps.size() * stepBeats;
    const long long firstCycle = (long long) std::floor (from / cycleBeats) - 1;
    const long long lastCycle = (long long) std::floor (to / cycleBeats) + 1;
    Hit sounding; // the latest hit that started before the chord and still sounds at its start
    bool haveSounding = false;
    for (long long k = firstCycle; k <= lastCycle; ++k)
        for (const auto& c : cycle)
        {
            const double start = (double) k * cycleBeats + (double) c.step * stepBeats;
            const double length = (double) c.steps * stepBeats;
            const long long index = k * (long long) steps.size() + (long long) c.step;
            if (start < from - tiny)
            {
                if (start + length > from + tiny && (! haveSounding || start > sounding.start))
                {
                    sounding.start = start; sounding.length = length; sounding.symbol = c.symbol; sounding.stepIndex = index;
                    haveSounding = true;
                }
                continue;
            }
            if (start >= to - tiny)
                continue;
            Hit h;
            h.start = start;
            h.length = std::min (length * gate, to - start);
            h.symbol = c.symbol;
            h.stepIndex = index;
            h.atChordStart = std::abs (start - from) < tiny;
            out.push_back (h);
        }
    std::sort (out.begin(), out.end(), [] (const Hit& a, const Hit& b) { return a.start < b.start; });
    for (auto& h : out)
        h.grid = h.start;

    const bool struckAtStart = ! out.empty() && out.front().atChordStart;
    if (! struckAtStart && haveSounding)
    {
        // A note held over the chord change is struck again for the new chord.
        Hit h = sounding;
        h.start = from;
        h.length = std::min ((sounding.start + sounding.length - from) * gate, to - from);
        if (h.length <= tiny) h.length = std::min (to - from, stepBeats);
        h.atChordStart = true;
        h.restruck = true;
        h.grid = from;
        out.insert (out.begin(), h);
    }
    else if (out.empty())
    {
        // Nothing of the pattern falls in this chord: it is struck on its start.
        Hit h;
        h.start = from;
        h.length = std::min (to - from, std::max (stepBeats, 1.0) * gate);
        if (h.length <= tiny) h.length = to - from;
        h.atChordStart = true;
        h.grid = from;
        h.symbol = (char) std::toupper ((unsigned char) cycle.front().symbol);
        out.push_back (h);
    }
    return out;
}

bool onBarLine (double beat, double beatsPerBar)
{
    const double r = beat / beatsPerBar;
    return std::abs (r - std::round (r)) < 1.0e-6;
}

bool onBeat (double beat)
{
    return std::abs (beat - std::round (beat)) < 1.0e-6;
}

int clampVelocity (double v)
{
    return std::min (127, std::max (1, (int) std::lround (v)));
}

int velocityFor (bool accent, double beat, const PatternOptions& o)
{
    const double d = std::min (std::max (o.dynamics, 0.0), 1.0);
    double v = accent ? 98.0 + 26.0 * d : 94.0 - 24.0 * d;
    if (! accent && onBeat (beat)) v += 6.0 * d;
    return clampVelocity (v);
}

// Leaves out weak hits (never the first of a chord, an accent or a hit on the beat), then swings,
// humanises and keeps every hit inside its chord.
// Swing works on the finest subdivision a pattern uses: its own steps if it has hits on odd
// steps, otherwise twice that (eighth-note swing for a pattern of eighths on a sixteenth grid).
double swingUnit (const std::string& steps, double stepBeats)
{
    for (size_t i = 1; i < steps.size(); i += 2)
        if (isHit (steps[i]))
            return stepBeats;
    return steps.size() % 2 == 0 ? 2.0 * stepBeats : stepBeats;
}

std::vector<Hit> shapeHits (std::vector<Hit> hits, const ChordEvent& event, double unit, const PatternOptions& o)
{
    const double from = event.start, to = event.start + event.length;
    const double density = std::min (std::max (o.density, 0.0), 1.0);
    std::vector<Hit> kept;
    for (size_t i = 0; i < hits.size(); ++i)
    {
        const Hit& h = hits[i];
        const bool essential = i == 0 || std::isupper ((unsigned char) h.symbol) || onBeat (h.start);
        if (! essential && density < 1.0)
        {
            Random r (mixSeed (o.seed, (std::uint64_t) (h.stepIndex + 7919)));
            if (r.uniform() >= density)
                continue;
        }
        kept.push_back (h);
    }

    const double swing = std::min (std::max (o.swing, 0.0), 1.0);
    const double human = std::min (std::max (o.humanise, 0.0), 1.0);
    for (auto& h : kept)
    {
        double shift = 0.0;
        const long long slot = (long long) std::llround (h.grid / unit);
        if (swing > 0.0 && ! h.restruck && std::abs (h.grid - (double) slot * unit) < 1.0e-6 && (slot % 2 + 2) % 2 == 1)
            shift += swing * unit / 3.0;
        if (human > 0.0)
        {
            Random r (mixSeed (o.seed ^ 0x48554D41u, (std::uint64_t) (h.stepIndex * 131 + (long long) (h.start * 1000.0))));
            shift += human * 0.03 * r.uniform();
        }
        if (shift > 0.0)
        {
            const double end = h.start + h.length;
            const double latest = std::min (end, to) - std::max (0.02, 0.25 * h.length);
            const double newStart = std::min (h.start + shift, latest);
            if (newStart > h.start + tiny)
            {
                h.length = std::min (end, to) - newStart;
                h.start = newStart;
            }
        }
        h.start = std::max (h.start, from);
        h.length = std::min (h.length, to - h.start);
    }
    return kept;
}

// Close voicing around `centre`, with a slash chord's bass note underneath (as renderChords).
std::vector<int> closeVoicing (Chord chord, double centre)
{
    const int lowest = (int) std::floor (centre) - 6;
    const int bass = chord.bassPitchClass();
    const bool slash = bass != wrap (chord.root);
    std::vector<int> pitches;
    for (int pc : chordPitchClasses (chord))
    {
        if (slash && pc == bass)
            continue;
        int pitch = lowest;
        while (wrap (pitch) != pc) ++pitch;
        if (std::find (pitches.begin(), pitches.end(), pitch) == pitches.end())
            pitches.push_back (pitch);
    }
    std::sort (pitches.begin(), pitches.end());
    if (slash)
    {
        int pitch = pitches.front() - 1;
        while (wrap (pitch) != bass) --pitch;
        pitches.insert (pitches.begin(), pitch);
    }
    return pitches;
}

std::vector<int> voiceChord (Chord chord, double centre, Voicing voicing)
{
    auto pitches = closeVoicing (chord, centre);
    const bool slash = chord.bassPitchClass() != wrap (chord.root);
    if (voicing == Voicing::Open && pitches.size() >= (slash ? 4u : 3u))
    {
        // Drop 2: the second note from the top goes down an octave.
        pitches[pitches.size() - 2] -= 12;
        std::sort (pitches.begin(), pitches.end());
    }
    else if (voicing == Voicing::Spread && pitches.size() >= 3)
    {
        // The bass note (root, or the slash bass) an octave under the rest, the rest opened up.
        const int bassPc = chord.bassPitchClass();
        std::vector<int> upper;
        int bass = 0;
        bool haveBass = false;
        for (int p : pitches)
        {
            if (! haveBass && wrap (p) == bassPc) { bass = p; haveBass = true; }
            else upper.push_back (p);
        }
        if (! haveBass) { bass = pitches.front(); upper.erase (upper.begin()); }
        while (! upper.empty() && bass > upper.front() - 7) bass -= 12;
        if (upper.size() >= 3)
            upper[1] += 12;
        std::sort (upper.begin(), upper.end());
        pitches.assign (1, bass);
        pitches.insert (pitches.end(), upper.begin(), upper.end());
    }
    return pitches;
}

int fold (int pitch, int low, int high)
{
    while (pitch < low) pitch += 12;
    while (pitch > high) pitch -= 12;
    return pitch;
}

template <typename Def>
const Def& pick (const std::vector<Def>& list, int index)
{
    return list[(size_t) std::min (std::max (index, 0), (int) list.size() - 1)];
}
}

const std::vector<ChordPatternDef>& chordPatterns()
{
    static const std::vector<ChordPatternDef> patterns = makeChordPatterns();
    return patterns;
}

const std::vector<BassPatternDef>& bassPatterns()
{
    static const std::vector<BassPatternDef> patterns = makeBassPatterns();
    return patterns;
}

int findChordPattern (const std::string& id)
{
    const auto& p = chordPatterns();
    for (size_t i = 0; i < p.size(); ++i)
        if (p[i].id == id) return (int) i;
    return -1;
}

int findBassPattern (const std::string& id)
{
    const auto& p = bassPatterns();
    for (size_t i = 0; i < p.size(); ++i)
        if (p[i].id == id) return (int) i;
    return -1;
}

std::vector<Note> renderChordPattern (const Progression& progression, const PatternOptions& options)
{
    std::vector<Note> notes;
    const ChordPatternDef& def = pick (chordPatterns(), options.pattern);
    const int centreTarget = std::min (std::max (options.registerCentre, 48), 76);
    const int shift = 12 * std::min (std::max (options.octaveShift, -2), 2);
    double centre = (double) centreTarget;
    const double stepBeats = def.style == ChordStyle::Arpeggio && options.arpRate > tiny ? options.arpRate : def.stepBeats;

    for (size_t ci = 0; ci < progression.size(); ++ci)
    {
        const auto& event = progression[ci];
        if (! (event.length > tiny))
            continue;
        auto pitches = voiceChord (event.chord, centre, options.voicing);
        double sum = 0.0;
        for (int p : pitches) sum += (double) p;
        centre = std::min (std::max (sum / (double) pitches.size(), (double) centreTarget - 5.0), (double) centreTarget + 5.0);
        // Nothing sinks into the bass part's register, however far the voicing drifted.
        for (auto& p : pitches)
            while (p < centreTarget - 15) p += 12;
        std::sort (pitches.begin(), pitches.end());
        pitches.erase (std::unique (pitches.begin(), pitches.end()), pitches.end());
        for (auto& p : pitches)
            p = fold (p + shift, 21, 108);

        std::vector<Hit> hits;
        if (def.style == ChordStyle::Block)
        {
            Hit h;
            h.start = event.start; h.grid = event.start; h.length = event.length * std::min (def.gate, 1.0); h.symbol = 'X';
            h.atChordStart = true; h.stepIndex = 0;
            hits.push_back (h);
        }
        else
            hits = shapeHits (hitsInChord (event, def.steps, stepBeats, def.gate), event, swingUnit (def.steps, stepBeats), options);

        // Arpeggio notes: the voicing (with the top an octave up for a triad) in the pattern's order.
        std::vector<int> arp = pitches;
        if (arp.size() <= 3)
            arp.push_back (std::min (arp.back() + 12, 108));
        if (def.order == ArpOrder::Down)
            std::reverse (arp.begin(), arp.end());
        else if (def.order == ArpOrder::UpDown && arp.size() > 2)
            for (size_t i = arp.size() - 1; i-- > 1;)
                arp.push_back (arp[i]);

        for (size_t h = 0; h < hits.size(); ++h)
        {
            const Hit& hit = hits[h];
            if (! (hit.length > tiny))
                continue;
            const bool accent = h == 0 || std::isupper ((unsigned char) hit.symbol);
            int velocity = velocityFor (accent, hit.grid, options);
            if (options.humanise > 0.0)
            {
                Random r (mixSeed (options.seed ^ 0x56454Cu, (std::uint64_t) (ci * 977 + h)));
                velocity = clampVelocity (velocity + options.humanise * 14.0 * (r.uniform() - 0.5));
            }
            if (def.style == ChordStyle::Arpeggio)
            {
                size_t index = h % arp.size();
                if (def.order == ArpOrder::Random)
                {
                    Random r (mixSeed (options.seed, (std::uint64_t) (ci * 1013 + h)));
                    index = (size_t) r.range (0, (int) arp.size());
                }
                notes.push_back ({ arp[index], hit.start, hit.length, velocity, false });
                continue;
            }
            const bool strum = def.style == ChordStyle::Strum && def.strumBeats > 0.0;
            // Down strokes on the beat, up strokes between.
            const bool up = strum && ! onBeat (hit.grid);
            for (size_t k = 0; k < pitches.size(); ++k)
            {
                const size_t order = up ? pitches.size() - 1 - k : k;
                double delay = strum ? def.strumBeats * (double) order : 0.0;
                delay = std::min (delay, 0.5 * hit.length);
                notes.push_back ({ pitches[k], hit.start + delay, hit.length - delay,
                                   clampVelocity (velocity - (strum ? 3.0 * (double) order : 0.0)), false });
            }
        }
    }
    return notes;
}

std::vector<Note> renderBassPattern (const Progression& progression, const PatternOptions& options, Key key)
{
    std::vector<Note> notes;
    const BassPatternDef& def = pick (bassPatterns(), options.pattern);
    const double bar = options.beatsPerBar > tiny ? options.beatsPerBar : 4.0;
    const int base = 36 + 12 * (def.octave + std::min (std::max (options.octaveShift, -2), 2));

    for (size_t ci = 0; ci < progression.size(); ++ci)
    {
        const auto& event = progression[ci];
        if (! (event.length > tiny))
            continue;
        const Chord chord = event.chord;
        const Chord next = progression[(ci + 1) % progression.size()].chord;
        const int low = base + chord.bassPitchClass();
        const int rootPitch = low + wrap (chord.root - chord.bassPitchClass());
        const auto intervals = chordIntervals (chord.quality);
        int target = base + next.bassPitchClass();
        while (target - low > 6) target -= 12;
        while (low - target > 6) target += 12;
        const int direction = target >= low ? 1 : -1;

        auto hits = shapeHits (hitsInChord (event, def.steps, def.stepBeats, def.gate), event, swingUnit (def.steps, def.stepBeats), options);
        int walked = low;
        for (size_t h = 0; h < hits.size(); ++h)
        {
            const Hit& hit = hits[h];
            if (! (hit.length > tiny))
                continue;
            const char symbol = (char) std::tolower ((unsigned char) hit.symbol);
            int pitch = low;
            switch (symbol)
            {
                case 'o': pitch = low + 12; break;
                case 'f': pitch = rootPitch + (intervals.size() > 2 ? intervals[2] : 7); break;
                case 't': pitch = rootPitch + (intervals.size() > 1 ? intervals[1] : 4); break;
                case 's': pitch = hasSeventh (chord.quality) ? rootPitch + intervals[3] : low + 12; break;
                case 'w':
                {
                    // One step of the key's scale from the last walking note towards the next chord.
                    int p = walked + direction;
                    while (! isInScale (p, key)) p += direction;
                    if ((direction > 0 && p >= target) || (direction < 0 && p <= target) || target == low)
                        p = rootPitch + (intervals.size() > 2 ? intervals[2] : 7);
                    pitch = p;
                    walked = p;
                    break;
                }
                case 'a': pitch = target == low ? low + 7 : target - direction; break;
                default:  pitch = low; break;
            }
            // Chord starts and bar lines always have the chord's bass note.
            if (hit.atChordStart || onBarLine (hit.grid, bar))
                pitch = low;
            if (pitch > low + 14) pitch -= 12;
            pitch = fold (pitch, 21, 76);

            const bool accent = h == 0 || std::isupper ((unsigned char) hit.symbol);
            int velocity = clampVelocity (velocityFor (accent, hit.grid, options) + 8);
            if (options.humanise > 0.0)
            {
                Random r (mixSeed (options.seed ^ 0x42415353u, (std::uint64_t) (ci * 977 + h)));
                velocity = clampVelocity (velocity + options.humanise * 12.0 * (r.uniform() - 0.5));
            }
            Note n { pitch, hit.start, hit.length, velocity, false };
            if (def.slides && ! notes.empty())
            {
                const Note& prev = notes.back();
                n.slide = prev.pitch != n.pitch && prev.start + prev.length >= n.start - 1.0e-6;
            }
            notes.push_back (n);
        }
    }
    return notes;
}

PatternOptions randomChordOptions (std::uint32_t seed)
{
    Random r (mixSeed (seed, 0xC40Du));
    PatternOptions o;
    o.seed = seed;
    o.pattern = r.range (0, (int) chordPatterns().size());
    o.voicing = (Voicing) r.range (0, 3);
    o.registerCentre = r.range (55, 67);
    o.density = 0.75 + 0.25 * r.uniform();
    o.swing = r.chance (0.3) ? 0.35 * r.uniform() : 0.0;
    o.humanise = 0.25 * r.uniform();
    o.dynamics = 0.3 + 0.5 * r.uniform();
    const auto& def = chordPatterns()[(size_t) o.pattern];
    if (def.style == ChordStyle::Arpeggio && r.chance (0.4))
        o.arpRate = r.chance (0.5) ? 0.25 : 0.5;
    return o;
}

PatternOptions randomBassOptions (std::uint32_t seed)
{
    Random r (mixSeed (seed, 0xBA55u));
    PatternOptions o;
    o.seed = seed;
    o.pattern = r.range (0, (int) bassPatterns().size());
    o.density = 0.75 + 0.25 * r.uniform();
    o.swing = r.chance (0.3) ? 0.35 * r.uniform() : 0.0;
    o.humanise = 0.2 * r.uniform();
    o.dynamics = 0.3 + 0.5 * r.uniform();
    return o;
}
}
