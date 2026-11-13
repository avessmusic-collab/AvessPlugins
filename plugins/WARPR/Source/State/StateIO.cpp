#include "State/StateIO.h"
#include "PluginIdentity.h"
#include "State/SampleCodec.h"

#include <cstring>

namespace warpr::state
{
    namespace
    {
        constexpr std::size_t kMaxXmlBytes = 64u * 1024u * 1024u;   // sanity cap against garbage lengths
        const juce::Identifier kUiStateVersion { "warprStateVersion" };

        bool startsWith (const char* s, std::size_t n, std::size_t at, const char* what) noexcept
        {
            const auto len = std::strlen (what);
            return at + len <= n && std::memcmp (s + at, what, len) == 0;
        }

        /** Index just after the first occurrence of `what` at or after `from` (n when there is none). */
        std::size_t skipPast (const char* s, std::size_t n, std::size_t from, const char* what) noexcept
        {
            const auto len = std::strlen (what);

            for (std::size_t i = from; i + len <= n; ++i)
                if (std::memcmp (s + i, what, len) == 0)
                    return i + len;

            return n;
        }

        int uiStateVersionOf (const juce::ValueTree& tree)
        {
            const auto ui = tree.getChildWithName (identity::uiStateTag);
            return ui.isValid() ? juce::jmax (0, static_cast<int> (ui.getProperty (kUiStateVersion, 1))) : 1;
        }
    }

    bool xmlNestsTooDeeply (const char* s, std::size_t n, int limit) noexcept
    {
        int depth = 0;
        std::size_t i = 0;

        while (i < n)
        {
            if (s[i] != '<')
            {
                ++i;
                continue;
            }

            if (startsWith (s, n, i, "<!--"))        { i = skipPast (s, n, i + 4, "-->");  continue; }
            if (startsWith (s, n, i, "<![CDATA["))   { i = skipPast (s, n, i + 9, "]]>");  continue; }
            if (startsWith (s, n, i, "<!") || startsWith (s, n, i, "<?")) { i = skipPast (s, n, i + 2, ">"); continue; }

            const bool closing = startsWith (s, n, i, "</");
            char quote = 0;
            char previous = 0;
            std::size_t j = i + 1;

            if (! closing && ++depth > limit)
                return true;

            for (; j < n; ++j)
            {
                const char c = s[j];

                if (quote != 0)
                {
                    if (c == quote)
                        quote = 0;
                }
                else if (c == '"' || c == '\'')
                {
                    quote = c;
                }
                else if (c == '>')
                {
                    if ((closing || previous == '/') && depth > 0)
                        --depth;                             // </x> or <x/>

                    break;
                }

                previous = c;
            }

            i = j + 1;
        }

        return false;
    }

    void migrate (juce::ValueTree& tree, int fromUiStateVersion)
    {
        // Version 1 is current.  Future steps slot in here in order, e.g.:
        //     if (fromUiStateVersion < 2) { ...rewrite tree...; }
        // Rules (CORRUPTR lesson): removed parameters are never re-indexed, choice lists are append-only, dead
        // entries are retained.  Unknown newer versions fall through untouched (best-effort read).
        juce::ignoreUnused (tree, fromUiStateVersion);
    }

    //==========================================================================
    juce::MemoryBlock write (juce::AudioProcessorValueTreeState& apvts, const UiModel& model)
    {
        return write (apvts, model, juce::MemoryBlock());
    }

    juce::MemoryBlock write (juce::AudioProcessorValueTreeState& apvts, const UiModel& model, const juce::MemoryBlock& sampleBlob)
    {
        auto tree = apvts.copyState();

        // Never carry a stale UI-state child from a previous load; the model is authoritative.
        for (auto stale = tree.getChildWithName (identity::uiStateTag); stale.isValid();
             stale = tree.getChildWithName (identity::uiStateTag))
            tree.removeChild (stale, nullptr);

        tree.appendChild (model.toValueTree(), nullptr);

        juce::String xmlText;
        if (auto xml = tree.createXml())
            xmlText = xml->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader());

        const auto numXmlBytes = xmlText.getNumBytesAsUTF8();

        juce::MemoryOutputStream out;
        out.writeInt (static_cast<int> (identity::containerMagic));
        out.writeInt (identity::containerVersion);
        out.writeInt (static_cast<int> (numXmlBytes));
        out.write (xmlText.toRawUTF8(), numXmlBytes);
        out.writeInt (static_cast<int> (static_cast<juce::uint32> (sampleBlob.getSize())));      // sampleBlobByteCount (0 = none)

        if (sampleBlob.getSize() > 0)
            out.write (sampleBlob.getData(), sampleBlob.getSize());

        return out.getMemoryBlock();
    }

    bool readXml (const juce::XmlElement& xml,
                  juce::AudioProcessorValueTreeState& apvts, UiModel& modelOut)
    {
        auto tree = juce::ValueTree::fromXml (xml);

        if (! tree.isValid() || tree.getType() != apvts.state.getType())
            return false;

        migrate (tree, uiStateVersionOf (tree));
        modelOut.fromValueTree (tree.getChildWithName (identity::uiStateTag));

        // The UI-state child belongs to the model, not to the APVTS tree.
        for (auto child = tree.getChildWithName (identity::uiStateTag); child.isValid();
             child = tree.getChildWithName (identity::uiStateTag))
            tree.removeChild (child, nullptr);

        apvts.replaceState (tree);
        return true;
    }

    bool readContainer (const void* data, std::size_t numBytes,
                        juce::AudioProcessorValueTreeState& apvts, UiModel& modelOut, SampleLoad* sampleOut)
    {
        if (sampleOut != nullptr)
            *sampleOut = {};

        if (data == nullptr || numBytes < kHeaderBytes)
            return false;

        const auto* bytes = static_cast<const juce::uint8*> (data);

        if (juce::ByteOrder::littleEndianInt (bytes) != identity::containerMagic)
            return false;

        const auto containerVersion = static_cast<int> (juce::ByteOrder::littleEndianInt (bytes + 4));
        if (containerVersion < 1)
            return false;                                        // newer versions: read what we understand

        const auto numXmlBytes = static_cast<std::size_t> (juce::ByteOrder::littleEndianInt (bytes + 8));
        const auto remaining   = numBytes - kHeaderBytes;

        if (numXmlBytes == 0 || numXmlBytes > kMaxXmlBytes || numXmlBytes > remaining)
            return false;

        // The u32 blob byte count is part of every container version >= 1; a state that ends before it is truncated.
        if (remaining - numXmlBytes < sizeof (juce::uint32))
            return false;

        const auto* xmlBegin  = reinterpret_cast<const char*> (bytes + kHeaderBytes);
        const auto* blobCount = bytes + kHeaderBytes + numXmlBytes;
        const auto numBlobBytes = static_cast<std::size_t> (juce::ByteOrder::littleEndianInt (blobCount));
        const auto* blobBegin = blobCount + sizeof (juce::uint32);

        if (numBlobBytes > remaining - numXmlBytes - sizeof (juce::uint32))
            return false;                                        // blob extends past the end: truncated

        if (xmlNestsTooDeeply (xmlBegin, numXmlBytes))
            return false;

        const auto xmlText = juce::String::fromUTF8 (xmlBegin, static_cast<int> (numXmlBytes));
        const auto xml = juce::XmlDocument::parse (xmlText);
        if (xml == nullptr)
            return false;

        // Validate the root before the (expensive) sample decode so a foreign document costs nothing.
        {
            const auto probe = juce::ValueTree::fromXml (*xml);
            if (! probe.isValid() || probe.getType() != apvts.state.getType())
                return false;
        }

        SampleLoad load;
        load.hadBlob = numBlobBytes > 0;

        if (load.hadBlob && sampleOut != nullptr)
        {
            samplecodec::Status status = samplecodec::Status::corrupt;
            auto decoded = samplecodec::decode (blobBegin, numBlobBytes, status);

            if (status == samplecodec::Status::ok)
                load.sample = decoded;
            else if (status == samplecodec::Status::corrupt)
                load.corruptBlob = true;
            // defaultWave: nothing embedded, the loader regenerates it
        }

        if (! readXml (*xml, apvts, modelOut))
            return false;

        if (sampleOut != nullptr)
            *sampleOut = load;

        return true;
    }
}
