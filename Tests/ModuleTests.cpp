#include <juce_audio_utils/juce_audio_utils.h>
#include <dlfcn.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace
{
void require(bool condition, const char* description)
{
    if (! condition) throw std::runtime_error(description);
}
class PlayHead final : public juce::AudioPlayHead
{
public:
    juce::Optional<PositionInfo> getPosition() const override { return position; }
    PositionInfo position;
};
}

int main(int argc, char** argv)
{
    try
    {
        require(argc == 2, "Expected VST3 bundle path");
        juce::ScopedJuceInitialiser_GUI juce;
        const juce::File bundle(argv[1]);
        juce::VST3PluginFormat format;
        juce::OwnedArray<juce::PluginDescription> descriptions;
        format.findAllTypesForFile(descriptions, bundle.getFullPathName());
        require(descriptions.size() == 1, "VST3 scanner did not find exactly one plugin");
        juce::String error;
        auto first = format.createInstanceFromDescription(*descriptions[0], 48000, 128, error);
        auto second = format.createInstanceFromDescription(*descriptions[0], 48000, 128, error);
        if (! first || ! second) throw std::runtime_error(error.toStdString());
        const auto binary = bundle.getChildFile("Contents/MacOS/Oscilloscope");
        void* module = dlopen(binary.getFullPathName().toRawUTF8(), RTLD_NOW | RTLD_LOCAL);
        require(module != nullptr, "Could not open loaded VST3 image");
        const auto count = reinterpret_cast<unsigned int (*)()>(dlsym(module, "oscilloscopeProbeSourceCount"));
        const auto token = reinterpret_cast<const char* (*)()>(dlsym(module, "oscilloscopeProbeRegistryToken"));
        require(count != nullptr && token != nullptr, "Development registry hooks absent from VST3");
        const auto captureCount = reinterpret_cast<unsigned int (*)()>(dlsym(module, "oscilloscopeCaptureSourceCount"));
        const auto drain = reinterpret_cast<void (*)()>(dlsym(module, "oscilloscopeCaptureDrain"));
        const auto samples = reinterpret_cast<std::uint64_t (*)(unsigned int)>(dlsym(module, "oscilloscopeCapturedSamples"));
        require(captureCount && drain && samples && captureCount() == 2, "Raw capture registry hooks or sources absent");
        require(count() == 2, "Actual VST3 instances did not share one module-local registry");
        const auto registry = juce::String(token());
        require(registry.isNotEmpty(), "Module registry identity absent");
        first->prepareToPlay(48000, 128);
        second->prepareToPlay(48000, 128);
        PlayHead playHead;
        playHead.position.setIsPlaying(true); playHead.position.setBpm(120);
        playHead.position.setTimeInSamples(0); playHead.position.setPpqPosition(0);
        first->setPlayHead(&playHead); second->setPlayHead(&playHead);
        require(first->getLatencySamples() == 0 && second->getLatencySamples() == 0, "VST3 reported latency");
        {
            std::unique_ptr<juce::AudioProcessorEditor> editor(first->createEditorIfNeeded());
            require(editor != nullptr, "VST3 editor could not open");
            juce::AudioBuffer<float> audio(2, 128);
            audio.clear(); audio.setSample(0, 33, 1.5f); audio.setSample(1, 65, -1.5f);
            juce::AudioBuffer<float> before; before.makeCopyOf(audio);
            juce::MidiBuffer midi;
            first->processBlock(audio, midi); second->processBlock(audio, midi);
            for (int channel = 0; channel < 2; ++channel)
                require(std::memcmp(audio.getReadPointer(channel), before.getReadPointer(channel), 128 * sizeof(float)) == 0,
                        "Loaded VST3 changed audio");
            require(first->supportsDoublePrecisionProcessing() && second->supportsDoublePrecisionProcessing(),
                    "Loaded VST3 did not advertise double precision");
            first->releaseResources(); second->releaseResources();
            first->setProcessingPrecision(juce::AudioProcessor::doublePrecision);
            second->setProcessingPrecision(juce::AudioProcessor::doublePrecision);
            first->prepareToPlay(48000, 128); second->prepareToPlay(48000, 128);
            playHead.position.setTimeInSamples(128); playHead.position.setPpqPosition(128.0 / 24000);
            juce::AudioBuffer<double> precise(2, 128);
            precise.clear(); precise.setSample(0, 11, 1.500000000123); precise.setSample(1, 19, -2.500000000123);
            juce::AudioBuffer<double> preciseBefore; preciseBefore.makeCopyOf(precise);
            first->processBlock(precise, midi); second->processBlock(precise, midi);
            for (int channel = 0; channel < 2; ++channel)
                require(std::memcmp(precise.getReadPointer(channel), preciseBefore.getReadPointer(channel), 128 * sizeof(double)) == 0,
                        "Loaded VST3 changed double-precision audio");
            drain();
            require(samples(0) == 256 && samples(1) == 256,
                    "One VST3 editor did not capture both taps, including unopened editor");
            require(count() == 2 && registry == juce::String(token()), "Editor changed shared discovery identity");
        }
        first->releaseResources(); second->releaseResources();
        second.reset(); require(count() == 1, "VST3 removal left stale source");
        first.reset(); require(count() == 0, "VST3 teardown leaked sources");
        dlclose(module);
        std::cout << "PASS: actual arm64 VST3 scanning, two-instance module sharing, editor, audio identity, zero latency, teardown\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
