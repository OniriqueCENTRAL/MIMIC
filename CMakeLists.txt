cmake_minimum_required(VERSION 3.25)
project(MIMIC VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- Steinberg VST3 SDK -------------------------------------------------------
# Option 1 : -DMIMIC_SDK_DIR=C:/chemin/vst3sdk (SDK déjà cloné, avec ses sous-modules)
# Option 2 : récupération automatique (FetchContent)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "")
if(WIN32)
  set(SMTG_USE_STATIC_CRT ON CACHE BOOL "")   # WebView2LoaderStatic.lib est construite en /MT
endif()
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "")
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "")
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "")   # évite d'exiger les droits admin sous Windows

if(MIMIC_SDK_DIR)
  add_subdirectory(${MIMIC_SDK_DIR} ${CMAKE_BINARY_DIR}/vst3sdk)
else()
  include(FetchContent)
  FetchContent_Declare(vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG        v3.8.1_build_84 # ajuster au tag SDK souhaité
    GIT_SUBMODULES base cmake pluginterfaces public.sdk
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(vst3sdk)
endif()

smtg_enable_vst3_sdk()

# --- Plugin -------------------------------------------------------------------
smtg_add_vst3plugin(MIMIC
  source/entry.cpp
  source/audio_file.cpp
  source/audio_file.h
  source/dsp/fileplayer.h
  source/processor.cpp
  source/processor.h
  source/controller.cpp
  source/controller.h
  source/ids.h
  source/version.h
  source/dsp/mimic_dsp.h)

smtg_target_configure_version_file(MIMIC)

# --- Éditeur graphique ---------------------------------------------------------
target_sources(MIMIC PRIVATE source/editor.h source/editor_bridge.h source/editor_bridge.cpp)
if(WIN32)
  # Microsoft Edge WebView2 (SDK récupéré depuis NuGet, aucune installation manuelle)
  include(FetchContent)
  FetchContent_Declare(webview2
    URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2739.15
    DOWNLOAD_NAME microsoft.web.webview2.zip)
  FetchContent_MakeAvailable(webview2)
  if(CMAKE_GENERATOR_PLATFORM MATCHES "ARM64|arm64")
    set(MIMIC_WV2_ARCH arm64)
  else()
    set(MIMIC_WV2_ARCH x64)
  endif()
  set(MIMIC_WV2 ${webview2_SOURCE_DIR}/build/native)
  target_sources(MIMIC PRIVATE source/editor_win.cpp resource/mimic.rc)
  target_include_directories(MIMIC PRIVATE ${MIMIC_WV2}/include)
  target_link_libraries(MIMIC PRIVATE ${MIMIC_WV2}/${MIMIC_WV2_ARCH}/WebView2LoaderStatic.lib version advapi32 ole32 shell32 user32 gdi32)
  set_source_files_properties(resource/mimic.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/ui/mimic_ui.html)
else()
  target_sources(MIMIC PRIVATE source/editor_other.cpp)
endif()
target_include_directories(MIMIC PRIVATE source)
target_link_libraries(MIMIC PRIVATE sdk)

if(MSVC)
  target_compile_options(MIMIC PRIVATE $<$<CONFIG:Release>:/O2 /arch:AVX2 /fp:fast>)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64|ARM64")
  target_compile_options(MIMIC PRIVATE $<$<CONFIG:Release>:-O3 -ffast-math>)
else()
  target_compile_options(MIMIC PRIVATE $<$<CONFIG:Release>:-O3 -mavx2 -mfma -ffast-math>)
endif()

# --- Tests DSP hors-plugin ----------------------------------------------------
option(MIMIC_BUILD_TESTS "Compiler les tests DSP" ON)
if(MIMIC_BUILD_TESTS)
  add_executable(mimic_dsp_test test/dsp_test.cpp)
  target_include_directories(mimic_dsp_test PRIVATE source)
  if(MSVC)
    target_compile_options(mimic_dsp_test PRIVATE /O2 /arch:AVX2)
  else()
    target_compile_options(mimic_dsp_test PRIVATE -O2 -mavx2)
  endif()
endif()

# --- Test de bout en bout de la source fichier (Processor sans hôte) -------------
if(MIMIC_BUILD_TESTS AND TARGET sdk_hosting)
  add_executable(mimic_file_test test/file_test.cpp source/processor.cpp source/audio_file.cpp ${vstsdk_SOURCE_DIR}/public.sdk/source/common/memorystream.cpp)
  target_include_directories(mimic_file_test PRIVATE source)
  target_link_libraries(mimic_file_test PRIVATE sdk sdk_hosting sdk_common)
endif()

if(MIMIC_BUILD_TESTS AND TARGET sdk_hosting)
  add_executable(mimic_bridge_test test/bridge_test.cpp source/controller.cpp source/editor_bridge.cpp source/editor_other.cpp)
  target_include_directories(mimic_bridge_test PRIVATE source)
  target_link_libraries(mimic_bridge_test PRIVATE sdk sdk_common)
endif()
