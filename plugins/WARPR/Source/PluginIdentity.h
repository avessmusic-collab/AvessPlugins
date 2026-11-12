#pragma once
/*
  ==============================================================================
   PluginIdentity.h - the ONE header holding every product-name-dependent string.

   Product name: VOID (A21 resolved 2026-11-12; folder/targets keep the WARPR codename).  To rename:
     1. change the strings in the "RENAMEABLE" block below,
     2. change PRODUCT_NAME in CMakeLists.txt (and the plugin folder if desired),
     3. uninstall the old bundles FIRST (juce8-critical-patterns #14).

   Parameter IDs never contain the product name and survive a rename.

   FROZEN block: these are part of the saved-state wire format.  They must NOT change
   after a rename, or every saved project / preset would stop loading.

   AU/VST3 identity (CMakeLists.txt): PLUGIN_CODE "Wrpr", PLUGIN_MANUFACTURER_CODE "Vstf"
   are a WORKING DECISION (architecture ambiguity A21), pending final name.  They must be
   settled before the first public build: changing them later orphans saved DAW projects.
  ==============================================================================
*/

namespace warpr::identity
{
    // ------------------------------------------------------------------ RENAMEABLE
    inline constexpr const char* productName       = "VOID";                  // final name (A21 resolved 2026-11-12)
    inline constexpr const char* companyName       = "VST Factory";
    inline constexpr const char* companyDirName    = "VST Factory";           // <userApplicationData>/<this>/<product>/Presets
    inline constexpr const char* presetExtension   = ".void";
    inline constexpr const char* presetDirName     = "Presets";
    inline constexpr const char* webView2DataDir   = "VOID-WebView2";        // writable WebView2 user-data folder (Windows)
    inline constexpr const char* resourceLogPrefix = "VOID";

    // Documentation mirror of the CMake identity (CMakeLists.txt is authoritative).
    inline constexpr const char* pluginCode        = "Void";                  // final (A21 resolved: VOID)
    inline constexpr const char* manufacturerCode  = "Vstf";                  // final (A21 resolved)

    // ------------------------------------------------------------------ FROZEN (state wire format)
    inline constexpr const char* apvtsStateId      = "Parameters";            // APVTS ValueTree root type
    inline constexpr const char* uiStateTag        = "WARPR_UI_STATE";        // child of the APVTS state
    inline constexpr unsigned int containerMagic   = 0x52505257u;             // bytes on disk: 'W' 'R' 'P' 'R' (little-endian u32)
    inline constexpr int containerVersion          = 1;                       // binary container layout version
    inline constexpr int uiStateVersion            = 1;                       // warprStateVersion (UI-state migration)
}
