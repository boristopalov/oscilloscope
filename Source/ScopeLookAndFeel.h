#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace scopeStyle
{
inline const juce::Colour background { 0xff181c1a }, surface { 0xff242925 }, ink { 0xffe5e6dd },
    muted { 0xffa3aca2 }, rule { 0xff3c453e }, accent { 0xff95bda4 }, warning { 0xfff08c70 },
    plot { 0xff121514 }, beatRule { 0xff202624 }, barRule { 0xff1b1d20 },
    zeroRule { 0xff1b1d20 }, fullScaleRule { 0xff1b1d20 };
}

class ScopeLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    ScopeLookAndFeel()
    {
        using namespace scopeStyle;
        setColour(juce::Label::textColourId, ink);
        setColour(juce::TextButton::buttonColourId, surface);
        setColour(juce::TextButton::textColourOffId, ink);
        setColour(juce::TextButton::textColourOnId, ink);
        setColour(juce::ComboBox::backgroundColourId, surface);
        setColour(juce::ComboBox::textColourId, ink);
        setColour(juce::ComboBox::outlineColourId, rule);
        setColour(juce::ComboBox::arrowColourId, muted);
        setColour(juce::TextEditor::backgroundColourId, surface);
        setColour(juce::TextEditor::textColourId, ink);
        setColour(juce::TextEditor::outlineColourId, rule);
        setColour(juce::TextEditor::focusedOutlineColourId, accent);
        setColour(juce::TextEditor::highlightColourId, accent.withAlpha(0.2f));
        setColour(juce::TextEditor::highlightedTextColourId, ink);
        setColour(juce::CaretComponent::caretColourId, ink);
        setColour(juce::PopupMenu::backgroundColourId, surface);
        setColour(juce::PopupMenu::textColourId, ink);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, surface.brighter(0.12f));
        setColour(juce::PopupMenu::highlightedTextColourId, ink);
        setColour(juce::ScrollBar::thumbColourId, muted.withAlpha(0.4f));
        setColour(juce::Slider::backgroundColourId, rule);
        setColour(juce::Slider::trackColourId, accent.withAlpha(0.65f));
        setColour(juce::Slider::thumbColourId, accent);
        setColour(juce::Slider::rotarySliderFillColourId, accent);
        setColour(juce::Slider::rotarySliderOutlineColourId, rule);
        setColour(juce::Slider::textBoxBackgroundColourId, surface);
        setColour(juce::Slider::textBoxTextColourId, ink);
        setColour(juce::Slider::textBoxOutlineColourId, rule);
        setColour(juce::Slider::textBoxHighlightColourId, accent.withAlpha(0.2f));
    }
    juce::Font getTextButtonFont(juce::TextButton&, int) override { return juce::FontOptions(13.0f); }
    juce::Font getComboBoxFont(juce::ComboBox&) override { return juce::FontOptions(12.0f); }
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float position,
                          float startAngle, float endAngle, juce::Slider& slider) override
    {
        const auto area = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
            static_cast<float>(width), static_cast<float>(height)).reduced(4);
        const auto radius = std::min(area.getWidth(), area.getHeight()) * 0.5f;
        const auto centre = area.getCentre();
        const auto angle = startAngle + position * (endAngle - startAngle);
        const auto opacity = slider.isEnabled() ? 1.0f : 0.4f;
        juce::Path track, value;
        track.addCentredArc(centre.x, centre.y, radius, radius, 0, startAngle, endAngle, true);
        value.addCentredArc(centre.x, centre.y, radius, radius, 0, startAngle, angle, true);
        const juce::PathStrokeType stroke(2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        g.setColour(slider.findColour(juce::Slider::rotarySliderOutlineColourId).withMultipliedAlpha(opacity));
        g.strokePath(track, stroke);
        g.setColour(slider.findColour(juce::Slider::rotarySliderFillColourId).withMultipliedAlpha(opacity));
        g.strokePath(value, stroke);
        const auto bodyRadius = std::max(1.0f, radius - 4);
        const juce::Rectangle<float> body(centre.x - bodyRadius, centre.y - bodyRadius, bodyRadius * 2, bodyRadius * 2);
        g.setColour(scopeStyle::surface.brighter(0.04f).withMultipliedAlpha(opacity));
        g.fillEllipse(body);
        g.setColour(scopeStyle::rule.withMultipliedAlpha(opacity));
        g.drawEllipse(body, 1);
        juce::Path pointer;
        pointer.startNewSubPath(0, -bodyRadius * 0.4f);
        pointer.lineTo(0, -bodyRadius * 0.82f);
        g.setColour(scopeStyle::ink.withMultipliedAlpha(opacity));
        g.strokePath(pointer, juce::PathStrokeType(2, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
            juce::AffineTransform::rotation(angle).translated(centre.x, centre.y));
    }
    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& colour,
                              bool hover, bool down) override
    {
        auto area = button.getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(colour.withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.4f)
            .brighter(down ? 0.12f : hover ? 0.04f : 0.0f));
        g.fillRoundedRectangle(area, 3.0f);
        g.setColour(button.hasKeyboardFocus(true) ? scopeStyle::accent : scopeStyle::rule);
        g.drawRoundedRectangle(area, 3.0f, 1.0f);
    }
    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool hover, bool) override
    {
        const auto box = juce::Rectangle<float>(3, (static_cast<float>(button.getHeight()) - 12) / 2, 12, 12);
        g.setColour(button.getToggleState() ? scopeStyle::accent : scopeStyle::surface);
        g.fillRoundedRectangle(box, 2);
        g.setColour(hover ? scopeStyle::ink : scopeStyle::muted);
        g.drawRoundedRectangle(box, 2, 1);
        if (button.getToggleState())
        {
            juce::Path tick; tick.startNewSubPath(box.getX() + 3, box.getY() + 6);
            tick.lineTo(box.getX() + 5, box.getY() + 8); tick.lineTo(box.getX() + 9, box.getY() + 3);
            g.setColour(scopeStyle::surface.withAlpha(button.isEnabled() ? 1.0f : 0.5f));
            g.strokePath(tick, juce::PathStrokeType(1.4f));
        }
        g.setColour(scopeStyle::muted); g.setFont(juce::FontOptions(12.0f));
        g.drawText(button.getButtonText(), button.getLocalBounds().withTrimmedLeft(22), juce::Justification::centredLeft);
    }
    void drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto area = juce::Rectangle<float>(0, 0, static_cast<float>(width), static_cast<float>(height)).reduced(0.5f);
        g.setColour(scopeStyle::surface.withAlpha(box.isEnabled() ? 1.0f : 0.5f)); g.fillRoundedRectangle(area, 3);
        g.setColour(box.hasKeyboardFocus(true) ? scopeStyle::accent : scopeStyle::rule); g.drawRoundedRectangle(area, 3, 1);
        const auto x = static_cast<float>(width) - 13, y = static_cast<float>(height) / 2;
        juce::Path arrow; arrow.startNewSubPath(x - 3, y - 2); arrow.lineTo(x, y + 1); arrow.lineTo(x + 3, y - 2);
        g.setColour(scopeStyle::muted); g.strokePath(arrow, juce::PathStrokeType(1.2f));
    }
};
