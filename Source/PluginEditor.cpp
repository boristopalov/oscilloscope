#include "PluginEditor.h"

using namespace oscilloscope;
ScopeEditor::ScopeEditor(ScopeProcessor& scope)
    : AudioProcessorEditor(scope), owner(scope), view(scope.displayService())
{
    setLookAndFeel(&lookAndFeel);
    trackCanvas.onPaint = [this](juce::Graphics& g) { paintWaveforms(g); };
    trackViewport.setName("Tracks");
    trackViewport.setViewedComponent(&trackCanvas, false);
    trackViewport.setScrollBarsShown(true, false);
    trackViewport.setScrollBarThickness(8);
    addAndMakeVisible(trackViewport);
    heading.setText("Oscilloscope", juce::dontSendNotification);
    heading.setFont(juce::FontOptions(20.0f));
    for (auto* label : { &status, &headroomLabel })
    {
        label->setFont(juce::FontOptions(12.0f));
        label->setColour(juce::Label::textColourId, scopeStyle::muted);
    }
    resetButton.setTooltip("Clear capture in all live viewers. Frozen views are preserved.");
    freezeButton.setTooltip("Hold a snapshot in this viewer. Other viewers stay live.");
    status.setColour(juce::Label::textColourId, scopeStyle::warning);
    for (auto* component : std::initializer_list<juce::Component*> {
             &heading, &status, &resetButton, &referenceSelector,
             &windowSelector, &layoutButton, &channelSelector, &headroomLabel, &headroomSlider, &focusSelector, &freezeButton })
        { component->setBufferedToImage(true); addAndMakeVisible(component); }
    resetButton.onClick = [this] { owner.displayService().reset(); };
    freezeButton.onClick = [this]
    {
        if (view.frozen()) view.unfreeze(); else view.freeze();
        resizeToFitTracks(); resized(); refreshUi(true);
    };
    referenceSelector.setTooltip("Shared timing reference only; no audio routing. Changing it requires Reset.");
    referenceSelector.setTextWhenNothingSelected("Select shared reference");
    referenceSelector.onChange = [this]
    {
        const auto index = referenceSelector.getSelectedId() - 1;
        if (index >= 0 && static_cast<std::size_t>(index) < referenceChoices.size())
            owner.displayService().selectReference(referenceChoices[static_cast<std::size_t>(index)]);
    };
    windowSelector.addItem("1/4 beat", 1); windowSelector.addItem("1 beat", 2);
    windowSelector.addItem("1 bar", 3); windowSelector.addItem("2 bars", 4); windowSelector.addItem("4 bars", 5);
    windowSelector.setSelectedId(3, juce::dontSendNotification);
    windowSelector.onChange = [this]
    {
        view.setWindow(static_cast<BeatWindow>(windowSelector.getSelectedId() - 1)); refreshUi(true);
    };
    layoutButton.setName("Layout");
    layoutButton.setClickingTogglesState(true);
    layoutButton.setTooltip("Toggle between separate stacked plots and a shared waveform overlay.");
    layoutButton.onClick = [this]
    {
        view.setLayout(layoutButton.getToggleState() ? ViewLayout::overlay : ViewLayout::stacked);
        layoutButton.setButtonText(layoutButton.getToggleState() ? "Overlay" : "Stacked");
        resizeToFitTracks(); resized(); repaint();
    };
    channelSelector.setName("Channels");
    channelSelector.setTooltip("Channel projection for all tracks in this viewer, including frozen audio.");
    channelSelector.addItem("Left", 1); channelSelector.addItem("Right", 2);
    channelSelector.addItem("Stereo L/R", 3); channelSelector.addItem("Mono (Mid)", 4);
    channelSelector.addItem("Side", 5); channelSelector.addItem("Mid/Side", 6);
    channelSelector.setSelectedId(3, juce::dontSendNotification);
    channelSelector.onChange = [this]
    {
        view.setChannelMode(static_cast<ChannelMode>(channelSelector.getSelectedId() - 1));
        trackCanvas.repaint(); repaint();
    };
    headroomLabel.setText("Headroom", juce::dontSendNotification);
    headroomSlider.setName("Headroom");
    headroomSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    headroomSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 62, 24);
    headroomSlider.setRange(0, 24, 0.1);
    headroomSlider.setTextValueSuffix(" dB");
    headroomSlider.setNumDecimalPlacesToDisplay(1);
    headroomSlider.setDoubleClickReturnValue(true, 6);
    headroomSlider.setValue(view.selectedHeadroomDb(), juce::dontSendNotification);
    headroomSlider.setTooltip("Display headroom above 0 dBFS, from 0 to 24 dB. Double-click to return to 6 dB.");
    headroomSlider.onValueChange = [this] { view.setHeadroomDb(headroomSlider.getValue()); trackCanvas.repaint(); };
    focusSelector.setName("Overlay focus");
    focusSelector.setTooltip("Highlight one source in overlays; keep the others visible for comparison.");
    focusSelector.onChange = [this]
    {
        const int index = focusSelector.getSelectedId() - 2;
        focusedSource = index >= 0 && static_cast<std::size_t>(index) < referenceChoices.size()
            ? referenceChoices[static_cast<std::size_t>(index)] : 0;
        trackCanvas.repaint();
    };
    setResizable(true, true); setResizeLimits(760, 260, 1800, 1400);
    owner.displayService().openViewer(owner.captureSource()->id);
    owner.displayService().addListener(this);
    refreshUi(true);
    vblank = std::make_unique<juce::VBlankAttachment>(this, std::function<void(double)>([this](double)
    {
        if (! pacer && isShowing()) pacer = std::make_unique<FramePacer>([this](double target) { present(target); });
        if (pacer && ! pacer->running()) present(juce::Time::getMillisecondCounterHiRes() * 0.001);
    }));
}
ScopeEditor::~ScopeEditor()
{
    vblank.reset(); pacer.reset(); waveformSurface.reset();
    setLookAndFeel(nullptr);
    trackViewport.setViewedComponent(nullptr, false);
    owner.displayService().removeListener(this);
    owner.displayService().closeViewer();
}
void ScopeEditor::paint(juce::Graphics& graphics)
{
    using namespace scopeStyle;
    graphics.fillAll(background);
    graphics.setColour(ink);
    graphics.fillRect(14, 17, 2, 18);
}
std::vector<std::uint64_t> ScopeEditor::trackIds() const
{
    std::vector<std::uint64_t> ids;
    for (const auto& row : rows) ids.push_back(row->id);
    // A removed tap's frozen audio still has a dedicated presentation row.
    for (const auto& lane : view.lanes())
        if (std::find(ids.begin(), ids.end(), lane.id) == ids.end()) ids.push_back(lane.id);
    return ids;
}
juce::Rectangle<int> ScopeEditor::trackBounds(std::uint64_t id) const
{
    const auto found = std::find(trackOrder.begin(), trackOrder.end(), id);
    return { 0, static_cast<int>(found - trackOrder.begin()) * trackHeight, trackCanvas.getWidth(), trackHeight };
}
void ScopeEditor::resized()
{
    if (! adjustingSize && automaticSize != juce::Point<int>()
        && automaticSize != juce::Point<int>(getWidth(), getHeight())) automaticallySized = false;
    auto bounds = getLocalBounds().reduced(14, 12);
    auto title = bounds.removeFromTop(28);
    heading.setBounds(title.removeFromLeft(166).withTrimmedLeft(8));
    resetButton.setBounds(title.removeFromRight(104)); title.removeFromRight(6);
    freezeButton.setBounds(title.removeFromRight(72)); title.removeFromRight(10);
    focusSelector.setVisible(view.selectedLayout() == ViewLayout::overlay);
    if (focusSelector.isVisible())
    { focusSelector.setBounds(title.removeFromLeft(188)); title.removeFromLeft(8); }
    status.setBounds(title);
    bounds.removeFromTop(6);
    auto controls = bounds.removeFromTop(48);
    referenceSelector.setBounds(controls.removeFromLeft(160).withSizeKeepingCentre(160, 28)); controls.removeFromLeft(6);
    windowSelector.setBounds(controls.removeFromLeft(76).withSizeKeepingCentre(76, 28)); controls.removeFromLeft(6);
    layoutButton.setBounds(controls.removeFromLeft(72).withSizeKeepingCentre(72, 28)); controls.removeFromLeft(6);
    channelSelector.setBounds(controls.removeFromLeft(106).withSizeKeepingCentre(106, 28)); controls.removeFromLeft(8);
    headroomSlider.setBounds(controls.removeFromRight(128));
    headroomLabel.setBounds(controls.removeFromRight(66));
    bounds.removeFromTop(8);
    trackViewport.setBounds(bounds);
    trackOrder = trackIds();
    const auto count = std::max(1, static_cast<int>(trackOrder.size()));
    trackHeight = std::max(80, bounds.getHeight() / count);
    const auto canvasHeight = std::max(bounds.getHeight(), count * trackHeight);
    trackCanvas.setSize(bounds.getWidth() - (canvasHeight > bounds.getHeight() ? 8 : 0), canvasHeight);
    waveformBounds = trackCanvas.getLocalBounds().withTrimmedLeft(trackRailWidth + 12).withTrimmedRight(8);
    for (auto& row : rows)
    {
        row->bounds = trackBounds(row->id);
        auto rail = row->bounds.withWidth(trackRailWidth).reduced(10, 5);
        auto identity = rail.removeFromTop(22);
        row->visible.setBounds(identity.removeFromLeft(22));
        row->colour.setBounds(identity.removeFromLeft(20).reduced(2, 3)); identity.removeFromLeft(6);
        row->name.setBounds(identity);
        auto settings = rail.removeFromTop(48);
        row->clipLabel.setBounds(settings.removeFromLeft(60));
        row->clip.setBounds(settings);
    }
    trackCanvas.repaint();
}
void ScopeEditor::resizeToFitTracks()
{
    if (! automaticallySized) return;
    const auto count = std::max(1, static_cast<int>(trackIds().size()));
    const auto rowHeight = view.selectedLayout() == ViewLayout::overlay ? 80 : 96;
    automaticSize = { 900, std::clamp(114 + count * rowHeight, 260, 800) };
    const juce::ScopedValueSetter<bool> guard(adjustingSize, true);
    setSize(automaticSize.x, automaticSize.y);
}
void ScopeEditor::rebuildSources()
{
    const auto sources = SourceRegistry::instance().sources();
    bool changed = rows.size() != sources.size();
    for (std::size_t n = 0; ! changed && n < rows.size(); ++n) changed = rows[n]->id != sources[n]->id;
    if (changed)
    {
        rows.clear();
        for (const auto& source : sources)
        {
            auto row = std::make_unique<SourceRow>(); row->id = source->id;
            row->visible.setName("Visible " + juce::String(source->id));
            row->visible.setTooltip("Show this source in this viewer");
            row->name.setName("Track " + juce::String(source->id));
            row->name.setFont(juce::FontOptions(14.0f, juce::Font::bold));
            row->name.setBorderSize(juce::BorderSize<int>(0));
            row->name.setEditable(false, true);
            row->clipLabel.setFont(juce::FontOptions(11.0f));
            row->clipLabel.setColour(juce::Label::textColourId, scopeStyle::muted);
            row->name.setTooltip("Double-click to rename this tap across viewers");
            row->name.onTextChange = [this, control = row.get()]
            {
                for (const auto& shared : SourceRegistry::instance().sources())
                    if (shared->id == control->id) shared->nameOverride = control->name.getText().trim().substring(0, 64);
                refreshUi(true);
            };
            row->colour.setName("Color " + juce::String(source->id));
            row->colour.setTooltip("Cycle source color across viewers");
            row->colour.onClick = [this, control = row.get()]
            {
                for (const auto& shared : SourceRegistry::instance().sources())
                    if (shared->id == control->id)
                    {
                        const auto found = std::find(std::begin(SourceState::palette), std::end(SourceState::palette), shared->colour);
                        const auto index = found == std::end(SourceState::palette) ? 0 : (found - std::begin(SourceState::palette) + 1) % 8;
                        shared->colour = SourceState::palette[index];
                        shared->colourOverridden = true;
                    }
                refreshUi(true);
            };
            row->visible.setToggleState(view.visible(source->id), juce::dontSendNotification);
            row->visible.onClick = [this, control = row.get()]
            { view.setVisible(control->id, control->visible.getToggleState()); refreshUi(true); };
            row->clipLabel.setText("Clip dBFS", juce::dontSendNotification);
            row->clip.setName("Clip " + juce::String(source->id));
            row->clip.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            row->clip.setTextBoxStyle(juce::Slider::TextBoxRight, false, 68, 24);
            row->clip.setRange(-60, 24, 0.1);
            row->clip.setSkewFactorFromMidPoint(0);
            row->clip.setNumDecimalPlacesToDisplay(1);
            row->clip.setTextValueSuffix(" dB");
            row->clip.setDoubleClickReturnValue(true, 0);
            row->clip.setValue(source->clipThresholdDb, juce::dontSendNotification);
            row->clip.setTooltip("Warning threshold, -60 to +24 dBFS. Double-click to return to 0 dBFS. Drawing only.");
            row->clip.onValueChange = [this, control = row.get()]
            {
                const auto db = std::round(control->clip.getValue() * 10) / 10;
                owner.displayService().setClipThreshold(control->id, db);
            };
            for (auto* component : std::initializer_list<juce::Component*> { &row->visible, &row->name, &row->colour, &row->clip, &row->clipLabel })
                { component->setBufferedToImage(true); trackCanvas.addAndMakeVisible(component); }
            rows.push_back(std::move(row));
        }
        resizeToFitTracks(); resized();
    }
    std::vector<std::uint64_t> choices;
    for (const auto& source : sources) if (source->admitted.load()) choices.push_back(source->id);
    if (choices != referenceChoices)
    {
        referenceChoices = std::move(choices); referenceSelector.clear(juce::dontSendNotification);
        focusSelector.clear(juce::dontSendNotification); focusSelector.addItem("Focus: all tracks", 1);
        int focus = 1;
        for (std::size_t n = 0; n < referenceChoices.size(); ++n)
            for (const auto& shared : sources) if (shared->id == referenceChoices[n])
            {
                focusSelector.addItem("Focus: " + shared->displayName(), static_cast<int>(n + 2));
                if (focusedSource == shared->id) focus = static_cast<int>(n + 2);
            }
        if (focus == 1) focusedSource = 0;
        focusSelector.setSelectedId(focus, juce::dontSendNotification);
        for (std::size_t n = 0; n < referenceChoices.size(); ++n)
            for (const auto& shared : sources) if (shared->id == referenceChoices[n])
                referenceSelector.addItem("Reference: " + shared->displayName(), static_cast<int>(n + 1));
    }
    for (std::size_t n = 0; n < referenceChoices.size(); ++n)
        for (const auto& shared : sources) if (shared->id == referenceChoices[n])
        {
            referenceSelector.changeItemText(static_cast<int>(n + 1), "Reference: " + shared->displayName());
            focusSelector.changeItemText(static_cast<int>(n + 2), "Focus: " + shared->displayName());
        }
    int selected = 0;
    for (std::size_t n = 0; n < referenceChoices.size(); ++n)
        if (referenceChoices[n] == owner.displayService().referenceId()) selected = static_cast<int>(n + 1);
    referenceSelector.setSelectedId(selected, juce::dontSendNotification);
    for (std::size_t n = 0; n < rows.size(); ++n)
    {
        const auto& source = *sources[n]; auto& row = *rows[n];
        const bool admitted = source.admitted.load();
        if (! row.name.isBeingEdited()) row.name.setText(source.displayName(), juce::dontSendNotification);
        row.name.setColour(juce::Label::textColourId, scopeStyle::ink);
        row.colour.setColour(juce::TextButton::buttonColourId, juce::Colour(source.colour));
        row.visible.setEnabled(admitted && ! view.frozen());
        row.name.setTooltip("Double-click to rename this tap across viewers\n"
            + (admitted ? juce::String(captureStatusText(source.summary.status)) : "Eight-track limit reached; audio passes through"));
        row.clip.setEnabled(admitted);
        row.clip.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(source.colour));
        row.clip.setValue(source.clipThresholdDb, juce::dontSendNotification);
    }
}
void ScopeEditor::displayUpdated() { refreshUi(false); }
void ScopeEditor::refreshUi(bool force)
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (! force && gpuActive && now - lastUiRefreshMs < 100) return;
    lastUiRefreshMs = now;
    auto& service = owner.displayService();
    rebuildSources();
    auto notice = ! view.frozen() && service.held() ? service.sessionMessage() : juce::String();
    if (! view.frozen() && view.freezeMessage().isNotEmpty()) notice = view.freezeMessage();
    status.setText(notice, juce::dontSendNotification);
    status.setTooltip(notice);
    status.setVisible(notice.isNotEmpty());
    freezeButton.setButtonText(view.frozen() ? "Unfreeze" : "Freeze");
    windowSelector.setEnabled(! view.frozen());
    trackCanvas.repaint();
    repaint();
}
void ScopeEditor::present(double timestamp)
{
    if (! isShowing() || getPeer() == nullptr || waveformBounds.isEmpty()) return;
    const auto preparationStart = juce::Time::getMillisecondCounterHiRes();
    presentationTime = timestamp;
    if (! waveformSurface)
    {
        waveformSurface = std::make_unique<WaveformSurface>();
        addAndMakeVisible(*waveformSurface);
    }
    // Update bounds even during recovery so a resized view can return to GPU.
    const auto visiblePlot = getLocalArea(&trackCanvas, waveformBounds).getIntersection(trackViewport.getBounds());
    waveformSurface->setBounds(visiblePlot);
    if (! waveformSurface->prepare())
    {
        gpuActive = false; waveformSurface->setVisible(false); trackCanvas.repaint(); return;
    }
    // NSViewComponent is attached directly to the native peer, so clip its
    // actual bounds to the viewport rather than relying on JUCE child clipping.
    waveformSurface->setVisible(! visiblePlot.isEmpty());
    const auto canvasOrigin = getLocalPoint(&trackCanvas, juce::Point<int>{});
    if (pacer) pacer->setDisplay(waveformSurface->displayId());
    const auto scale = waveformSurface->pixelScale();
    preparePlots(true, scale, timestamp);
    std::vector<WaveformDraw> draws;
    for (const auto& plot : plots)
        for (std::size_t i = 0; i < plot.indices.size(); ++i)
        {
            const auto& lane = frameLanes[plot.indices[i]];
            const auto opacity = plot.overlay && focusedSource != 0 && lane.id != focusedSource ? 0.18f : 1.0f;
            draws.push_back({ plot.frames[i], plot.area.translated(static_cast<float>(canvasOrigin.x-visiblePlot.getX()), static_cast<float>(canvasOrigin.y-visiblePlot.getY())),
                juce::Colour(lane.colour).withAlpha(opacity), plot.range, lane.clipAmplitude(), plot.overlay });
        }
    waveformSurface->onPresented = onPresented;
    const auto submitted = waveformSurface->render(draws, timestamp, frameWindow.end, juce::Time::getMillisecondCounterHiRes() - preparationStart);
    if (! waveformSurface->available())
    { gpuActive = false; waveformSurface->setVisible(false); trackCanvas.repaint(); return; }
    if (submitted && ! gpuActive)
    { gpuActive = true; trackCanvas.repaint(); }
}
void ScopeEditor::preparePlots(bool aligned, float pixelScale, double timestamp)
{
    frameWindow = view.window(timestamp); frameLanes = view.lanes(); plots.clear();
    const auto& window = frameWindow; const auto& lanes = frameLanes;
    if (lanes.empty() || waveformBounds.isEmpty()) return;
    std::vector<std::vector<std::size_t>> groups;
    // Only compatible synchronized lanes share an overlay. Independent clocks
    // keep separate plots beside the same compact track rail.
    if (view.selectedLayout() == ViewLayout::overlay && window.synced)
    {
        std::vector<std::size_t> synced;
        for (std::size_t n = 0; n < lanes.size(); ++n) if (lanes[n].synced) synced.push_back(n);
        if (! synced.empty()) groups.push_back(std::move(synced));
        for (std::size_t n = 0; n < lanes.size(); ++n) if (! lanes[n].synced) groups.push_back({ n });
    }
    else for (std::size_t n = 0; n < lanes.size(); ++n) groups.push_back({ n });
    const bool overlayLayout = view.selectedLayout() == ViewLayout::overlay;
    // Keep the shared plot in view while the track controls scroll beside it.
    const auto plotBounds = overlayLayout
        ? waveformBounds.withY(trackViewport.getViewPositionY()).withHeight(trackViewport.getViewHeight()) : waveformBounds;
    const float height = static_cast<float>(plotBounds.getHeight()) / static_cast<float>(groups.size());
    for (std::size_t group = 0; group < groups.size(); ++group)
    {
        auto indices = groups[group];
        if (overlayLayout && focusedSource != 0)
            std::stable_sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b)
                { return (lanes[a].id == focusedSource) < (lanes[b].id == focusedSource); });
        const bool overlay = indices.size() > 1;
        auto bounds = overlayLayout
            ? juce::Rectangle<float>(static_cast<float>(plotBounds.getX()), static_cast<float>(plotBounds.getY()) + static_cast<float>(group) * height,
                                     static_cast<float>(plotBounds.getWidth()), height)
            : trackBounds(lanes[indices[0]].id).toFloat().withX(static_cast<float>(waveformBounds.getX()))
                .withWidth(static_cast<float>(waveformBounds.getWidth()));
        auto area = bounds.reduced(0, 8);
        if (area.getHeight() < 5 || area.getWidth() < 1) continue;
        std::vector<WaveformFrame> frames;
        for (auto n : indices)
        {
            const auto& lane = lanes[n]; const bool synced = window.synced && lane.synced;
            const auto independentEnd = view.frozen() && std::isfinite(lane.frozenEnd) ? lane.frozenEnd
                : ! view.frozen() && ! owner.displayService().held() && timestamp > 0 && lane.sourceHandle && lane.sourceHandle->presentation.initialized()
                ? lane.sourceHandle->presentation.cursorAt(timestamp) : static_cast<double>(lane.end);
            const double start = synced ? lane.anchor.sampleAt(window.begin, lane.offsetSamples)
                                        : independentEnd - window.seconds * supportedSampleRate;
            const double end = synced ? lane.anchor.sampleAt(window.end, lane.offsetSamples) : independentEnd;
            frames.push_back((aligned ? WaveformRenderer::analyseAligned : WaveformRenderer::analyse)(lane, start, end, static_cast<int>(std::ceil(area.getWidth() * pixelScale)), view.channelMode()));
        }
        const auto range = WaveformRenderer::amplitudeRange(view.selectedHeadroomDb());
        plots.push_back({ std::move(indices), std::move(frames), area, range, overlay });
    }
}

void ScopeEditor::paintWaveforms(juce::Graphics& graphics)
{
    using namespace scopeStyle;
    if (waveformBounds.isEmpty()) return;
    const float pixelScale = std::max(1.0f, graphics.getInternalContext().getPhysicalPixelScaleFactor());
    if (! gpuActive) preparePlots(false, pixelScale, presentationTime);
    const auto& window = frameWindow; const auto& lanes = frameLanes;
    for (const auto id : trackOrder)
    {
        const auto bounds = trackBounds(id);
        graphics.setColour(surface);
        graphics.fillRect(bounds.withWidth(trackRailWidth));
        graphics.setColour(rule);
        graphics.drawHorizontalLine(bounds.getBottom() - 1, 0.0f, static_cast<float>(trackCanvas.getWidth()));
        graphics.drawVerticalLine(trackRailWidth, static_cast<float>(bounds.getY()), static_cast<float>(bounds.getBottom()));
        if (view.selectedLayout() == ViewLayout::stacked
            && std::none_of(lanes.begin(), lanes.end(), [id](const ViewLane& lane) { return lane.id == id; }))
        {
            graphics.setFont(juce::FontOptions(12.0f)); graphics.setColour(muted);
            graphics.drawText("Track hidden", bounds.withTrimmedLeft(trackRailWidth + 12), juce::Justification::centred);
        }
    }
    if (lanes.empty())
    {
        graphics.setColour(muted); graphics.setFont(juce::FontOptions(14.0f));
        graphics.drawText("Show a track to view its signal", waveformBounds, juce::Justification::centred);
        return;
    }
    for (const auto& groupPlot : plots)
    {
        const auto& indices = groupPlot.indices; const auto& frames = groupPlot.frames;
        auto area = groupPlot.area; const auto range = groupPlot.range; const bool overlay = groupPlot.overlay;
        WaveformRenderer::drawGrid(graphics, area, window, window.synced && lanes[indices[0]].synced, range);
        const bool wide = std::none_of(frames.begin(), frames.end(), [](const WaveformFrame& frame) { return frame.individualSamples; });
        if (wide && ! gpuActive)
        {
            juce::Image composite(juce::Image::ARGB, static_cast<int>(std::ceil(area.getWidth() * pixelScale)),
                std::max(1, static_cast<int>(std::ceil(area.getHeight() * pixelScale))), true, juce::NativeImageType());
            for (std::size_t i = 0; i < indices.size(); ++i)
                WaveformRenderer::rasterize(composite, frames[i], juce::Colour(lanes[indices[i]].colour).withAlpha(overlay && focusedSource != 0
                    && lanes[indices[i]].id != focusedSource ? 0.18f : 1.0f), range, lanes[indices[i]].clipAmplitude(), overlay, pixelScale);
            graphics.drawImageTransformed(composite, juce::AffineTransform::scale(1 / pixelScale).translated(area.getX(), area.getY()));
        }
        for (std::size_t i = 0; i < indices.size(); ++i)
        {
            const auto& lane = lanes[indices[i]]; const auto& frame = frames[i];
            const juce::Colour colour(lane.colour);
            const auto opacity = overlay && focusedSource != 0 && lane.id != focusedSource ? 0.18f : 1.0f;
            if (! wide && ! gpuActive) WaveformRenderer::draw(graphics, area, frame, colour.withAlpha(opacity), range, lane.clipAmplitude(), pixelScale);
            if (! juce::exactlyEqual(lane.clipThresholdDb, 0.0))
            {
                graphics.setColour(colour.withAlpha(0.45f * opacity));
                const float dashes[] { 3, 5 };
                for (float value : { lane.clipAmplitude(), -lane.clipAmplitude() })
                    if (std::abs(value) <= range)
                        graphics.drawDashedLine({ area.getX(), WaveformRenderer::yFor(value, area, range),
                            area.getRight(), WaveformRenderer::yFor(value, area, range) }, dashes, 2, 1);
            }
            const auto rail = trackBounds(lane.id).withWidth(trackRailWidth).reduced(10, 5);
            if (std::none_of(rows.begin(), rows.end(), [&](const auto& row) { return row->id == lane.id; }))
            {
                graphics.setColour(ink); graphics.setFont(juce::FontOptions(14.0f));
                graphics.drawText(lane.name, rail.withHeight(24), juce::Justification::centredLeft);
                graphics.setColour(muted); graphics.setFont(juce::FontOptions(11.0f));
                graphics.drawText("Frozen / source removed", rail.withTrimmedTop(26).withHeight(24), juce::Justification::centredLeft);
            }
        }
        WaveformRenderer::drawReferences(graphics, area, range);
        if (! (window.synced && lanes[indices[0]].synced))
        {
            graphics.setFont(juce::FontOptions(10.5f)); graphics.setColour(muted);
            graphics.drawText(lanes[indices[0]].name + " / independent time view", area.removeFromTop(14), juce::Justification::centredLeft);
        }
    }
}
