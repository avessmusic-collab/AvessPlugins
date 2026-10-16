#pragma once

// Parameter and state ids, verbatim from parameter-spec.md (LOCKED). Case-sensitive.
namespace prism::ids
{
    // 17 APVTS parameters, in spec order
    inline constexpr const char* fftSize          = "fft_size";
    inline constexpr const char* window           = "window";
    inline constexpr const char* smoothing        = "smoothing";
    inline constexpr const char* spectrumTilt     = "spectrum_tilt";
    inline constexpr const char* displayRange     = "display_range";
    inline constexpr const char* spectrogramSpeed = "spectrogram_speed";
    inline constexpr const char* peakDecay        = "peak_decay";
    inline constexpr const char* freeze           = "freeze";
    inline constexpr const char* analysisMode     = "analysis_mode";
    inline constexpr const char* colorTheme       = "color_theme";
    inline constexpr const char* preset           = "preset";
    inline constexpr const char* reducedMotion    = "reduced_motion";
    inline constexpr const char* glowIntensity    = "glow_intensity";
    inline constexpr const char* src1Visible      = "src1_visible";
    inline constexpr const char* src1Solo         = "src1_solo";
    inline constexpr const char* src2Visible      = "src2_visible";
    inline constexpr const char* src2Solo         = "src2_solo";

    inline constexpr int kParameterCount = 17;

    // ValueTree state entries (properties of apvts.state, NOT parameters)
    inline constexpr const char* src1Name = "src1_name";
    inline constexpr const char* src2Name = "src2_name";

    // Serialized UI state (properties of apvts.state)
    inline constexpr const char* uiRowSplits      = "ui_row_splits";
    inline constexpr const char* uiLowerColSplits = "ui_lower_col_splits";
    inline constexpr const char* uiSgOrientation  = "ui_spectrogram_orientation";
    inline constexpr const char* uiWaveformSpan   = "ui_waveform_span";   // bars: continuous 0.5 .. 32 (mouse-wheel zoom; chip presets 1/2/4/8/16/32; default 8)
    inline constexpr const char* uiGonioSource    = "ui_gonio_source";    // 0 | 1: which source the vectorscope shows (UI only)
    inline constexpr const char* uiScopeSource    = "ui_scope_source";    // 0 | 1: which source the scope shows (UI only)
    inline constexpr const char* uiFullscreen     = "ui_fullscreen";
    inline constexpr const char* uiWidth          = "ui_width";
    inline constexpr const char* uiHeight         = "ui_height";
} // namespace prism::ids
