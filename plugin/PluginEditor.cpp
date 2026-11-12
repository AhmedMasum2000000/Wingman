#include "PluginEditor.h"
#include "BinaryData.h"

using namespace backline;

namespace ui
{
namespace
{
// Flat, high-contrast palette: near-black surfaces, white type, one blue accent.
const juce::Colour bg (0xff0f1113), surface (0xff17191c), control (0xff1f2226), border (0xff2b2e33),
                   text (0xfff7f7f8), textDim (0xff8c9096), accent (0xff3478ff), onAccent (0xffffffff),
                   record (0xffff4b4b);

juce::Font makeFont (const juce::Typeface::Ptr& face, float size)
{
    return juce::Font (juce::FontOptions (face).withHeight (size));
}

juce::Font display (float size)       { return makeFont (juce::SharedResourcePointer<Fonts>()->display, size); }
juce::Font displayMedium (float size) { return makeFont (juce::SharedResourcePointer<Fonts>()->displayMedium, size); }
juce::Font body (float size)          { return makeFont (juce::SharedResourcePointer<Fonts>()->body, size); }
juce::Font bodyBold (float size)      { return makeFont (juce::SharedResourcePointer<Fonts>()->bodyBold, size); }

void drawSurface (juce::Graphics& g, juce::Rectangle<float> area, float radius = 14.0f, juce::Colour edge = border)
{
    g.setColour (surface);
    g.fillRoundedRectangle (area, radius);
    g.setColour (edge);
    g.drawRoundedRectangle (area.reduced (0.5f), radius, 1.0f);
}

juce::String romanNumeral (Chord chord, Key key)
{
    static const char* const numerals[12] = { "I", "bII", "II", "bIII", "III", "IV", "#IV", "V", "bVI", "VI", "bVII", "VII" };
    const juce::String n (numerals[(chord.root - key.tonic + 12) % 12]);
    return chord.quality == Quality::Minor ? n.toLowerCase() : n;
}

// One octave of piano keys with the chord's notes lit.
void drawKeyboard (juce::Graphics& g, juce::Rectangle<float> area, Chord chord,
                   juce::Colour white, juce::Colour black, juce::Colour lit, juce::Colour gapColour)
{
    bool on[12] = {};
    on[chord.root] = on[(chord.root + (chord.quality == Quality::Major ? 4 : 3)) % 12] = on[(chord.root + 7) % 12] = true;
    static const int whites[7] = { 0, 2, 4, 5, 7, 9, 11 };
    static const int blacks[5] = { 1, 3, 6, 8, 10 };
    static const float blackPos[5] = { 1.0f, 2.0f, 4.0f, 5.0f, 6.0f };
    const float w = area.getWidth() / 7.0f;
    for (int i = 0; i < 7; ++i)
    {
        g.setColour (on[whites[i]] ? lit : white);
        g.fillRoundedRectangle (area.getX() + (float) i * w + 1.0f, area.getY(), w - 2.0f, area.getHeight(), 2.0f);
    }
    for (int i = 0; i < 5; ++i)
    {
        const juce::Rectangle<float> key (area.getX() + blackPos[i] * w - w * 0.28f, area.getY(), w * 0.56f, area.getHeight() * 0.6f);
        g.setColour (gapColour);
        g.fillRoundedRectangle (key.expanded (1.5f, 0.0f).withBottom (key.getBottom() + 1.5f), 2.0f);
        g.setColour (on[blacks[i]] ? lit : black);
        g.fillRoundedRectangle (key, 2.0f);
    }
}
}

//==============================================================================
Fonts::Fonts()
    : display (juce::Typeface::createSystemTypefaceFor (BinaryData::SpaceGroteskBold_ttf, BinaryData::SpaceGroteskBold_ttfSize)),
      displayMedium (juce::Typeface::createSystemTypefaceFor (BinaryData::SpaceGroteskMedium_ttf, BinaryData::SpaceGroteskMedium_ttfSize)),
      body (juce::Typeface::createSystemTypefaceFor (BinaryData::InterMedium_ttf, BinaryData::InterMedium_ttfSize)),
      bodyBold (juce::Typeface::createSystemTypefaceFor (BinaryData::InterSemiBold_ttf, BinaryData::InterSemiBold_ttfSize))
{
}

Look::Look()
{
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::PopupMenu::backgroundColourId, control);
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::headerTextColourId, textDim);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent);
    setColour (juce::PopupMenu::highlightedTextColourId, onAccent);
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, onAccent);
}

void Look::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto area = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
    const bool hot = box.isMouseOver (true);
    g.setColour (control);
    g.fillRoundedRectangle (area, 10.0f);
    g.setColour (hot ? textDim : border);
    g.drawRoundedRectangle (area, 10.0f, 1.0f);

    juce::Path arrow;
    const float cx = (float) width - 16.0f, cy = (float) height * 0.5f;
    arrow.startNewSubPath (cx - 4.0f, cy - 2.0f);
    arrow.lineTo (cx, cy + 2.0f);
    arrow.lineTo (cx + 4.0f, cy - 2.0f);
    g.setColour (hot ? text : textDim);
    g.strokePath (arrow, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font Look::getComboBoxFont (juce::ComboBox&) { return body (13.5f); }
juce::Font Look::getPopupMenuFont() { return body (14.0f); }
juce::Font Look::getTextButtonFont (juce::TextButton&, int) { return bodyBold (13.0f); }

void Look::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (control);
    g.setColour (border);
    g.drawRect (0, 0, width, height);
}

void Look::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&, bool highlighted, bool down)
{
    const auto area = button.getLocalBounds().toFloat().reduced (0.5f);
    if (button.getToggleState())
    {
        g.setColour (accent);
        g.fillRoundedRectangle (area, 10.0f);
        return;
    }
    const float alpha = button.isEnabled() ? 1.0f : 0.45f;
    g.setColour ((down ? border : control).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (area, 10.0f);
    g.setColour ((highlighted && button.isEnabled() ? textDim : border).withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (area, 10.0f, 1.0f);
}

void Look::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool, bool)
{
    const bool on = button.getToggleState();
    const auto track = juce::Rectangle<float> (0.0f, ((float) button.getHeight() - 20.0f) * 0.5f, 36.0f, 20.0f);
    g.setColour (on ? accent : control);
    g.fillRoundedRectangle (track, 10.0f);
    if (! on)
    {
        g.setColour (border);
        g.drawRoundedRectangle (track.reduced (0.5f), 10.0f, 1.0f);
    }
    g.setColour (on ? onAccent : textDim);
    g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ on ? track.getRight() - 10.0f : track.getX() + 10.0f, track.getCentreY() }));
    g.setColour (on ? text : textDim);
    g.setFont (body (13.5f));
    g.drawText (button.getButtonText(), button.getLocalBounds().withTrimmedLeft (46), juce::Justification::centredLeft);
}

void Look::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float, float,
                             juce::Slider::SliderStyle, juce::Slider&)
{
    const float cy = (float) y + (float) height * 0.5f;
    const auto track = juce::Rectangle<float> ((float) x, cy - 2.0f, (float) width, 4.0f);
    g.setColour (border);
    g.fillRoundedRectangle (track, 2.0f);
    g.setColour (text);
    g.fillRoundedRectangle (track.withRight (sliderPos), 2.0f);
    g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ sliderPos, cy }));
}

//==============================================================================
void RecordButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto centre = getLocalBounds().toFloat().getCentre();
    const float radius = (float) juce::jmin (getWidth(), getHeight()) * 0.5f - 7.0f;
    const auto disc = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);

    if (armed || recording) // pulsing ring
    {
        g.setColour (record.withAlpha (0.20f + 0.35f * pulse));
        g.drawEllipse (disc.expanded (3.0f + 2.5f * pulse), 1.5f);
    }

    // A white, slightly raised button; it turns red while a take is running.
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillEllipse (disc.translated (0.0f, 2.0f));
    g.setColour (recording ? record : (down ? juce::Colour (0xffd9d9dc) : highlighted ? juce::Colours::white : ui::text));
    g.fillEllipse (disc);
    g.setColour (juce::Colours::black.withAlpha (0.10f));
    g.drawEllipse (disc.reduced (1.0f), 2.0f);

    g.setColour (recording ? juce::Colours::white : record);
    if (armed || recording)
        g.fillRoundedRectangle (juce::Rectangle<float> (radius * 0.6f, radius * 0.6f).withCentre (centre), 3.0f);
    else
        g.fillEllipse (juce::Rectangle<float> (radius * 0.72f, radius * 0.72f).withCentre (centre));
}

//==============================================================================
void WaveformView::update (bool force)
{
    if (! force && processor.getRevision() == lastRevision && ! processor.isRecording())
        return;
    lastRevision = processor.getRevision();
    peaks = processor.getWaveform (juce::jmax (1, getWidth() / 4));
    repaint();
}

void WaveformView::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    const bool recording = processor.isRecording();
    const bool hasAudio = processor.getRecordedSeconds() > 0.0 && (recording || processor.hasCapture());
    const float top = 30.0f, midY = top + (area.getHeight() - top) * 0.5f, maxHalf = (area.getHeight() - top) * 0.5f - 2.0f;

    // Top row: either a sentence, or the facts about the take as small tags.
    if (facts.isEmpty())
    {
        g.setColour (recording ? record : textDim);
        g.setFont (body (13.0f));
        g.drawText (message, getLocalBounds().removeFromTop (22), juce::Justification::centredLeft);
    }
    else
    {
        float x = 0.0f;
        g.setFont (bodyBold (12.0f));
        for (int i = 0; i < facts.size(); ++i)
        {
            const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), facts[i]) + 18.0f;
            const juce::Rectangle<float> tag (x, 0.0f, w, 22.0f);
            const bool isKey = i == facts.size() - 1;
            g.setColour (isKey ? accent : control);
            g.fillRoundedRectangle (tag, 6.0f);
            g.setColour (isKey ? onAccent : text);
            g.drawText (facts[i], tag.toNearestInt(), juce::Justification::centred);
            x += w + 6.0f;
        }
    }

    if (! hasAudio)
    {
        g.setColour (border);
        for (float x = area.getX(); x < area.getRight(); x += 4.0f)
            g.fillRoundedRectangle (x, midY - 1.5f, 2.5f, 3.0f, 1.25f);
        return;
    }

    const int numChords = (int) processor.getProgression().size();
    if (! recording && numChords > 1) // chord boundaries
    {
        g.setColour (border);
        for (int i = 1; i < numChords; ++i)
            g.fillRect (area.getX() + area.getWidth() * (float) i / (float) numChords, top, 1.0f, area.getHeight() - top);
    }

    // Mirrored bars; the part already played turns blue.
    const double loop = processor.getLoopBeats(), head = processor.getPlayheadBeats();
    const float headX = (head >= 0.0 && loop > 0.0 && ! recording) ? area.getX() + area.getWidth() * (float) (head / loop) : -1.0f;
    float peakMax = 0.05f;
    for (float p : peaks) peakMax = juce::jmax (peakMax, p);
    for (size_t i = 0; i < peaks.size(); ++i)
    {
        const float x = area.getX() + (float) i * 4.0f;
        const float h = juce::jmax (1.5f, std::sqrt (peaks[i] / peakMax) * maxHalf);
        g.setColour (recording ? record : headX < 0.0f ? text.withAlpha (0.75f) : x <= headX ? accent : text.withAlpha (0.28f));
        g.fillRoundedRectangle (x, midY - h, 2.5f, h * 2.0f, 1.25f);
    }
    if (headX >= 0.0f)
    {
        g.setColour (accent);
        g.fillRoundedRectangle (headX - 1.0f, top - 4.0f, 2.0f, area.getHeight() - top + 4.0f, 1.0f);
    }
}

//==============================================================================
int ChordStrip::segmentAt (int x) const
{
    const int n = (int) processor.getProgression().size();
    return n == 0 ? -1 : juce::jlimit (0, n - 1, x * n / juce::jmax (1, getWidth()));
}

void ChordStrip::mouseMove (const juce::MouseEvent& e)
{
    const int now = segmentAt (e.x);
    if (now != hovered) { hovered = now; repaint(); }
}

void ChordStrip::paint (juce::Graphics& g)
{
    const auto& progression = processor.getProgression();
    const auto key = processor.getKey();
    if (progression.empty())
    {
        const auto zone = getLocalBounds().toFloat();
        juce::Path outline, dashed;
        outline.addRoundedRectangle (zone.reduced (0.5f), 14.0f);
        const float dashes[2] = { 6.0f, 6.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
        g.setColour (border);
        g.fillPath (dashed);
        g.setColour (text);
        g.setFont (display (24.0f));
        g.drawText ("Record a few bars to get chord ideas", getLocalBounds().translated (0, -13), juce::Justification::centred);
        g.setColour (textDim);
        g.setFont (body (13.5f));
        g.drawText ("Press the record button while your track plays, then press it again to stop.",
                    getLocalBounds().translated (0, 16), juce::Justification::centred);
        return;
    }

    const float width = (float) getWidth() / (float) progression.size();
    const float gap = progression.size() > 8 ? 2.0f : 4.0f;
    for (size_t i = 0; i < progression.size(); ++i)
    {
        const auto chord = progression[i];
        const bool active = (int) i == activeSegment, hot = (int) i == hovered;
        auto cell = juce::Rectangle<float> ((float) i * width, 0.0f, width, (float) getHeight());
        cell = cell.withTrimmedLeft (i == 0 ? 0.0f : gap).withTrimmedRight (i + 1 == progression.size() ? 0.0f : gap);

        if (active)
        {
            g.setColour (accent);
            g.fillRoundedRectangle (cell, 12.0f);
        }
        else
            drawSurface (g, cell, 12.0f, hot ? accent : border);

        const juce::Colour ink = active ? onAccent : text, soft = active ? onAccent.withAlpha (0.65f) : textDim;
        auto inner = cell.reduced (juce::jmin (14.0f, width * 0.12f), 12.0f);
        if (width > 44.0f)
        {
            const auto top = inner.removeFromTop (16.0f).toNearestInt();
            g.setFont (bodyBold (12.0f));
            g.setColour (soft);
            g.drawText (romanNumeral (chord, key), top, juce::Justification::centredLeft);
            if (width > 70.0f)
                g.drawText (juce::String ((int) i + 1), top, juce::Justification::centredRight);
        }
        const float keysHeight = juce::jmin (26.0f, inner.getHeight() * 0.28f);
        const auto keys = inner.removeFromBottom (keysHeight);
        if (width > 60.0f)
            drawKeyboard (g, keys.withSizeKeepingCentre (juce::jmin (keys.getWidth(), 112.0f), keysHeight), chord,
                          active ? onAccent.withAlpha (0.16f) : border, active ? onAccent.withAlpha (0.30f) : juce::Colour (0xff3a3e45),
                          active ? onAccent : accent, active ? accent : surface);

        g.setColour (ink);
        g.setFont (display (juce::jlimit (16.0f, 46.0f, width * 0.40f)));
        g.drawFittedText (chordName (chord), inner.toNearestInt(), juce::Justification::centred, 1);
    }
}

void ChordStrip::mouseDown (const juce::MouseEvent& e)
{
    const int segment = segmentAt (e.x);
    if (segment < 0)
        return;
    const auto& progression = processor.getProgression();
    const auto alternatives = processor.getAlternatives (segment);
    const auto key = processor.getKey();
    juce::PopupMenu menu;
    menu.setLookAndFeel (&getLookAndFeel());
    menu.addSectionHeader ("Chords that fit here, best first");
    for (size_t i = 0; i < alternatives.size(); ++i)
        menu.addItem ((int) i + 1, juce::String (chordName (alternatives[i])) + "    " + romanNumeral (alternatives[i], key),
                      true, alternatives[i] == progression[(size_t) segment]);

    const float width = (float) getWidth() / (float) progression.size();
    const auto target = localAreaToGlobal (juce::Rectangle<int> ((int) ((float) segment * width), 0, (int) width, getHeight()));
    juce::Component::SafePointer<ChordStrip> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (target).withMinimumWidth (200),
                        [safe, segment, alternatives] (int result)
                        {
                            if (safe != nullptr && result > 0)
                                safe->processor.setChord (segment, alternatives[(size_t) result - 1]);
                        });
}

//==============================================================================
PartTile::PartTile (BacklineProcessor& p, BacklineProcessor::Part whichPart, juce::String name, juce::Colour c, bool hasRhythm)
    : processor (p), part (whichPart), title (std::move (name)), colour (c)
{
    if (hasRhythm)
        addAndMakeVisible (rhythm);
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
}

void PartTile::resized()
{
    rhythm.setBounds (getWidth() - 152, 12, 138, 28);
}

void PartTile::update()
{
    notes = processor.getPartNotes (part);
    repaint();
}

void PartTile::paint (juce::Graphics& g)
{
    const bool ready = ! notes.empty();
    drawSurface (g, getLocalBounds().toFloat(), 14.0f, ready && hover ? accent : border);

    auto inner = getLocalBounds().reduced (14, 12);
    g.setColour (ready ? text : textDim);
    g.setFont (displayMedium (16.0f));
    g.drawText (title, inner.removeFromTop (28), juce::Justification::centredLeft);

    auto footer = inner.removeFromBottom (18);
    g.setFont (body (12.5f));
    g.setColour (ready && hover ? accent : textDim);
    g.drawText (ready ? "Drag into your DAW" : (part == BacklineProcessor::Part::Melody ? "For single-note audio" : "Nothing yet"),
                footer, juce::Justification::centredLeft);
    if (ready) // grip dots
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 3; ++c)
                g.fillEllipse ((float) footer.getRight() - 16.0f + (float) c * 6.0f, (float) footer.getY() + 4.0f + (float) r * 6.0f, 2.5f, 2.5f);

    // Piano roll of the part.
    const auto roll = inner.reduced (0, 8).toFloat();
    g.setColour (bg);
    g.fillRoundedRectangle (roll, 8.0f);
    if (! ready)
        return;
    int low = 127, high = 0;
    double length = 0.0;
    for (const auto& n : notes)
    {
        low = juce::jmin (low, n.pitch); high = juce::jmax (high, n.pitch);
        length = juce::jmax (length, n.start + n.length);
    }
    length = juce::jmax (length, part == BacklineProcessor::Part::Melody ? 0.0 : processor.getLoopBeats());
    const int rows = juce::jmax (6, high - low + 1);
    const auto grid = roll.reduced (6.0f, 5.0f);
    const float rowHeight = grid.getHeight() / (float) rows;
    const float offset = (float) (rows - (high - low + 1)) * 0.5f;
    g.setColour (colour);
    for (const auto& n : notes)
    {
        const float x = grid.getX() + grid.getWidth() * (float) (n.start / length);
        const float w = juce::jmax (2.5f, grid.getWidth() * (float) (n.length / length) - 1.5f);
        const float y = grid.getBottom() - ((float) (n.pitch - low) + offset + 1.0f) * rowHeight;
        g.fillRoundedRectangle (x, y + 0.5f, w, juce::jmax (2.0f, rowHeight - 1.0f), 1.5f);
    }
}

void PartTile::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || notes.empty() || e.getDistanceFromDragStart() < 6)
        return;
    const auto file = processor.writeMidiFile (part);
    if (file.existsAsFile())
    {
        dragging = true;
        juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this);
    }
}
}

//==============================================================================
using namespace ui;

BacklineEditor::BacklineEditor (BacklineProcessor& p)
    : AudioProcessorEditor (p), processor (p), waveform (p), chordStrip (p),
      chordsTile (p, BacklineProcessor::Part::Chords, "Chords", accent, true),
      bassTile (p, BacklineProcessor::Part::Bass, "Bass", text, true),
      melodyTile (p, BacklineProcessor::Part::Melody, "Audio to MIDI", juce::Colour (0xffa9adb4), false)
{
    setLookAndFeel (&look);
    for (auto* c : std::initializer_list<juce::Component*> { &recordButton, &waveform, &tonicBox, &modeBox, &changeBox,
             &chordStrip, &previewButton, &volume, &chordsTile, &bassTile, &melodyTile })
        addAndMakeVisible (c);

    recordButton.setTooltip ("Record audio from this track");
    recordButton.onClick = [this] { processor.setRecordArmed (! processor.isRecordArmed()); refresh(); };

    for (int i = 0; i < 12; ++i) tonicBox.addItem (pitchClassName (i), i + 1);
    modeBox.addItemList ({ "major", "minor" }, 1);
    auto keyChanged = [this] { processor.setKey ({ tonicBox.getSelectedId() - 1, modeBox.getSelectedId() == 2, 1.0f }); };
    tonicBox.onChange = keyChanged;
    modeBox.onChange = keyChanged;

    changeBox.addItem ("Every 2 beats", 2); changeBox.addItem ("Every bar", 4); changeBox.addItem ("Every 2 bars", 8);
    changeBox.onChange = [this] { processor.setBeatsPerChord ((double) changeBox.getSelectedId()); };

    chordsTile.rhythm.addItemList ({ "Sustained", "Quarter notes", "Offbeat stabs", "Dotted" }, 1);
    chordsTile.rhythm.onChange = [this] { processor.setChordRhythm ((ChordRhythm) (chordsTile.rhythm.getSelectedId() - 1)); };
    bassTile.rhythm.addItemList ({ "Sustained", "Eighth notes", "Octaves", "Root and fifth", "Offbeat" }, 1);
    bassTile.rhythm.onChange = [this] { processor.setBassRhythm ((BassRhythm) (bassTile.rhythm.getSelectedId() - 1)); };

    for (int i = 0; i < 4; ++i)
    {
        auto* b = ideaButtons.add (new juce::TextButton ("Idea " + juce::String (i + 1)));
        b->onClick = [this, i] { processor.selectIdea (i); };
        addAndMakeVisible (b);
    }

    previewButton.onClick = [this] { processor.previewEnabled = previewButton.getToggleState(); };
    volume.setSliderStyle (juce::Slider::LinearHorizontal);
    volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    volume.setRange (0.0, 1.0);
    volume.onValueChange = [this] { processor.previewGain = (float) volume.getValue(); };

    setSize (900, 580);
    refresh();
    startTimerHz (30);
}

BacklineEditor::~BacklineEditor() { setLookAndFeel (nullptr); }

void BacklineEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);

    // Logo: a accent tile with three stacked bars, like a chord.
    const juce::Rectangle<float> tile (24.0f, 20.0f, 34.0f, 34.0f);
    g.setColour (accent);
    g.fillRoundedRectangle (tile, 9.0f);
    const float widths[3] = { 18.0f, 11.0f, 15.0f };
    g.setColour (onAccent);
    for (int i = 0; i < 3; ++i)
        g.fillRoundedRectangle (tile.getX() + 8.0f, tile.getY() + 8.5f + (float) i * 6.5f, widths[i], 4.0f, 2.0f);
    g.setColour (text);
    g.setFont (display (23.0f));
    g.drawText ("Backline", 68, 20, 200, 34, juce::Justification::centredLeft);

    g.setColour (textDim);
    g.setFont (body (13.0f));
    g.drawText ("Key", juce::Rectangle<int> (tonicBox.getX() - 44, tonicBox.getY(), 36, tonicBox.getHeight()), juce::Justification::centredRight);
    drawSurface (g, recordButton.getBounds().getUnion (waveform.getBounds()).expanded (16, 14).toFloat());
}

void BacklineEditor::resized()
{
    auto area = getLocalBounds().reduced (24, 20);

    auto header = area.removeFromTop (34);
    modeBox.setBounds (header.removeFromRight (96));
    header.removeFromRight (6);
    tonicBox.setBounds (header.removeFromRight (66));
    header.removeFromRight (68);
    volume.setBounds (header.removeFromRight (96).reduced (0, 4));
    header.removeFromRight (6);
    previewButton.setBounds (header.removeFromRight (112));
    area.removeFromTop (32);

    auto capture = area.removeFromTop (88).reduced (16, 0);
    recordButton.setBounds (capture.removeFromLeft (76).withSizeKeepingCentre (76, 76));
    capture.removeFromLeft (12);
    waveform.setBounds (capture);
    area.removeFromTop (28);

    auto row = area.removeFromTop (30);
    for (auto* b : ideaButtons)
    {
        b->setBounds (row.removeFromLeft (84));
        row.removeFromLeft (6);
    }
    changeBox.setBounds (row.removeFromRight (150));
    area.removeFromTop (12);

    chordStrip.setBounds (area.removeFromTop (156));
    area.removeFromTop (16);

    const int tileWidth = (area.getWidth() - 24) / 3;
    chordsTile.setBounds (area.removeFromLeft (tileWidth));
    area.removeFromLeft (12);
    bassTile.setBounds (area.removeFromLeft (tileWidth));
    area.removeFromLeft (12);
    melodyTile.setBounds (area);
}

void BacklineEditor::timerCallback()
{
    ++tick;
    const bool armed = processor.isRecordArmed(), recording = processor.isRecording();
    if (processor.getRevision() != lastRevision || armed != lastArmed || recording != lastRecording || (recording && tick % 6 == 0))
        refresh();

    if (armed || recording)
    {
        recordButton.pulse = 0.5f + 0.5f * std::sin ((float) tick * 0.21f);
        recordButton.repaint();
    }
    if (recording && tick % 3 == 0)
        waveform.update (true);

    // Follow the transport.
    const double head = processor.getPlayheadBeats(), loop = processor.getLoopBeats();
    const int count = (int) processor.getProgression().size();
    const int active = (head >= 0.0 && loop > 0.0 && count > 0) ? juce::jlimit (0, count - 1, (int) (head / loop * count)) : -1;
    if (active != chordStrip.activeSegment)
    {
        chordStrip.activeSegment = active;
        chordStrip.repaint();
    }
    if (head >= 0.0 && ! recording)
        waveform.repaint();
}

void BacklineEditor::refresh()
{
    lastRevision = processor.getRevision();
    const bool armed = lastArmed = processor.isRecordArmed();
    const bool recording = lastRecording = processor.isRecording();
    recordButton.armed = armed;
    recordButton.recording = recording;
    recordButton.repaint();

    waveform.facts.clear();
    if (recording)
        waveform.message = "Recording  " + juce::String (processor.getRecordedSeconds(), 1) + " s";
    else if (armed)
        waveform.message = "Waiting for playback to start";
    else if (processor.hasCapture())
    {
        const int bars = juce::roundToInt (processor.getLoopBeats() / 4.0);
        waveform.facts = { juce::String (bars) + (bars == 1 ? " bar" : " bars"),
                           juce::String (juce::roundToInt (processor.getBpm())) + " BPM", keyName (processor.getKey()) };
    }
    else if (processor.hasProgression())
        waveform.message = "Progression restored. Record again for new ideas.";
    else if (processor.getRecordedSeconds() > 0.0)
        waveform.message = "That take was too short. Record at least one bar.";
    else
        waveform.message = "Put Backline on the track you want to build around.";

    const auto key = processor.getKey();
    tonicBox.setSelectedId (key.tonic + 1, juce::dontSendNotification);
    modeBox.setSelectedId (key.minor ? 2 : 1, juce::dontSendNotification);
    changeBox.setSelectedId ((int) processor.getBeatsPerChord(), juce::dontSendNotification);
    chordsTile.rhythm.setSelectedId ((int) processor.getChordRhythm() + 1, juce::dontSendNotification);
    bassTile.rhythm.setSelectedId ((int) processor.getBassRhythm() + 1, juce::dontSendNotification);
    previewButton.setToggleState (processor.previewEnabled.load(), juce::dontSendNotification);
    volume.setValue (processor.previewGain.load(), juce::dontSendNotification);

    for (int i = 0; i < ideaButtons.size(); ++i)
    {
        ideaButtons[i]->setEnabled (i < processor.getNumIdeas());
        ideaButtons[i]->setToggleState (i == processor.getSelectedIdea() && processor.getNumIdeas() > 0, juce::dontSendNotification);
    }
    waveform.update (true);
    chordStrip.repaint();
    chordsTile.update(); bassTile.update(); melodyTile.update();
}
