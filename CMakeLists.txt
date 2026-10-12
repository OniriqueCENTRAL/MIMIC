cmake_minimum_required(VERSION 3.22)

project(Mimic VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    # Universal binary. 10.15 is the first macOS with std::filesystem, which the file loader uses.
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# JUCE is looked for next to this file first, then in the PitchBox checkout
# beside this one, so the plugins of the range can share a single copy.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    set(MIMIC_JUCE_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/JUCE")
else()
    set(MIMIC_JUCE_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../PitchBox/JUCE")
endif()

set(MIMIC_JUCE_DIR "${MIMIC_JUCE_DEFAULT}" CACHE PATH "Path to JUCE")

if(NOT EXISTS "${MIMIC_JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE not found at ${MIMIC_JUCE_DIR}.\n"
        "Run:  git clone --depth 1 --branch 9.0.3 https://github.com/juce-framework/JUCE.git")
endif()

add_subdirectory("${MIMIC_JUCE_DIR}" JUCE EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The engine (header-only, in Source/dsp) and the audio file loader, built on
# their own so the offline tests can link them without dragging in JUCE.
add_library(mimic_core STATIC
    Source/audio_file.cpp)

target_include_directories(mimic_core PUBLIC Source)
set_target_properties(mimic_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    # The sources are UTF-8, and MSVC has to be told.
    target_compile_options(mimic_core PUBLIC /utf-8)
endif()

# ---------------------------------------------------------------------------
# The plugin.
set(MIMIC_FORMATS VST3)

if(APPLE)
    list(APPEND MIMIC_FORMATS AU)
endif()

# The system VST3 folder on Windows is under Program Files, which a normal build
# cannot write to, so installing after the build is opt-in there.
if(WIN32)
    option(MIMIC_COPY_AFTER_BUILD "Install the plugin after building" OFF)
else()
    option(MIMIC_COPY_AFTER_BUILD "Install the plugin after building" ON)
endif()

juce_add_plugin(Mimic
    COMPANY_NAME "ONIRIQUE x Abrasion"
    BUNDLE_ID "com.onirique.abrasion.mimic"
    PLUGIN_MANUFACTURER_CODE Onrq
    PLUGIN_CODE Mimc
    FORMATS ${MIMIC_FORMATS}
    PRODUCT_NAME "MIMIC"
    PLUGIN_NAME "MIMIC"
    # An instrument: notes come in by MIDI, and the sidechain is its only audio input.
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    # Space freezes, the arrows turn the seal under the pointer.
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES Instrument Synth
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    COPY_PLUGIN_AFTER_BUILD ${MIMIC_COPY_AFTER_BUILD})

# The scenery of the interface: photographs of the original page, layer by layer (tools/bake_ui.py).
juce_add_binary_data(MimicAssets SOURCES
    Resources/ui/under.png
    Resources/ui/over.png
    Resources/ui/over_on.png)

target_sources(Mimic PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

# The licence every Onirique plugin carries: the folder is copied as it is from
# plugin-license/ in the ONIRIQUE CENTRAL repository, and is only ever changed there.
include(License/OniriqueLicense.cmake)
onirique_add_license(Mimic)

target_compile_definitions(Mimic PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)

target_link_libraries(Mimic
    PRIVATE
        mimic_core
        MimicAssets
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Offline test: the whole engine without a host. It leaves two demo files in
# the build folder: the sidechain it used and what MIMIC made of it.
enable_testing()

add_executable(mimic_dsp_test tests/dsp_test.cpp)
target_link_libraries(mimic_dsp_test PRIVATE mimic_core)
add_test(NAME dsp COMMAND mimic_dsp_test "${CMAKE_BINARY_DIR}")

# A development tool, not part of the product: renders the editor to a PNG
# without a host, and puts the plugin through what a host does to it.
# Built only when asked for by name.
add_executable(mimic_snapshot EXCLUDE_FROM_ALL tools/snapshot.cpp)

target_include_directories(mimic_snapshot PRIVATE
    $<TARGET_PROPERTY:Mimic,INCLUDE_DIRECTORIES>)

target_compile_definitions(mimic_snapshot PRIVATE
    $<TARGET_PROPERTY:Mimic,COMPILE_DEFINITIONS>)

target_link_libraries(mimic_snapshot PRIVATE Mimic mimic_core MimicAssets)
