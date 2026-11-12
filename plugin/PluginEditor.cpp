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

float textWidth (const juce::Font& font, const juce::String& s) { return juce::GlyphArrangement::getStringWidth (font, s); }

const juce::String ellipsis (juce::CharPointer_UTF8 ("\xe2\x80\xa6"));

void drawSurface (juce::Graphics& g, juce::Rectangle<float> area, float radius = 14.0f, juce::Colour edge = border)
{
    g.setColour (surface);
    g.fillRoundedRectangle (area, radius);
    g.setColour (edge);
    g.drawRoundedRectangle (area.reduced (0.5f), radius, 1.0f);
}

// Six dots that say "you can drag this".
void drawGrip (juce::Graphics& g, float x, float y)
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 2; ++c)
            g.fillEllipse (x + (float) c * 4.5f, y + (float) r * 4.5f, 2.5f, 2.5f);
}

// One octave of piano keys with the chord's notes lit.
void drawKeyboard (juce::Graphics& g, juce::Rectangle<float> area, Chord chord,
                   juce::Colour white, juce::Colour black, juce::Colour lit, juce::Colour gapColour)
{
    bool on[12] = {};
    for (int pc : chordPitchClasses (chord))
        on[((pc % 12) + 12) % 12] = true;
    on[chord.bassPitchClass()] = true;
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

// The largest size (from `largest` down to `smallest`) at which the text fits the width.
float fittingSize (const juce::String& s, float largest, float smallest, float maxWidth)
{
    float size = largest;
    while (size > smallest && textWidth (display (size), s) > maxWidth)
        size -= 1.0f;
    return juce::jmax (size, smallest);
}

// A position as musicians read it: "Bar 3, beat 2" (beats from 1).
juce::String barAndBeat (double beat, double beatsPerBar)
{
    const double bar = juce::jmax (1.0, beatsPerBar);
    const int barIndex = (int) std::floor (beat / bar + 1.0e-9);
    const double inBar = beat - barIndex * bar + 1.0;
    const bool whole = std::abs (inBar - std::round (inBar)) < 1.0e-6;
    return "Bar " + juce::String (barIndex + 1) + ", beat " + (whole ? juce::String (juce::roundToInt (inBar)) : juce::String (inBar, 1));
}
}

juce::String romanNumeral (Chord chord, Key key)
{
    static const char* const numerals[12] = { "I", "bII", "II", "bIII", "III", "IV", "#IV", "V", "bVI", "VI", "bVII", "VII" };
    juce::String n (numerals[(((chord.root - key.tonic) % 12) + 12) % 12]);
    if (hasMinorThird (chord.quality))
        n = n.toLowerCase();
    switch (chord.quality)
    {
        case Quality::Dominant7:  return n + "7";
        case Quality::Major7:     return n + "maj7";
        case Quality::Minor7:     return n + "7";
        case Quality::Sus2:       return n + "sus2";
        case Quality::Sus4:       return n + "sus4";
        case Quality::Diminished: return n + juce::String (juce::CharPointer_UTF8 ("\xc2\xb0"));
        case Quality::Major:
        case Quality::Minor:      break;
    }
    return n;
}

//==============================================================================
StripLayout StripLayout::make (const Progression& progression, double beatsPerBar, float width, float height)
{
    StripLayout l;
    l.width = width;
    l.height = height;
    l.loopBeats = progressionLength (progression);
    if (progression.empty() || l.loopBeats <= 0.0)
    {
        l.beatsPerRow = juce::jmax (1.0, l.loopBeats);
        return l;
    }

    // Rows hold whole bars; use as few as keep a typical chord at least 48 px wide.
    std::vector<double> lengths;
    for (const auto& e : progression)
        lengths.push_back (e.length);
    std::sort (lengths.begin(), lengths.end());
    const double typical = lengths[lengths.size() / 2];
    const double bar = juce::jmax (1.0, beatsPerBar);
    const int maxRows = juce::jlimit (1, 6, (int) (height / 26.0f));
    auto beatsFor = [&] (int rows) { return rows == 1 ? l.loopBeats : std::ceil (l.loopBeats / rows / bar - 1.0e-9) * bar; };
    auto rowsFor = [&] (double perRow) { return juce::jmax (1, (int) std::ceil (l.loopBeats / perRow - 1.0e-9)); };
    auto exact = [&] (double perRow) { return std::abs (l.loopBeats / perRow - std::round (l.loopBeats / perRow)) < 1.0e-9; };

    int rows = maxRows;
    for (int r = 1; r <= maxRows; ++r)
        if (rowsFor (beatsFor (r)) == r && typical / beatsFor (r) * width >= 48.0)
        {
            rows = r;
            break;
        }
    // A last row only partly filled looks unfinished: take one more row if that divides evenly.
    if (! exact (beatsFor (rows)) && rows < maxRows && exact (beatsFor (rows + 1)) && rowsFor (beatsFor (rows + 1)) == rows + 1)
        ++rows;
    l.beatsPerRow = beatsFor (rows);
    l.rows = rowsFor (l.beatsPerRow);
    return l;
}

std::vector<juce::Rectangle<float>> StripLayout::pieces (double start, double length) const
{
    std::vector<juce::Rectangle<float>> result;
    if (beatsPerRow <= 0.0 || length <= 0.0)
        return result;
    const double end = start + length;
    const float rh = rowHeight();
    double s = start;
    while (s < end - 1.0e-9)
    {
        const int row = (int) std::floor (s / beatsPerRow + 1.0e-9);
        const double rowStart = row * beatsPerRow, e = juce::jmin (end, rowStart + beatsPerRow);
        const float x0 = (float) ((s - rowStart) / beatsPerRow) * width, x1 = (float) ((e - rowStart) / beatsPerRow) * width;
        result.push_back ({ x0, (float) row * rh, x1 - x0, rh });
        s = e;
    }
    return result;
}

double StripLayout::beatAt (juce::Point<float> point) const
{
    const int row = juce::jlimit (0, rows - 1, (int) std::floor (point.y / rowHeight()));
    const double frac = juce::jlimit (0.0, 1.0, width > 0.0f ? (double) (point.x / width) : 0.0);
    return juce::jlimit (0.0, loopBeats, (row + frac) * beatsPerRow);
}

juce::Line<float> StripLayout::boundaryLine (double beat) const
{
    const int row = juce::jmax (0, (int) std::floor (beat / beatsPerRow - 1.0e-9));
    const float x = (float) ((beat - row * beatsPerRow) / beatsPerRow) * width, rh = rowHeight();
    return { x, (float) row * rh, x, (float) (row + 1) * rh };
}

int StripLayout::boundaryAt (const Progression& progression, juce::Point<float> point, float tolerance) const
{
    int best = -1;
    float bestDistance = tolerance;
    auto consider = [&] (int index, juce::Line<float> line)
    {
        if (point.y < line.getStartY() || point.y > line.getEndY())
            return;
        const float d = std::abs (point.x - line.getStartX());
        if (d <= bestDistance)
        {
            best = index;
            bestDistance = d;
        }
    };
    for (size_t i = 0; i + 1 < progression.size(); ++i)
    {
        const double beat = progression[i].start + progression[i].length;
        consider ((int) i, boundaryLine (beat));
        const double rowsIn = beat / beatsPerRow;
        if (std::abs (rowsIn - std::round (rowsIn)) < 1.0e-9) // on a row break: also the start of the next row
        {
            const float rh = rowHeight(), y = (float) std::round (rowsIn) * rh;
            consider ((int) i, { 0.0f, y, 0.0f, y + rh });
        }
    }
    return best;
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
    setColour (juce::TextEditor::backgroundColourId, control);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.45f));
    setColour (juce::TextEditor::highlightedTextColourId, onAccent);
    setColour (juce::TextEditor::outlineColourId, border);
    setColour (juce::TextEditor::focusedOutlineColourId, accent);
    setColour (juce::CaretComponent::caretColourId, text);
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

void Look::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool)
{
    const bool on = button.getToggleState() && button.isEnabled();
    const float alpha = button.isEnabled() ? 1.0f : 0.45f;
    const auto track = juce::Rectangle<float> (0.0f, ((float) button.getHeight() - 20.0f) * 0.5f, 36.0f, 20.0f);
    g.setColour ((on ? accent : control).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (track, 10.0f);
    if (! on)
    {
        g.setColour ((highlighted && button.isEnabled() ? textDim : border).withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (track.reduced (0.5f), 10.0f, 1.0f);
    }
    g.setColour ((on ? onAccent : textDim).withMultipliedAlpha (alpha));
    g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ on ? track.getRight() - 10.0f : track.getX() + 10.0f, track.getCentreY() }));
    g.setColour ((on ? text : textDim).withMultipliedAlpha (alpha));
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

juce::Rectangle<int> Look::getTooltipBounds (const juce::String& tip, juce::Point<int> pos, juce::Rectangle<int> parentArea)
{
    const int w = juce::roundToInt (textWidth (body (12.5f), tip)) + 22, h = 28;
    return juce::Rectangle<int> (pos.x > parentArea.getCentreX() ? pos.x - (w + 12) : pos.x + 20,
                                 pos.y > parentArea.getCentreY() ? pos.y - (h + 8) : pos.y + 14, w, h)
        .constrainedWithin (parentArea);
}

void Look::drawTooltip (juce::Graphics& g, const juce::String& tip, int width, int height)
{
    const auto area = juce::Rectangle<float> ((float) width, (float) height);
    g.setColour (control);
    g.fillRoundedRectangle (area, 8.0f);
    g.setColour (border);
    g.drawRoundedRectangle (area.reduced (0.5f), 8.0f, 1.0f);
    g.setColour (text);
    g.setFont (body (12.5f));
    g.drawText (tip, area.reduced (11.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft);
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

void TransportButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto area = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (down ? border : control);
    g.fillRoundedRectangle (area, 10.0f);
    g.setColour (highlighted ? textDim : border);
    g.drawRoundedRectangle (area, 10.0f, 1.0f);

    const auto c = area.getCentre();
    g.setColour (ui::text);
    if (playing)
        g.fillRoundedRectangle (juce::Rectangle<float> (11.0f, 11.0f).withCentre (c), 2.0f);
    else
    {
        juce::Path triangle;
        triangle.addTriangle (c.x - 4.5f, c.y - 6.5f, c.x - 4.5f, c.y + 6.5f, c.x + 6.5f, c.y);
        g.fillPath (triangle, {});
        g.strokePath (triangle, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

//==============================================================================
TempoField::TempoField()
{
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
}

juce::String TempoField::format (double bpm)
{
    const double rounded = std::round (bpm * 10.0) / 10.0;
    return std::abs (rounded - std::round (rounded)) < 0.05 ? juce::String (juce::roundToInt (rounded)) : juce::String (rounded, 1);
}

void TempoField::setTempo (double bpm)
{
    if (! isEditing() && std::abs (bpm - shown) > 1.0e-9)
    {
        shown = bpm;
        repaint();
    }
}

void TempoField::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (control);
    g.fillRoundedRectangle (area, 10.0f);
    g.setColour (dragging ? accent : isMouseOver (true) ? textDim : border);
    g.drawRoundedRectangle (area, 10.0f, 1.0f);
    if (editor != nullptr)
        return;

    auto inner = getLocalBounds().reduced (12, 0);
    const auto value = format (shown);
    const auto valueFont = bodyBold (13.5f);
    const int valueWidth = juce::roundToInt (textWidth (valueFont, value)) + 4;
    g.setColour (text);
    g.setFont (valueFont);
    g.drawText (value, inner.removeFromLeft (valueWidth), juce::Justification::centredLeft, false);
    g.setColour (textDim);
    g.setFont (body (13.0f));
    g.drawText ("BPM", inner.withTrimmedLeft (2).withTrimmedRight (14), juce::Justification::centredLeft, true);

    // Up/down chevrons: drag to change.
    const float cx = (float) getWidth() - 16.0f, cy = (float) getHeight() * 0.5f;
    juce::Path chevrons;
    chevrons.startNewSubPath (cx - 3.5f, cy - 2.5f); chevrons.lineTo (cx, cy - 6.0f); chevrons.lineTo (cx + 3.5f, cy - 2.5f);
    chevrons.startNewSubPath (cx - 3.5f, cy + 2.5f); chevrons.lineTo (cx, cy + 6.0f); chevrons.lineTo (cx + 3.5f, cy + 2.5f);
    g.setColour (isMouseOver (true) ? text : textDim);
    g.strokePath (chevrons, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void TempoField::resized()
{
    if (editor != nullptr)
        editor->setBounds (getLocalBounds());
}

void TempoField::mouseDown (const juce::MouseEvent&)
{
    dragStart = shown;
    dragging = moved = false;
}

void TempoField::mouseDrag (const juce::MouseEvent& e)
{
    if (! moved && e.getDistanceFromDragStart() < 3)
        return;
    moved = dragging = true;
    const double perPixel = e.mods.isShiftDown() ? 0.1 : 0.5; // shift: fine steps
    double next = dragStart - (double) e.getDistanceFromDragStartY() * perPixel;
    next = e.mods.isShiftDown() ? std::round (next * 10.0) / 10.0 : std::round (next);
    next = juce::jlimit (BacklineProcessor::minManualTempo, BacklineProcessor::maxManualTempo, next);
    if (std::abs (next - shown) > 1.0e-9)
    {
        shown = next;
        repaint();
    }
}

void TempoField::mouseUp (const juce::MouseEvent&)
{
    const bool wasDrag = moved;
    dragging = moved = false;
    repaint();
    if (wasDrag)
        commit (shown);
    else
        showEditor();
}

void TempoField::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (editor != nullptr || std::abs (wheel.deltaY) < 1.0e-6f)
        return;
    shown = juce::jlimit (BacklineProcessor::minManualTempo, BacklineProcessor::maxManualTempo,
                          std::round (shown) + (wheel.deltaY > 0.0f ? 1.0 : -1.0));
    repaint();
    startTimer (450); // apply once the wheel stops
}

void TempoField::timerCallback()
{
    stopTimer();
    commit (shown);
}

void TempoField::commit (double bpm)
{
    if (onCommit != nullptr)
        onCommit (bpm);
}

void TempoField::showEditor()
{
    if (editor != nullptr)
        return;
    editor = std::make_unique<juce::TextEditor>();
    editor->setFont (bodyBold (13.5f));
    editor->setIndents (11, 0);
    editor->setJustification (juce::Justification::centredLeft);
    editor->setInputRestrictions (5, "0123456789.");
    editor->setText (format (shown), false);
    editor->setBounds (getLocalBounds());
    editor->onReturnKey = [this] { closeEditor (true); };
    editor->onEscapeKey = [this] { closeEditor (false); };
    editor->onFocusLost = [this] { closeEditor (true); };
    addAndMakeVisible (*editor);
    editor->selectAll();
    editor->grabKeyboardFocus();
    repaint();
}

void TempoField::closeEditor (bool apply)
{
    if (editor == nullptr || ! editor->isVisible())
        return;
    const double typed = editor->getText().getDoubleValue();
    editor->setVisible (false); // marks it closed; it is deleted after this callback returns
    juce::Component::SafePointer<TempoField> safe (this);
    juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->editor.reset(); safe->repaint(); } });
    if (apply && typed > 0.0)
    {
        shown = juce::jlimit (BacklineProcessor::minManualTempo, BacklineProcessor::maxManualTempo, std::round (typed * 10.0) / 10.0);
        commit (shown);
    }
    repaint();
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
    const float top = (float) topRow + 8.0f, midY = top + (area.getHeight() - top) * 0.5f, maxHalf = (area.getHeight() - top) * 0.5f - 2.0f;
    const int rowWidth = juce::jmax (0, getWidth() - topRowInset);

    // Top row: either a sentence, or the take's name and the facts about it as small tags.
    if (facts.isEmpty())
    {
        g.setColour (messageIsError || recording ? record : textDim);
        g.setFont (body (13.0f));
        g.drawText (message, juce::Rectangle<int> (0, 0, rowWidth, topRow), juce::Justification::centredLeft, true);
    }
    else
    {
        const auto tagFont = bodyBold (12.0f);
        float tagsWidth = 0.0f;
        for (const auto& f : facts)
            tagsWidth += textWidth (tagFont, f) + 18.0f + 6.0f;
        float x = 0.0f;
        if (title.isNotEmpty())
        {
            const auto titleFont = bodyBold (13.5f);
            const float room = (float) rowWidth - tagsWidth - 12.0f;
            const float w = juce::jmin (room, textWidth (titleFont, title) + 2.0f);
            if (w > 24.0f)
            {
                g.setColour (text);
                g.setFont (titleFont);
                g.drawText (title, juce::Rectangle<float> (0.0f, 0.0f, w, (float) topRow), juce::Justification::centredLeft, true);
                x = w + 12.0f;
            }
        }
        g.setFont (tagFont);
        for (int i = 0; i < facts.size(); ++i)
        {
            const float w = textWidth (tagFont, facts[i]) + 18.0f;
            const juce::Rectangle<float> tag (x, ((float) topRow - 22.0f) * 0.5f, w, 22.0f);
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

    const auto progression = processor.getProgression();
    const double loop = progressionLength (progression), head = processor.getPlayheadBeats();
    if (! recording && progression.size() > 1 && loop > 0.0) // chord boundaries
    {
        g.setColour (border);
        for (size_t i = 1; i < progression.size(); ++i)
            g.fillRect (area.getX() + area.getWidth() * (float) (progression[i].start / loop), top, 1.0f, area.getHeight() - top);
    }

    // Mirrored bars; the part already played turns blue.
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
StripLayout ChordStrip::layout() const
{
    return StripLayout::make (processor.getProgression(), processor.getBeatsPerBar(), (float) getWidth(), (float) getHeight());
}

int ChordStrip::chordAt (juce::Point<float> point) const
{
    const auto progression = processor.getProgression();
    const auto l = StripLayout::make (progression, processor.getBeatsPerBar(), (float) getWidth(), (float) getHeight());
    for (size_t i = 0; i < progression.size(); ++i)
        for (const auto& piece : l.pieces (progression[i].start, progression[i].length))
            if (piece.contains (point))
                return (int) i;
    return -1;
}

void ChordStrip::showBoundary (int boundary, bool dragging)
{
    hoveredBoundary = boundary;
    draggedBoundary = dragging ? boundary : -1;
    repaint();
}

void ChordStrip::paint (juce::Graphics& g)
{
    const auto progression = processor.getProgression();
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
        g.drawText (emptyTitle, getLocalBounds().reduced (20, 0).translated (0, -13), juce::Justification::centred, true);
        g.setColour (textDim);
        g.setFont (body (13.5f));
        g.drawText (emptyHint, getLocalBounds().reduced (20, 0).translated (0, 16), juce::Justification::centred, true);
        return;
    }

    const auto l = StripLayout::make (progression, processor.getBeatsPerBar(), (float) getWidth(), (float) getHeight());
    const bool manyRows = l.rows > 1;
    for (size_t i = 0; i < progression.size(); ++i)
    {
        const auto& event = progression[i];
        const bool active = (int) i == activeSegment, hot = (int) i == hovered && hoveredBoundary < 0 && draggedBoundary < 0;
        const auto pieces = l.pieces (event.start, event.length);
        size_t main = 0;
        for (size_t p = 1; p < pieces.size(); ++p)
            if (pieces[p].getWidth() > pieces[main].getWidth() + 0.5f)
                main = p;

        const juce::String name (chordName (event.chord, key));
        for (size_t p = 0; p < pieces.size(); ++p)
        {
            const auto& full = pieces[p];
            const float halfGap = full.getWidth() > 24.0f ? 2.0f : 1.0f;
            const auto cell = full.reduced (halfGap, manyRows ? 2.0f : 0.0f);
            const float width = cell.getWidth(), height = cell.getHeight();
            if (width < 3.0f)
                continue;
            const float radius = juce::jmin (12.0f, height * 0.25f, width * 0.3f);
            if (active)
            {
                g.setColour (accent);
                g.fillRoundedRectangle (cell, radius);
            }
            else
                drawSurface (g, cell, radius, hot ? accent : border);

            const juce::Colour ink = active ? onAccent : text, soft = active ? onAccent.withAlpha (0.65f) : textDim;
            auto inner = cell.reduced (juce::jmin (14.0f, width * 0.12f), juce::jmin (12.0f, height * 0.08f));
            if (p == main && width > 44.0f && height > 70.0f)
            {
                const auto topRow = inner.removeFromTop (16.0f).toNearestInt();
                const auto numeral = romanNumeral (event.chord, key);
                const bool showIndex = width > 70.0f && textWidth (bodyBold (12.0f), numeral) + 24.0f < inner.getWidth();
                g.setFont (bodyBold (12.0f));
                g.setColour (soft);
                g.drawText (numeral, showIndex ? topRow.withTrimmedRight (22) : topRow, juce::Justification::centredLeft, true);
                if (showIndex)
                    g.drawText (juce::String ((int) i + 1), topRow, juce::Justification::centredRight);
            }
            if (height > 110.0f) // room for the keys is kept on every card so the names line up
            {
                const float keysHeight = juce::jmin (26.0f, inner.getHeight() * 0.28f);
                const auto keys = inner.removeFromBottom (keysHeight);
                if (p == main && width > 44.0f)
                    drawKeyboard (g, keys.withSizeKeepingCentre (juce::jmin (keys.getWidth(), 112.0f), keysHeight), event.chord,
                              active ? onAccent.withAlpha (0.16f) : border, active ? onAccent.withAlpha (0.30f) : juce::Colour (0xff3a3e45),
                              active ? onAccent : accent, active ? accent : surface);
            }

            // The name, as large as fits (a continuation piece on another row gets it too).
            const float room = juce::jmax (4.0f, cell.getWidth() - (width > 40.0f ? 12.0f : 6.0f));
            const float largest = juce::jlimit (11.0f, 46.0f, juce::jmin (width * 0.40f, height * 0.55f));
            const float size = fittingSize (name, largest, 9.0f, juce::jmin (room, inner.getWidth() + 8.0f));
            const float nameWidth = textWidth (display (size), name);
            if (nameWidth <= room + 0.5f)
            {
                // Laid out in a box a little wider than the text, so no glyph is ever dropped.
                g.setColour (p == main ? ink : soft);
                g.setFont (display (size));
                g.drawText (name, inner.withSizeKeepingCentre (nameWidth + 8.0f, inner.getHeight()).withX (cell.getCentreX() - nameWidth * 0.5f - 4.0f),
                            juce::Justification::centred, false);
            }
        }
    }

    // The boundary under the mouse (or being dragged): a resize handle, and where it is while dragging.
    const int boundary = draggedBoundary >= 0 ? draggedBoundary : hoveredBoundary;
    if (boundary >= 0 && boundary + 1 < (int) progression.size())
    {
        const double beat = progression[(size_t) boundary].start + progression[(size_t) boundary].length;
        const auto line = l.boundaryLine (beat);
        const float x = juce::jlimit (1.5f, (float) getWidth() - 1.5f, line.getStartX());
        const float y0 = line.getStartY() + (manyRows ? 6.0f : 10.0f), y1 = line.getEndY() - (manyRows ? 6.0f : 10.0f);
        g.setColour (accent);
        g.fillRoundedRectangle (x - 1.5f, y0, 3.0f, juce::jmax (4.0f, y1 - y0), 1.5f);
        if (draggedBoundary >= 0)
        {
            const auto label = barAndBeat (beat, processor.getBeatsPerBar());
            const auto font = bodyBold (12.0f);
            const float w = textWidth (font, label) + 20.0f, h = 24.0f;
            const float bx = juce::jlimit (0.0f, (float) getWidth() - w, x - w * 0.5f);
            const float by = juce::jlimit (0.0f, (float) getHeight() - h, line.getEndY() - h - (manyRows ? 4.0f : 14.0f));
            const juce::Rectangle<float> badge (bx, by, w, h);
            g.setColour (control);
            g.fillRoundedRectangle (badge, 8.0f);
            g.setColour (accent);
            g.drawRoundedRectangle (badge.reduced (0.5f), 8.0f, 1.0f);
            g.setColour (text);
            g.setFont (font);
            g.drawText (label, badge.toNearestInt(), juce::Justification::centred);
        }
    }
}

void ChordStrip::mouseMove (const juce::MouseEvent& e)
{
    const auto progression = processor.getProgression();
    const auto l = StripLayout::make (progression, processor.getBeatsPerBar(), (float) getWidth(), (float) getHeight());
    const int boundary = l.boundaryAt (progression, e.position, 5.0f);
    const int chord = boundary >= 0 ? -1 : chordAt (e.position);
    setMouseCursor (boundary >= 0 ? juce::MouseCursor::LeftRightResizeCursor
                                  : chord >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    if (boundary != hoveredBoundary || chord != hovered)
    {
        hoveredBoundary = boundary;
        hovered = chord;
        repaint();
    }
}

void ChordStrip::mouseExit (const juce::MouseEvent&)
{
    if (draggedBoundary < 0)
    {
        hovered = hoveredBoundary = -1;
        repaint();
    }
}

void ChordStrip::mouseDown (const juce::MouseEvent& e)
{
    const auto progression = processor.getProgression();
    const auto l = StripLayout::make (progression, processor.getBeatsPerBar(), (float) getWidth(), (float) getHeight());
    const int boundary = l.boundaryAt (progression, e.position, 5.0f);
    if (boundary >= 0)
    {
        showBoundary (boundary, true);
        return;
    }
    const int index = chordAt (e.position);
    if (index < 0)
        return;

    auto built = buildMenu (index);
    const auto pieces = l.pieces (progression[(size_t) index].start, progression[(size_t) index].length);
    auto target = pieces.empty() ? juce::Rectangle<float>() : pieces.front();
    for (const auto& p : pieces)
        if (p.getWidth() > target.getWidth())
            target = p;
    built.menu.setLookAndFeel (&getLookAndFeel());
    juce::Component::SafePointer<ChordStrip> safe (this);
    auto choices = built.choices;
    built.menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (localAreaToGlobal (target.toNearestInt()))
                                                        .withMinimumWidth (220),
                              [safe, index, choices] (int result)
                              {
                                  if (safe != nullptr && result > 0)
                                  {
                                      applyMenuResult (safe->processor, index, result, choices);
                                      safe->repaint();
                                  }
                              });
}

void ChordStrip::mouseDrag (const juce::MouseEvent& e)
{
    if (draggedBoundary < 0)
        return;
    // Live: the chords and the preview follow the mouse, snapped to the grid.
    processor.moveChordBoundary (draggedBoundary, layout().beatAt (e.position));
    repaint();
}

void ChordStrip::mouseUp (const juce::MouseEvent& e)
{
    if (draggedBoundary >= 0)
    {
        draggedBoundary = -1;
        hoveredBoundary = -1;
        mouseMove (e);
        repaint();
    }
}

ChordStrip::Menu ChordStrip::buildMenu (int index) const
{
    Menu m;
    const auto progression = processor.getProgression();
    if (index < 0 || index >= (int) progression.size())
        return m;
    const auto key = processor.getKey();
    const auto current = progression[(size_t) index].chord;

    auto item = [&] (Chord c, const juce::String& detail)
    {
        m.choices.push_back (c);
        juce::PopupMenu::Item it (juce::String (chordName (c, key)));
        it.itemID = (int) m.choices.size();
        it.shortcutKeyDescription = detail;
        it.isTicked = c == current;
        return it;
    };

    m.menu.addSectionHeader ("Chords that fit here, best first");
    const auto alternatives = processor.getAlternatives (index);
    for (size_t i = 0; i < alternatives.size() && i < 8; ++i)
        m.menu.addItem (item (alternatives[i], romanNumeral (alternatives[i], key)));

    // Same root: other chord types, and which note is in the bass.
    juce::PopupMenu types, inversions;
    const auto tones = chordPitchClasses (current);
    for (const auto& v : processor.getVariations (index))
    {
        const bool sameType = v.quality == current.quality;
        if (! sameType)
            types.addItem (item (v, romanNumeral (v, key)));
        else
        {
            const int bassNote = v.bassPitchClass();
            const juce::String where = bassNote == (((v.root % 12) + 12) % 12) ? "Root in the bass"
                                     : (tones.size() > 1 && bassNote == tones[1]) ? "Third in the bass"
                                     : (tones.size() > 2 && bassNote == tones[2]) ? "Fifth in the bass" : "";
            inversions.addItem (item (v, where));
        }
    }
    m.menu.addSeparator();
    m.menu.addSubMenu ("Chord type", types, types.getNumItems() > 0);
    m.menu.addSubMenu ("Bass note", inversions, inversions.getNumItems() > 0);

    // Edits. Check them on a copy so impossible ones are greyed out.
    const bool room = (int) progression.size() < BacklineProcessor::maxChords;
    const bool canSplit = room && backline::splitChord (progression, (size_t) index, processor.getGridBeats()) != progression;
    const auto withPassing = backline::insertPassingChord (progression, (size_t) index, key);
    const bool canPass = room && withPassing != progression && withPassing.size() == progression.size() + 1;
    m.menu.addSeparator();
    m.menu.addItem (splitId, "Split in two", canSplit);
    juce::PopupMenu::Item passing ("Add passing chord");
    passing.itemID = passingId;
    passing.isEnabled = canPass;
    if (canPass)
        passing.shortcutKeyDescription = juce::String (chordName (withPassing[(size_t) index + 1].chord, key));
    m.menu.addItem (passing);
    m.menu.addItem (deleteId, "Delete chord", progression.size() > 1);
    return m;
}

bool ChordStrip::applyMenuResult (BacklineProcessor& p, int index, int result, const std::vector<Chord>& choices)
{
    if (result >= 1 && result <= (int) choices.size())
        return p.setChord (index, choices[(size_t) result - 1]);
    switch (result)
    {
        case splitId:   return p.splitChord (index);
        case passingId: return p.insertPassingChord (index);
        case deleteId:  return p.deleteChord (index);
        default:        return false;
    }
}

//==============================================================================
PartTile::PartTile (BacklineProcessor& p, BacklineProcessor::Part whichPart, juce::String name, juce::Colour c, bool sound)
    : processor (p), part (whichPart), title (std::move (name)), colour (c), hasSound (sound)
{
    if (hasSound)
    {
        addAndMakeVisible (rhythm);
        addAndMakeVisible (instrument);
        instrument.setTooltip ("The sound that plays this part, here and in the WAV");
        instrument.onOpen = [this] { updateInstruments (true); };
        instrument.onChange = [this]
        {
            const int id = instrument.getSelectedId();
            if (id == 999) // "Open sounds folder"
            {
                auto folder = processor.getSoundsFolder();
                folder.createDirectory();
                folder.startAsProcess();
                updateInstruments (false);
            }
            else if (id >= 1 && id <= instrumentNames.size())
                processor.setInstrument (part, instrumentNames[id - 1]);
        };
        instrumentNames.add (instruments::builtInName);
        updateInstruments (false);
    }
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
}

void PartTile::resized()
{
    rhythm.setBounds (getWidth() - 152, 12, 138, 28);
    instrument.setBounds (getWidth() - 14 - 112, getHeight() - 12 - 26, 112, 26);
}

void PartTile::update()
{
    notes = processor.getPartNotes (part);
    if (hasSound)
        updateInstruments (false);
    repaint();
}

void PartTile::updateInstruments (bool rescan)
{
    if (! hasSound)
        return;
    if (rescan)
        instrumentNames = processor.getInstrumentNames();
    const auto wanted = processor.getInstrument (part);
    const int index = instrumentNames.indexOf (wanted);
    if (rescan || instrument.getNumItems() != instrumentNames.size() + 1)
    {
        instrument.clear (juce::dontSendNotification);
        for (int i = 0; i < instrumentNames.size(); ++i)
            instrument.addItem (instrumentNames[i], i + 1);
        instrument.addSeparator();
        instrument.addItem ("Open sounds folder", 999);
    }
    if (index >= 0)
        instrument.setSelectedId (index + 1, juce::dontSendNotification);
    else // chosen in a saved session but not found yet: show it, the synth plays until it appears
        instrument.setText (wanted.isNotEmpty() ? wanted : juce::String (instruments::builtInName), juce::dontSendNotification);
}

juce::Rectangle<int> PartTile::chipBounds (int chip) const
{
    if (notes.empty() || chip == none || (chip == wav && ! hasSound))
        return {};
    const int y = getHeight() - 12 - 26;
    return chip == midi ? juce::Rectangle<int> (14, y, 66, 26) : juce::Rectangle<int> (14 + 66 + 6, y, 62, 26);
}

int PartTile::chipAt (juce::Point<int> p) const
{
    for (int c : { (int) midi, (int) wav })
        if (chipBounds (c).contains (p))
            return c;
    return none;
}

void PartTile::paint (juce::Graphics& g)
{
    const bool ready = ! notes.empty();
    drawSurface (g, getLocalBounds().toFloat(), 14.0f, ready && hover ? accent : border);

    auto inner = getLocalBounds().reduced (14, 12);
    g.setColour (ready ? text : textDim);
    g.setFont (displayMedium (16.0f));
    g.drawText (title, inner.removeFromTop (28).withTrimmedRight (hasSound ? 146 : 0), juce::Justification::centredLeft, true);

    auto footer = inner.removeFromBottom (26);
    if (ready)
    {
        for (int c : { (int) midi, (int) wav })
        {
            const auto chip = chipBounds (c).toFloat();
            if (chip.isEmpty())
                continue;
            const bool hot = hoverChip == c;
            g.setColour (control);
            g.fillRoundedRectangle (chip.reduced (0.5f), 8.0f);
            g.setColour (hot ? accent : border);
            g.drawRoundedRectangle (chip.reduced (0.5f), 8.0f, 1.0f);
            g.setColour (hot ? text : textDim);
            drawGrip (g, chip.getX() + 10.0f, chip.getCentreY() - 6.0f);
            g.setColour (text);
            g.setFont (bodyBold (12.0f));
            g.drawText (c == midi ? "MIDI" : "WAV", chip.withTrimmedLeft (22.0f).withTrimmedRight (8.0f).toNearestInt(),
                        juce::Justification::centred, false);
        }
        if (! hasSound)
        {
            g.setFont (body (12.5f));
            g.setColour (hover ? text : textDim);
            g.drawText ("Drag into your DAW", footer.withTrimmedLeft (66 + 12), juce::Justification::centredLeft, true);
        }
    }
    else
    {
        g.setFont (body (12.5f));
        g.setColour (textDim);
        g.drawText (hasSound ? "Nothing yet" : "For single-note audio", footer.withTrimmedRight (hasSound ? 120 : 0),
                    juce::Justification::centredLeft, true);
    }

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

void PartTile::mouseMove (const juce::MouseEvent& e)
{
    const int chip = chipAt (e.getPosition());
    if (chip != hoverChip || ! hover)
    {
        hoverChip = chip;
        hover = true;
        setTooltip (chip == midi ? "Drag the MIDI into your DAW" : chip == wav ? "Drag the audio (WAV) into your DAW" : "");
        repaint();
    }
}

void PartTile::mouseDown (const juce::MouseEvent& e)
{
    pressedChip = chipAt (e.getPosition());
}

void PartTile::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || notes.empty() || e.getDistanceFromDragStart() < 6)
        return;
    // The WAV chip drags audio; anywhere else (the MIDI chip or the piano roll) drags MIDI.
    const auto file = pressedChip == wav ? processor.writeWavFile (part) : processor.writeMidiFile (part);
    if (file.existsAsFile())
    {
        dragging = true;
        exporting = true;
        auto flag = exportingFlag;
        juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this, [flag] { *flag = false; });
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
    for (auto* c : std::initializer_list<juce::Component*> { &recordButton, &waveform, &openButton, &playTakeButton, &playButton,
             &tempoField, &tonicBox, &modeBox, &changeBox, &seventhsButton, &chordStrip, &previewButton, &volume,
             &chordsTile, &bassTile, &melodyTile })
        addAndMakeVisible (c);

    recordButton.setTooltip ("Record audio from this track");
    recordButton.onClick = [this] { processor.setRecordArmed (! processor.isRecordArmed()); refresh(); };

    openButton.setTooltip ("Use a WAV, AIFF, FLAC or OGG file as the take (or drop one on the window)");
    openButton.onClick = [this] { openFileChooser(); };
    playTakeButton.setTooltip ("Play the take along with the chords");
    playTakeButton.onClick = [this] { processor.setTakePlayback (playTakeButton.getToggleState()); };

    playButton.setTooltip ("Play or stop");
    playButton.onClick = [this] { processor.setTransportPlaying (! processor.isTransportPlaying()); refresh(); };
    tempoField.onCommit = [this] (double bpm) { processor.setManualTempo (bpm); refresh(); };

    for (int i = 0; i < 12; ++i) tonicBox.addItem (pitchClassName (i), i + 1); // relabelled per mode in refresh()
    modeBox.addItemList ({ "major", "minor" }, 1);
    auto keyChanged = [this] { processor.setKey ({ tonicBox.getSelectedId() - 1, modeBox.getSelectedId() == 2, 1.0f }); };
    tonicBox.onChange = keyChanged;
    modeBox.onChange = keyChanged;

    changeBox.addItem ("Every half bar", 1); changeBox.addItem ("Every bar", 2); changeBox.addItem ("Every 2 bars", 3);
    changeBox.onChange = [this] { processor.setChordRate (changeBox.getSelectedId() == 1 ? 0.5 : changeBox.getSelectedId() == 3 ? 2.0 : 1.0); };
    changeBox.setTooltip ("How often the chords change in the ideas");
    seventhsButton.setTooltip ("Give each chord its seventh from the key");
    seventhsButton.onClick = [this] { processor.setSevenths (seventhsButton.getToggleState()); };

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

    previewButton.setTooltip ("Hear the chords and bass");
    previewButton.onClick = [this] { processor.previewEnabled = previewButton.getToggleState(); };
    volume.setSliderStyle (juce::Slider::LinearHorizontal);
    volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    volume.setRange (0.0, 1.0);
    volume.onValueChange = [this] { processor.previewGain = (float) volume.getValue(); };

    setSize (900, 612);
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
    g.drawText ("Backline", 68, 20, 150, 34, juce::Justification::centredLeft);

    g.setColour (textDim);
    g.setFont (body (13.0f));
    g.drawText ("Key", juce::Rectangle<int> (tonicBox.getX() - 44, tonicBox.getY(), 36, tonicBox.getHeight()), juce::Justification::centredRight);
    drawSurface (g, recordButton.getBounds().getUnion (waveform.getBounds()).expanded (16, 14).toFloat());
}

void BacklineEditor::paintOverChildren (juce::Graphics& g)
{
    if (dropName.isEmpty())
        return;
    // An audio file is being dragged over the window.
    g.setColour (bg.withAlpha (0.94f));
    g.fillAll();
    const auto zone = getLocalBounds().reduced (12).toFloat();
    juce::Path outline, dashed;
    outline.addRoundedRectangle (zone, 14.0f);
    const float dashes[2] = { 8.0f, 6.0f };
    juce::PathStrokeType (2.0f).createDashedStroke (dashed, outline, dashes, 2);
    g.setColour (accent);
    g.fillPath (dashed);
    const auto centre = getLocalBounds().reduced (60, 0);
    g.setColour (text);
    g.setFont (display (26.0f));
    g.drawText ("Drop to use this file as the take", centre.withSizeKeepingCentre (centre.getWidth(), 34).translated (0, -16),
                juce::Justification::centred, true);
    g.setColour (textDim);
    g.setFont (body (14.0f));
    g.drawText (dropName, centre.withSizeKeepingCentre (centre.getWidth(), 20).translated (0, 18), juce::Justification::centred, true);
}

void BacklineEditor::resized()
{
    auto area = getLocalBounds().reduced (24, 20);

    // Header: logo, then the own transport (when there is no host), preview and key.
    auto header = area.removeFromTop (34);
    modeBox.setBounds (header.removeFromRight (96));
    header.removeFromRight (6);
    tonicBox.setBounds (header.removeFromRight (66));
    header.removeFromRight (68);
    volume.setBounds (header.removeFromRight (96).reduced (0, 4));
    header.removeFromRight (6);
    previewButton.setBounds (header.removeFromRight (112));
    header.removeFromRight (20);
    tempoField.setBounds (header.removeFromRight (100));
    header.removeFromRight (6);
    playButton.setBounds (header.removeFromRight (34));
    area.removeFromTop (26);

    // The take: record, its waveform, and in the top row of it, file and playback controls.
    auto capture = area.removeFromTop (96).reduced (16, 0);
    recordButton.setBounds (capture.removeFromLeft (76).withSizeKeepingCentre (76, 76));
    capture.removeFromLeft (12);
    waveform.setBounds (capture);
    auto takeRow = capture.removeFromTop (WaveformView::topRow);
    openButton.setBounds (takeRow.removeFromRight (128));
    takeRow.removeFromRight (16);
    playTakeButton.setBounds (takeRow.removeFromRight (104));
    waveform.topRowInset = 128 + 16 + 104 + 12;
    area.removeFromTop (28);

    auto row = area.removeFromTop (30);
    for (auto* b : ideaButtons)
    {
        b->setBounds (row.removeFromLeft (84));
        row.removeFromLeft (6);
    }
    changeBox.setBounds (row.removeFromRight (150));
    row.removeFromRight (20);
    seventhsButton.setBounds (row.removeFromRight (104));
    area.removeFromTop (12);

    chordStrip.setBounds (area.removeFromTop (164));
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
    if (processor.getRevision() != lastRevision || armed != lastArmed || recording != lastRecording || (recording && tick % 6 == 0)
        || processor.isLoadingFile() != lastLoading || processor.isAnalysing() != lastAnalysing
        || processor.hasHostTransport() != lastHost || (shownError.isNotEmpty() && tick % 30 == 0))
        refresh();

    if (armed || recording)
    {
        recordButton.pulse = 0.5f + 0.5f * std::sin ((float) tick * 0.21f);
        recordButton.repaint();
    }
    if (recording && tick % 3 == 0)
        waveform.update (true);

    // Follow the transport.
    const double head = processor.getPlayheadBeats();
    const int active = head >= 0.0 ? processor.getChordIndexAt (head) : -1;
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
    const bool loading = lastLoading = processor.isLoadingFile();
    const bool analysing = lastAnalysing = processor.isAnalysing();
    const bool host = lastHost = processor.hasHostTransport();
    recordButton.armed = armed;
    recordButton.recording = recording;
    recordButton.repaint();

    // Own transport, only without a host.
    playButton.setVisible (! host);
    tempoField.setVisible (! host);
    playButton.playing = processor.isTransportPlaying();
    playButton.repaint();
    tempoField.setTempo (processor.getManualTempo());
    const double estimate = processor.getEstimatedTempo();
    tempoField.setTooltip (estimate > 0.0 ? "Detected " + TempoField::format (estimate) + " BPM. Drag or click to change it."
                                          : "Tempo. Drag up or down, or click to type.");

    // A load error stays up for a few seconds.
    const auto error = processor.getLoadError();
    if (error != shownError)
    {
        shownError = error;
        errorShownAt = juce::Time::getMillisecondCounter();
    }
    const bool showError = error.isNotEmpty() && juce::Time::getMillisecondCounter() - errorShownAt < 8000;

    waveform.title.clear();
    waveform.facts.clear();
    waveform.messageIsError = false;
    if (recording)
        waveform.message = "Recording  " + juce::String (processor.getRecordedSeconds(), 1) + " s";
    else if (armed)
        waveform.message = "Waiting for playback to start";
    else if (loading)
        waveform.message = "Opening " + (pendingFile.isNotEmpty() ? pendingFile : juce::String ("the file")) + ellipsis;
    else if (showError)
    {
        waveform.message = error;
        waveform.messageIsError = true;
    }
    else if (processor.wasLastTakeTooShort())
        waveform.message = "That take was too short. Record at least one bar.";
    else if (processor.hasCapture())
    {
        const int bars = juce::jmax (1, juce::roundToInt (processor.getTakeBeats() / processor.getBeatsPerBar()));
        waveform.title = processor.getTakeName().isNotEmpty() ? processor.getTakeName() : juce::String ("Recorded take");
        waveform.facts = { juce::String (bars) + (bars == 1 ? " bar" : " bars"),
                           analysing ? "Analysing" + ellipsis : juce::String (juce::roundToInt (processor.getBpm())) + " BPM",
                           keyName (processor.getKey()) };
    }
    else if (processor.hasProgression())
        waveform.message = "Progression restored. Record or open a file for new ideas.";
    else
        waveform.message = host ? "Put Backline on the track you want to build around."
                                : "Record from your input, or open an audio file.";

    if (loading)
    {
        chordStrip.emptyTitle = "Listening to " + (pendingFile.isNotEmpty() ? pendingFile : juce::String ("the file"));
        chordStrip.emptyHint = host ? "Finding the key and the chords that fit." : "Finding the tempo, the key and the chords that fit.";
    }
    else
    {
        chordStrip.emptyTitle = host ? "Record a few bars to get chord ideas" : "Open an audio file to get chord ideas";
        chordStrip.emptyHint = host ? "Press record while your track plays, or drop an audio file anywhere here."
                                    : "Drop a file anywhere on this window, or press record and play into your input.";
    }

    playTakeButton.setEnabled (processor.hasTake());
    playTakeButton.setToggleState (processor.isTakePlaybackOn(), juce::dontSendNotification);
    seventhsButton.setToggleState (processor.getSevenths(), juce::dontSendNotification);

    const auto key = processor.getKey();
    tonicBox.setSelectedId (key.tonic + 1, juce::dontSendNotification);
    modeBox.setSelectedId (key.minor ? 2 : 1, juce::dontSendNotification);
    const double rate = processor.getChordRate();
    changeBox.setSelectedId (rate < 0.75 ? 1 : rate > 1.5 ? 3 : 2, juce::dontSendNotification);
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

//==============================================================================
bool BacklineEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    // Not our own exports being dragged out over the window.
    if (chordsTile.isDraggingOut() || bassTile.isDraggingOut() || melodyTile.isDraggingOut())
        return false;
    for (const auto& f : files)
        if (BacklineProcessor::isAudioFile (juce::File (f)))
            return true;
    return false;
}

void BacklineEditor::fileDragEnter (const juce::StringArray& files, int, int)
{
    for (const auto& f : files)
        if (BacklineProcessor::isAudioFile (juce::File (f)))
        {
            dropName = juce::File (f).getFileName();
            break;
        }
    repaint();
}

void BacklineEditor::fileDragExit (const juce::StringArray&)
{
    dropName.clear();
    repaint();
}

void BacklineEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dropName.clear();
    repaint();
    for (const auto& f : files)
        if (BacklineProcessor::isAudioFile (juce::File (f)))
        {
            loadFile (juce::File (f));
            return;
        }
}

void BacklineEditor::loadFile (const juce::File& file)
{
    pendingFile = file.getFileName();
    processor.loadAudioFile (file);
    refresh();
}

void BacklineEditor::openFileChooser()
{
    chooser = std::make_unique<juce::FileChooser> ("Open an audio file", juce::File(), BacklineProcessor::getAudioFileWildcard());
    juce::Component::SafePointer<BacklineEditor> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe] (const juce::FileChooser& fc)
                          {
                              if (safe != nullptr && fc.getResult() != juce::File())
                                  safe->loadFile (fc.getResult());
                          });
}
