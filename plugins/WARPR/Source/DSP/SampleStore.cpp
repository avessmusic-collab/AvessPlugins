#include "DSP/SampleStore.h"

namespace warpr
{
    //==========================================================================
    class SampleStore::Loader : public juce::Thread
    {
    public:
        explicit Loader (SampleStore& ownerStore)
            : juce::Thread ("WARPR sample loader"), owner (ownerStore)
        {
            startThread (juce::Thread::Priority::low);
        }

        ~Loader() override { stopLoader(); }

        void request (const juce::File& file, LoadedCallback onFinished)
        {
            {
                const juce::ScopedLock sl (lock);
                pendingFile = file;
                pendingCallback = std::move (onFinished);
                hasPending = true;
            }

            wake.signal();
        }

        void stopLoader()
        {
            signalThreadShouldExit();
            wake.signal();
            stopThread (10000);
        }

        void run() override
        {
            while (! threadShouldExit())
            {
                wake.wait (-1);

                if (threadShouldExit())
                    break;

                juce::File file;
                LoadedCallback callback;
                bool have = false;

                {
                    const juce::ScopedLock sl (lock);

                    if (hasPending)
                    {
                        file = pendingFile;
                        callback = std::move (pendingCallback);
                        pendingCallback = nullptr;
                        hasPending = false;
                        have = true;
                    }
                }

                if (! have)
                    continue;

                juce::String error;
                auto data = SampleStore::decodeFile (file, owner.formatManager, error);

                if (data != nullptr)
                    owner.publish (data);

                if (callback)
                    callback (data != nullptr, error);
            }
        }

    private:
        SampleStore& owner;
        juce::WaitableEvent wake;
        juce::CriticalSection lock;
        juce::File pendingFile;
        LoadedCallback pendingCallback;
        bool hasPending = false;
    };

    //==========================================================================
    SampleStore::SampleStore (RetireList& retireListToUse)
        : swap (retireListToUse)
    {
        formatManager.registerBasicFormats();
        publish (SampleData::createDefaultWave());
    }

    SampleStore::~SampleStore()
    {
        shutdown();
    }

    void SampleStore::shutdown()
    {
        if (loader != nullptr)
        {
            loader->stopLoader();
            loader.reset();
        }
    }

    void SampleStore::publish (SampleData::Ptr data)
    {
        if (data == nullptr)
            return;

        {
            const juce::ScopedLock sl (latestLock);
            latestPtr = data;
        }

        data->incReferenceCount();                       // the swap owns this unit
        swap.publish (data.get());
    }

    SampleData::Ptr SampleStore::latest() const
    {
        const juce::ScopedLock sl (latestLock);
        return latestPtr;
    }

    void SampleStore::loadFileAsync (const juce::File& file, LoadedCallback onFinished)
    {
        if (loader == nullptr)
            loader = std::make_unique<Loader> (*this);

        loader->request (file, std::move (onFinished));
    }

    SampleData::Ptr SampleStore::loadFileBlocking (const juce::File& file, juce::String& errorMessage)
    {
        auto data = decodeFile (file, formatManager, errorMessage);

        if (data != nullptr)
            publish (data);

        return data;
    }

    void SampleStore::releaseRef (void* object) noexcept
    {
        static_cast<SampleData*> (object)->decReferenceCount();
    }

    SampleData::Ptr SampleStore::decodeFile (const juce::File& file, juce::AudioFormatManager& manager,
                                             juce::String& errorMessage)
    {
        if (! file.existsAsFile())
        {
            errorMessage = "File not found";
            return nullptr;
        }

        std::unique_ptr<juce::AudioFormatReader> reader (manager.createReaderFor (file));

        if (reader == nullptr)
        {
            errorMessage = "Unsupported or unreadable audio file";
            return nullptr;
        }

        if (reader->lengthInSamples <= 0 || reader->numChannels == 0 || ! (reader->sampleRate > 0.0))
        {
            errorMessage = "The file contains no audio";
            return nullptr;
        }

        // soft cap: 10 minutes (architecture A8), and never more than int range / 4 frames
        const juce::int64 cap = (juce::int64) (reader->sampleRate * 600.0);
        const int frames = (int) juce::jmin<juce::int64> (reader->lengthInSamples, cap, (juce::int64) (0x7fffffff / 4));
        const int chans = reader->numChannels >= 2 ? 2 : 1;

        juce::AudioBuffer<float> buffer (chans, frames);

        if (! reader->read (&buffer, 0, frames, 0, true, chans == 2))
        {
            errorMessage = "The audio data could not be read";
            return nullptr;
        }

        auto data = SampleData::create (buffer, reader->sampleRate, file.getFileNameWithoutExtension(), false, false);

        if (data == nullptr)
            errorMessage = "The sample could not be built";

        return data;
    }
}
