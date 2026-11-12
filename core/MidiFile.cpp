#include "Backline.h"
#include <algorithm>
#include <cmath>

namespace backline
{
namespace
{
void put32 (std::vector<uint8_t>& out, uint32_t v) { for (int s = 24; s >= 0; s -= 8) out.push_back ((uint8_t) (v >> s)); }
void put16 (std::vector<uint8_t>& out, uint16_t v) { out.push_back ((uint8_t) (v >> 8)); out.push_back ((uint8_t) v); }

void putVarLen (std::vector<uint8_t>& out, uint32_t v)
{
    uint8_t bytes[5];
    int n = 0;
    bytes[n++] = (uint8_t) (v & 0x7f);
    while ((v >>= 7) != 0)
        bytes[n++] = (uint8_t) ((v & 0x7f) | 0x80);
    while (n > 0)
        out.push_back (bytes[--n]);
}
}

std::vector<uint8_t> writeMidiFile (const std::vector<Note>& notes, double bpm)
{
    const int ticksPerBeat = 480;
    struct Event { uint32_t tick; bool on; int pitch; int velocity; };
    std::vector<Event> events;
    for (const auto& n : notes)
    {
        const auto start = (uint32_t) std::lround (std::max (0.0, n.start) * ticksPerBeat);
        const auto end = std::max (start + 1, (uint32_t) std::lround ((n.start + n.length) * ticksPerBeat));
        events.push_back ({ start, true, std::clamp (n.pitch, 0, 127), std::clamp (n.velocity, 1, 127) });
        events.push_back ({ end, false, std::clamp (n.pitch, 0, 127), 0 });
    }
    std::stable_sort (events.begin(), events.end(), [] (const Event& a, const Event& b)
    {
        return a.tick != b.tick ? a.tick < b.tick : (! a.on && b.on); // note-offs first
    });

    std::vector<uint8_t> track;
    const auto usPerBeat = (uint32_t) std::lround (60000000.0 / std::max (1.0, bpm));
    track.insert (track.end(), { 0x00, 0xff, 0x51, 0x03,
                                 (uint8_t) (usPerBeat >> 16), (uint8_t) (usPerBeat >> 8), (uint8_t) usPerBeat });
    uint32_t last = 0;
    for (const auto& e : events)
    {
        putVarLen (track, e.tick - last);
        last = e.tick;
        track.push_back (e.on ? 0x90 : 0x80);
        track.push_back ((uint8_t) e.pitch);
        track.push_back ((uint8_t) e.velocity);
    }
    track.insert (track.end(), { 0x00, 0xff, 0x2f, 0x00 });

    std::vector<uint8_t> file { 'M', 'T', 'h', 'd' };
    put32 (file, 6); put16 (file, 0); put16 (file, 1); put16 (file, (uint16_t) ticksPerBeat);
    file.insert (file.end(), { 'M', 'T', 'r', 'k' });
    put32 (file, (uint32_t) track.size());
    file.insert (file.end(), track.begin(), track.end());
    return file;
}
}
