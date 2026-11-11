#include "State/StateIO.h"
#include "PluginIdentity.h"

namespace warpr::state
{
    namespace
    {
        constexpr std::size_t kHeaderBytes = 12;                 // magic + containerVersion + xmlByteCount
        constexpr std::size_t kMaxXmlBytes = 64u * 1024u * 1024u;   // sanity cap against garbage lengths
    }

    juce::MemoryBlock write (juce::AudioProcessorValueTreeState& apvts, const UiModel& model)
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
        out.writeInt (0);                                        // sampleBlobByteCount: none in Stage 1

        return out.getMemoryBlock();
    }

    bool readXml (const juce::XmlElement& xml,
                  juce::AudioProcessorValueTreeState& apvts, UiModel& modelOut)
    {
        auto tree = juce::ValueTree::fromXml (xml);

        if (! tree.isValid() || tree.getType() != apvts.state.getType())
            return false;

        modelOut.fromValueTree (tree.getChildWithName (identity::uiStateTag));

        // The UI-state child belongs to the model, not to the APVTS tree.
        for (auto child = tree.getChildWithName (identity::uiStateTag); child.isValid();
             child = tree.getChildWithName (identity::uiStateTag))
            tree.removeChild (child, nullptr);

        apvts.replaceState (tree);
        return true;
    }

    bool readContainer (const void* data, std::size_t numBytes,
                        juce::AudioProcessorValueTreeState& apvts, UiModel& modelOut)
    {
        if (data == nullptr || numBytes < kHeaderBytes)
            return false;

        juce::MemoryInputStream in (data, numBytes, false);

        if (static_cast<juce::uint32> (in.readInt()) != identity::containerMagic)
            return false;

        const auto containerVersion = in.readInt();
        if (containerVersion < 1)
            return false;                                        // newer versions: read what we understand

        const auto numXmlBytes = static_cast<juce::uint32> (in.readInt());
        const auto remaining   = numBytes - kHeaderBytes;

        if (numXmlBytes == 0 || numXmlBytes > kMaxXmlBytes || numXmlBytes > remaining)
            return false;

        const auto* xmlBegin = static_cast<const char*> (data) + kHeaderBytes;
        const auto xmlText   = juce::String::fromUTF8 (xmlBegin, static_cast<int> (numXmlBytes));

        const auto xml = juce::XmlDocument::parse (xmlText);
        if (xml == nullptr)
            return false;

        // D9: the sample blob (u32 byte count + payload) follows the XML; Stage 1 ignores it.
        return readXml (*xml, apvts, modelOut);
    }
}
