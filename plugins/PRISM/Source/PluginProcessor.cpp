#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Parameters/ParameterLayout.h"

namespace
{
    constexpr int kMaxNameLength = 10;
    constexpr int kRowTotal = 598, kRowMinSpec = 120, kRowMinWave = 40, kRowMinLower = 100;
    constexpr int kColTotal = 834, kColMinSpectrogram = 224, kColMinOther = 180;
    constexpr int kDefaultWidth = 1100, kMinWidth = 770, kMaxWidth = 8192;

    juce::Array<int> parseInts(const juce::String& csv)
    {
        juce::Array<int> out;
        for (auto& t : juce::StringArray::fromTokens(csv, ",", ""))
            out.add(t.getIntValue());
        return out;
    }

    juce::String joinInts(const juce::Array<int>& v)
    {
        juce::StringArray parts;
        for (int e : v)
            parts.add(juce::String(e));
        return parts.joinIntoString(",");
    }

    juce::String sanitizeRows(const juce::String& csv)
    {
        const auto in = parseInts(csv);
        const int a0 = in.size() == 3 ? in[0] : 226, b0 = in.size() == 3 ? in[1] : 70;
        const int a = juce::jlimit(kRowMinSpec, kRowTotal - kRowMinWave - kRowMinLower, a0);
        const int b = juce::jlimit(kRowMinWave, kRowTotal - a - kRowMinLower, b0);
        return joinInts({ a, b, kRowTotal - a - b });
    }

    juce::String sanitizeCols(const juce::String& csv)
    {
        const auto in = parseInts(csv);
        const int a0 = in.size() == 3 ? in[0] : 296, b0 = in.size() == 3 ? in[1] : 266;
        const int a = juce::jlimit(kColMinSpectrogram, kColTotal - 2 * kColMinOther, a0);
        const int b = juce::jlimit(kColMinOther, kColTotal - a - kColMinOther, b0);
        return joinInts({ a, b, kColTotal - a - b });
    }

    juce::String sanitizeName(const juce::var& v, const juce::String& fallback)
    {
        auto s = v.toString().trim().substring(0, kMaxNameLength).trim();
        return s.isEmpty() ? fallback : s;
    }
}

//==============================================================================
PRISMAudioProcessor::PRISMAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                         .withInput("Sidechain", juce::AudioChannelSet::stereo(), false))
    , apvts(*this, nullptr, "Parameters", prism::createParameterLayout())
{
    applyStateDefaults(apvts.state);
}

PRISMAudioProcessor::~PRISMAudioProcessor()
{
    stopWorker();   // join before the engine/mailbox/atomics the worker references are destroyed
}

//==============================================================================
void PRISMAudioProcessor::applyStateDefaults(juce::ValueTree& state)
{
    using namespace prism::ids;

    if (! state.hasProperty(src1Name))          state.setProperty(src1Name, "Ch1", nullptr);
    if (! state.hasProperty(src2Name))          state.setProperty(src2Name, "Ch2", nullptr);
    if (! state.hasProperty(uiRowSplits))       state.setProperty(uiRowSplits, "226,70,302", nullptr);
    if (! state.hasProperty(uiLowerColSplits))  state.setProperty(uiLowerColSplits, "296,266,272", nullptr);
    if (! state.hasProperty(uiSgOrientation))   state.setProperty(uiSgOrientation, "v", nullptr);
    if (! state.hasProperty(uiFullscreen))      state.setProperty(uiFullscreen, false, nullptr);
    if (! state.hasProperty(uiWidth))           state.setProperty(uiWidth, 1100, nullptr);
    if (! state.hasProperty(uiHeight))          state.setProperty(uiHeight, 720, nullptr);
}

void PRISMAudioProcessor::sanitizeState(juce::ValueTree& state)
{
    using namespace prism::ids;

    state.setProperty(src1Name, sanitizeName(state.getProperty(src1Name), "Ch1"), nullptr);
    state.setProperty(src2Name, sanitizeName(state.getProperty(src2Name), "Ch2"), nullptr);

    state.setProperty(uiRowSplits,
                      sanitizeRows(state.getProperty(uiRowSplits, "226,70,302").toString()), nullptr);
    state.setProperty(uiLowerColSplits,
                      sanitizeCols(state.getProperty(uiLowerColSplits, "296,266,272").toString()), nullptr);

    const auto orient = state.getProperty(uiSgOrientation, "v").toString();
    state.setProperty(uiSgOrientation, orient == "h" ? "h" : "v", nullptr);

    state.setProperty(uiFullscreen, (bool) state.getProperty(uiFullscreen, false), nullptr);

    // Window size: width >= 770, height locked to the 55:36 aspect (1100x720)
    const int w = juce::jlimit(kMinWidth, kMaxWidth, (int) state.getProperty(uiWidth, kDefaultWidth));
    const int h = (int) std::lround((double) w * 36.0 / 55.0);
    state.setProperty(uiWidth, w, nullptr);
    state.setProperty(uiHeight, h, nullptr);
}

//==============================================================================
prism::EngineConfig PRISMAudioProcessor::makeEngineConfig(double sampleRate) const
{
    prism::EngineConfig cfg;
    cfg.sampleRate = sampleRate;
    const int buses = juce::jmin(getBusCount(true), prism::kMaxSources);
    for (int i = 0; i < buses; ++i)
        if (auto* bus = getBus(true, i))
            cfg.busChannels[(size_t) i] = bus->isEnabled() ? bus->getNumberOfChannels() : 0;
    return cfg;
}

void PRISMAudioProcessor::stopWorker()
{
    if (worker != nullptr)
        worker->stop();
}

void PRISMAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);

    // Hosts call prepareToPlay repeatedly: rebuild only when {rate, layout} changed.
    const auto cfg = makeEngineConfig(sampleRate);
    if (engine == nullptr || engine->getConfig() != cfg)
    {
        stopWorker();
        worker.reset();   // references the old engine
        const std::uint64_t base = engine != nullptr ? engine->getLastSequence() : 0;   // keep sequence monotonic across rebuilds
        engine = std::make_unique<prism::AnalysisEngine>(cfg);
        engine->setSequenceBase(base);
        ++engineRebuildCount;
    }

    if (worker == nullptr)
        worker = std::make_unique<prism::AnalysisWorker>(*engine, mailbox,
                                                         prism::WorkerControls { &editorActiveCount, &lastFetchTimeMs });
    worker->start();
}

void PRISMAudioProcessor::releaseResources()
{
    stopWorker();   // joins the thread; the engine (and its history/rings) is kept for a same-config prepare
}

bool PRISMAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto mainIn  = layouts.getMainInputChannelSet();
    const auto mainOut = layouts.getMainOutputChannelSet();

    // Bit-exact in-place passthrough requires identical in/out; mono or stereo only.
    if (mainIn != mainOut)
        return false;
    if (mainIn != juce::AudioChannelSet::mono() && mainIn != juce::AudioChannelSet::stereo())
        return false;

    // Optional sidechain input (bus index 1): disabled, mono or stereo.
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet(true, 1);
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo())
            return false;
    }

    return true;
}

void PRISMAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // Bit-transparent: the buffer is never written, cleared or resized.
    // No allocation, no locks, no FFT: read-only capture into the per-source SPSC rings,
    // lossless sample-peak atomics and the playhead beat queue.
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused(midiMessages);

    auto* e = engine.get();
    const int numSamples = buffer.getNumSamples();
    if (e == nullptr || numSamples <= 0)
        return;

    const auto& constBuffer = buffer;   // read pointers only: never marks the buffer as written
    e->captureBlock(constBuffer.getArrayOfReadPointers(), constBuffer.getNumChannels(), numSamples);
    e->capturePlayhead(getPlayHead(), numSamples);
}

//==============================================================================
std::shared_ptr<const prism::AnalysisSnapshot> PRISMAudioProcessor::getLatestAnalysisSnapshot() const
{
    auto p = mailbox.fetch();
    lastFetchTimeMs.store(prism::steadyNowMs(), std::memory_order_relaxed);
    if (p != nullptr)
        lastFetchedSequence.store(p->sequence, std::memory_order_relaxed);
    return p;
}

void PRISMAudioProcessor::setEditorActive(bool active)
{
    if (active)
    {
        lastFetchTimeMs.store(prism::steadyNowMs(), std::memory_order_relaxed);   // heartbeat grace on open
        editorActiveCount.fetch_add(1, std::memory_order_relaxed);
    }
    else if (editorActiveCount.load(std::memory_order_relaxed) > 0)
        editorActiveCount.fetch_sub(1, std::memory_order_relaxed);
}

juce::AudioProcessorEditor* PRISMAudioProcessor::createEditor()
{
    return new PRISMAudioProcessorEditor(*this);
}

//==============================================================================
void PRISMAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void PRISMAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));

    if (xml == nullptr || ! xml->hasTagName(apvts.state.getType()))
        return;   // invalid state: keep current values

    auto newState = juce::ValueTree::fromXml(*xml);
    if (! newState.isValid())
        return;

    // Sanitize BEFORE it reaches the APVTS/editor; unknown properties are preserved.
    applyStateDefaults(newState);
    sanitizeState(newState);
    apvts.replaceState(newState);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PRISMAudioProcessor();
}
