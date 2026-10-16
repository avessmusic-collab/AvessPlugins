#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Parameters/ParameterLayout.h"
#include "UiStateUtil.h"

namespace
{
    using namespace prism::uistate;   // kMaxNameLength, kDefaultWidth/kMinWidth/kMaxWidth, parseInts, joinInts (shared with the editor)
    constexpr int kRowTotal = 598, kRowMinSpec = 120, kRowMinWave = 40, kRowMinLower = 100;
    constexpr int kColTotal = 834, kColMinSpectrogram = 224, kColMinOther = 180;

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
    if (! state.hasProperty(uiWaveformSpan))    state.setProperty(uiWaveformSpan, 8, nullptr);
    if (! state.hasProperty(uiGonioSource))     state.setProperty(uiGonioSource, 0, nullptr);
    if (! state.hasProperty(uiScopeSource))     state.setProperty(uiScopeSource, 0, nullptr);
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

    state.setProperty(uiGonioSource, (int) state.getProperty(uiGonioSource, 0) == 1 ? 1 : 0, nullptr);   // UI-only source selectors: {0, 1}
    state.setProperty(uiScopeSource, (int) state.getProperty(uiScopeSource, 0) == 1 ? 1 : 0, nullptr);

    const int span = (int) state.getProperty(uiWaveformSpan, 8);   // bars: 1 | 2 | 4 | 8 | 16 | 32 (older builds stored seconds -> values outside the set reset to 8)
    state.setProperty(uiWaveformSpan, (span == 1 || span == 2 || span == 4 || span == 8 || span == 16 || span == 32) ? span : 8, nullptr);

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
        mailbox.clear();   // worker is stopped: nothing published before the rebuild (old rate / layout / integrated state) may survive
        engine = std::make_unique<prism::AnalysisEngine>(cfg);
        engine->setSequenceBase(base);
        engine->attachControls({ &resetAllEpoch, &resetIntegratedEpoch,
                                 apvts.getRawParameterValue(prism::ids::analysisMode),
                                 apvts.getRawParameterValue(prism::ids::fftSize),
                                 apvts.getRawParameterValue(prism::ids::window),
                                 &lastFetchedSequence });   // before the worker starts
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

   #if PRISM_DEBUG_TONE
    // DEBUG ONLY (compile flag, OFF by default): analyse a synthetic 1 kHz / -12 dBFS stereo sine instead of the input.
    // The host buffer itself is still never touched. Allocation here is acceptable for a debug build.
    {
        if ((int) toneBuf.size() < numSamples) toneBuf.assign((size_t) numSamples, 0.0f);
        const double fs = getSampleRate() > 0.0 ? getSampleRate() : 48000.0, inc = juce::MathConstants<double>::twoPi * 1000.0 / fs;
        int left = toneLeft.load(std::memory_order_relaxed);   // -1 = endless, 0 = silent, >0 = samples of tone remaining (sample-accurate bursts)
        if (left > 0 && toneStartMs.load(std::memory_order_relaxed) == 0)
            toneStartMs.store((long long) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
        for (int i = 0; i < numSamples; ++i)
        {
            const bool on = left < 0 || i < left;
            toneBuf[(size_t) i] = on ? 0.25f * (float) std::sin(tonePhase) : 0.0f;
            tonePhase += inc;
            if (tonePhase > juce::MathConstants<double>::twoPi) tonePhase -= juce::MathConstants<double>::twoPi;
        }
        if (left > 0) toneLeft.store(juce::jmax(0, left - numSamples), std::memory_order_relaxed);
        const float* ptrs[2] = { toneBuf.data(), toneBuf.data() };
        e->captureBlock(ptrs, 2, numSamples);
        e->capturePlayhead(getPlayHead(), numSamples);
        return;
    }
   #endif

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
