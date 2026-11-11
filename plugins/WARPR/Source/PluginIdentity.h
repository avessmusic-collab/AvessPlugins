#pragma once
/*
  ==============================================================================
   PluginIdentity.h - the ONE header holding every product-name-dependent string.

   WARPR is a WORKING TITLE ("pending final name").  To rename the product:
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
    inline constexpr const char* productName       = "WARPR";                 // pending final name
    inline constexpr const char* companyName       = "VST Factory";
    inline constexpr const char* companyDirName    = "VST Factory";           // <userApplicationData>/<this>/<product>/Presets
    inline constexpr const char* presetExtension   = ".warpr";
    inline constexpr const char* presetDirName     = "Presets";
    inline constexpr const char* webView2DataDir   = "WARPR-WebView2";        // writable WebView2 user-data folder (Windows)
    inline constexpr const char* resourceLogPrefix = "WARPR";

    // Documentation mirror of the CMake identity (CMakeLists.txt is authoritative).
    inline constexpr const char* pluginCode        = "Wrpr";                  // pending final name (A21)
    inline constexpr const char* manufacturerCode  = "Vstf";                  // pending final name (A21)

    // ------------------------------------------------------------------ FROZEN (state wire format)
    inline constexpr const char* apvtsStateId      = "Parameters";            // APVTS ValueTree root type
    inline constexpr const char* uiStateTag        = "WARPR_UI_STATE";        // child of the APVTS state
    inline constexpr unsigned int containerMagic   = 0x52505257u;             // bytes on disk: 'W' 'R' 'P' 'R' (little-endian u32)
    inline constexpr int containerVersion          = 1;                       // binary container layout version
    inline constexpr int uiStateVersion            = 1;                       // warprStateVersion (UI-state migration)
}
