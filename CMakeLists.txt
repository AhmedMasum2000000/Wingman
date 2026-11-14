cmake_minimum_required(VERSION 3.22)
project(Backline VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # no VC++ redistributable needed

option(BACKLINE_BUILD_PLUGIN "Build the JUCE plugin (downloads JUCE)" ON)

add_library(backline_core STATIC
    core/Analysis.cpp core/Harmony.cpp core/Parts.cpp core/MidiFile.cpp
    core/Timing.cpp core/Variation.cpp core/Patterns.cpp)
target_include_directories(backline_core PUBLIC core)
set_target_properties(backline_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

enable_testing()
add_executable(backline_tests tests/CoreTests.cpp)
target_link_libraries(backline_tests PRIVATE backline_core)
add_test(NAME core COMMAND backline_tests)
add_executable(backline_engine_tests tests/EngineTests.cpp)
target_link_libraries(backline_engine_tests PRIVATE backline_core)
add_test(NAME engine COMMAND backline_engine_tests)

if(BACKLINE_BUILD_PLUGIN)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)

    if(APPLE)
        set(BACKLINE_FORMATS VST3 AU Standalone)
    else()
        set(BACKLINE_FORMATS VST3 Standalone)
    endif()

    juce_add_plugin(Backline
        COMPANY_NAME "ACND"
        PLUGIN_MANUFACTURER_CODE Bkln
        PLUGIN_CODE Bkl1
        FORMATS ${BACKLINE_FORMATS}
        PRODUCT_NAME "M1D1"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT TRUE
        COPY_PLUGIN_AFTER_BUILD FALSE)

    juce_add_binary_data(BacklineAssets SOURCES
        assets/SpaceGrotesk-Bold.ttf assets/SpaceGrotesk-Medium.ttf
        assets/Inter-Medium.ttf assets/Inter-SemiBold.ttf)
    set_target_properties(BacklineAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

    target_sources(Backline PRIVATE plugin/PluginProcessor.cpp plugin/PluginEditor.cpp plugin/Instruments.cpp)
    target_compile_definitions(Backline PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(Backline
        PRIVATE backline_core BacklineAssets juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    juce_add_console_app(backline_plugin_tests)
    target_sources(backline_plugin_tests PRIVATE
        tests/PluginTests.cpp plugin/PluginProcessor.cpp plugin/PluginEditor.cpp plugin/Instruments.cpp)
    target_compile_definitions(backline_plugin_tests PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(backline_plugin_tests PRIVATE backline_core BacklineAssets juce::juce_audio_utils)
    add_test(NAME plugin COMMAND backline_plugin_tests)

    juce_add_console_app(backline_screenshot)
    target_sources(backline_screenshot PRIVATE
        tests/Screenshot.cpp plugin/PluginProcessor.cpp plugin/PluginEditor.cpp plugin/Instruments.cpp)
    target_compile_definitions(backline_screenshot PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(backline_screenshot PRIVATE backline_core BacklineAssets juce::juce_audio_utils)
endif()
