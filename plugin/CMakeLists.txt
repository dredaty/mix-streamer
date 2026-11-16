cmake_minimum_required(VERSION 3.22)

# macOS: Universal Binary (Apple Silicon + Intel). Must be set before project().
if(APPLE)
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" FORCE)
    endif()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
endif()

project(MixStreamer VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows: static CRT so the .vst3 has no VC++ redistributable dependency.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(MIXSTREAMER_COPY_PLUGIN "Copy the built VST3 into the system plug-in folder" OFF)

# ------------------------------------------------------------------ JUCE
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)
FetchContent_MakeAvailable(JUCE)

# ------------------------------------------------------------------ plug-in
juce_add_plugin(MixStreamer
    COMPANY_NAME                "MixStreamer"
    BUNDLE_ID                   "com.mixstreamer.plugin"
    PLUGIN_MANUFACTURER_CODE    Mxsr
    PLUGIN_CODE                 MxSt
    FORMATS                     VST3
    PRODUCT_NAME                "Mix Streamer"
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES             Fx Tools
    COPY_PLUGIN_AFTER_BUILD     ${MIXSTREAMER_COPY_PLUGIN})

target_sources(MixStreamer PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/StreamSender.cpp
    Source/WebSocketClient.cpp)

target_compile_definitions(MixStreamer PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(MixStreamer
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ------------------------------------------------------------------ optional headless test
option(MIXSTREAMER_BUILD_TESTS "Build headless end-to-end test" OFF)
if(MIXSTREAMER_BUILD_TESTS)
    juce_add_console_app(MixStreamerHeadlessTest PRODUCT_NAME "MixStreamerHeadlessTest")
    target_sources(MixStreamerHeadlessTest PRIVATE
        tests/HeadlessTest.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/StreamSender.cpp
        Source/WebSocketClient.cpp)
    target_compile_definitions(MixStreamerHeadlessTest PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JucePlugin_Name="Mix Streamer")
    target_link_libraries(MixStreamerHeadlessTest PRIVATE
        juce::juce_audio_utils juce::juce_recommended_config_flags)
endif()
