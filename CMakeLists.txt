cmake_minimum_required(VERSION 3.22)

project(AutoEDO VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(AUTOEDO_BUILD_PLUGIN "Build the JUCE plugin (downloads JUCE)" ON)
option(AUTOEDO_BUILD_TESTS  "Build the standalone DSP unit tests"   ON)

# Optional Avid AAX SDK. AAX is built only when this points at a local SDK copy,
# because the SDK is NDA-gated and cannot be redistributed. See the README.
set(AUTOEDO_AAX_SDK_PATH "" CACHE PATH "Path to the Avid AAX SDK (enables the AAX build)")

# ---------------------------------------------------------------------------
# Standalone DSP tests — pure C++, no JUCE, no network. Great for CI.
# ---------------------------------------------------------------------------
if(AUTOEDO_BUILD_TESTS)
    enable_testing()

    add_executable(autoedo_tuning_tests tests/TuningTests.cpp)
    target_compile_features(autoedo_tuning_tests PRIVATE cxx_std_17)

    add_executable(autoedo_dsp_tests
        tests/DspTests.cpp
        Source/dsp/YinPitchDetector.cpp
        Source/dsp/PsolaPitchCorrector.cpp)
    target_compile_features(autoedo_dsp_tests PRIVATE cxx_std_17)

    add_test(NAME tuning COMMAND autoedo_tuning_tests)
    add_test(NAME dsp    COMMAND autoedo_dsp_tests)
endif()

# ---------------------------------------------------------------------------
# The plugin itself (JUCE).
# ---------------------------------------------------------------------------
if(AUTOEDO_BUILD_PLUGIN)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.13
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)

    set(AUTOEDO_FORMATS VST3 Standalone)
    if(APPLE)
        list(APPEND AUTOEDO_FORMATS AU)
    endif()

    if(AUTOEDO_AAX_SDK_PATH)
        juce_set_aax_sdk_path("${AUTOEDO_AAX_SDK_PATH}")
        list(APPEND AUTOEDO_FORMATS AAX)
        message(STATUS "AutoEDO: AAX build enabled (SDK: ${AUTOEDO_AAX_SDK_PATH})")
    else()
        message(STATUS "AutoEDO: AAX build disabled (set AUTOEDO_AAX_SDK_PATH to enable)")
    endif()

    juce_add_plugin(AutoEDO
        COMPANY_NAME              "AutoEDO"
        PLUGIN_MANUFACTURER_CODE  Aedo
        PLUGIN_CODE               Aed1
        FORMATS                   ${AUTOEDO_FORMATS}
        PRODUCT_NAME              "AutoEDO"
        DESCRIPTION               "Microtonal (EDO) pitch corrector"
        IS_SYNTH                  FALSE
        NEEDS_MIDI_INPUT          FALSE
        NEEDS_MIDI_OUTPUT         FALSE
        IS_MIDI_EFFECT            FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_STEP          FALSE
        VST3_CATEGORIES           Fx "Pitch Shift"
        AU_MAIN_TYPE              kAudioUnitType_Effect
        AAX_CATEGORY              AAX_ePlugInCategory_PitchShift)

    target_sources(AutoEDO PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/dsp/YinPitchDetector.cpp
        Source/dsp/PsolaPitchCorrector.cpp)

    target_compile_definitions(AutoEDO PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

    target_link_libraries(AutoEDO
        PRIVATE
            juce::juce_audio_utils
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endif()
