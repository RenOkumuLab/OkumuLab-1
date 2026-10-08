cmake_minimum_required(VERSION 3.22)

# the resource compiler without cmcldeps: its dependency scan passes the localized "Note: including file" prefix
# of cl.exe (Japanese here), which broke rc.exe's command line (RC1107) after a reconfigure
set(CMAKE_NINJA_CMCLDEPS_RC OFF)

# macOS: the oldest system the binaries run on (before project(); -DCMAKE_OSX_DEPLOYMENT_TARGET=... wins). For one
# binary for Apple silicon and Intel Macs, configure with -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" (build.sh does)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")

project(OkumuLab1 VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# static C runtime: the plugin does not need the Visual C++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# sources are UTF-8 (the Windows default code page here is 932)
add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>" "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

option(OKL_WITH_PLUGIN "Build the JUCE plugin (VST3 + Standalone) and the host test" ON)

# ---------------------------------------------------------------- DSP core (no JUCE)
add_library(labium_dsp STATIC
    src/dsp/LabiumCore.cpp
    src/dsp/Engine.cpp
    src/dsp/Params.cpp
    src/dsp/MidiRouter.cpp
    src/dsp/Mallet.cpp
    src/dsp/Bessel.cpp
    src/dsp/Reverb.cpp
    src/dsp/Presets.cpp
    src/dsp/Simd.cpp
    src/dsp/SimdAvx2.cpp)
target_include_directories(labium_dsp PUBLIC src/dsp)
# linked into the plugin's shared module (VST3 .so on Linux)
set_target_properties(labium_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)
# P5: the AVX2 + FMA kernels (x86-64 only, chosen at run time, Simd.cpp); everything else stays SSE2. Other CPUs
# (Apple silicon, ARM Linux) compile SimdAvx2.cpp to the baseline and need no flags.
if(MSVC)
    set_source_files_properties(src/dsp/SimdAvx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
elseif(APPLE)
    # (a universal binary passes the flags to its x86-64 half only)
    if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" OR (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"))
        set_source_files_properties(src/dsp/SimdAvx2.cpp PROPERTIES COMPILE_OPTIONS "-Xarch_x86_64;-mavx2;-Xarch_x86_64;-mfma")
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    set_source_files_properties(src/dsp/SimdAvx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
endif()
if(MSVC)
    target_compile_options(labium_dsp PRIVATE /W4 /fp:precise /O2)
else()
    # no fused multiply-adds the source does not ask for: the same arithmetic as MSVC's /fp:precise
    target_compile_options(labium_dsp PRIVATE -ffp-contract=off)
endif()

# offline checks: port vs Phase 1, pitch lock, CPU, stress, MIDI routing
add_executable(labium_check tests/labium_check.cpp tests/ref/labium_core.c)
target_include_directories(labium_check PRIVATE tests)
target_link_libraries(labium_check PRIVATE labium_dsp)
if(MSVC)
    set_source_files_properties(tests/ref/labium_core.c PROPERTIES COMPILE_OPTIONS "/O2;/fp:precise;/wd4244")
    # the checks keep whole engines (32 voices with their mallet models) on the stack
    target_link_options(labium_check PRIVATE /STACK:33554432)
else()
    target_compile_options(labium_check PRIVATE -ffp-contract=off)
    if(APPLE)
        target_link_options(labium_check PRIVATE "LINKER:-stack_size,0x2000000")
    endif()
    # (Linux: the main thread's stack is the shell's limit; build.sh and the CI raise it with ulimit -s)
endif()

if(NOT OKL_WITH_PLUGIN)
    return()
endif()

# ---------------------------------------------------------------- plugin
# the Phase 4 screen's web view: WebView2 on Windows (SDK = NuGet package Microsoft.Web.WebView2, unpacked here,
# linked statically), WKWebView on macOS, WebKitGTK on Linux (headers at build time, loaded when the screen opens)
if(WIN32)
    set(JUCE_WEBVIEW2_PACKAGE_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nuget")
endif()
add_subdirectory(third_party/JUCE)

# VST3 and the standalone app everywhere; Audio Unit on macOS (Logic Pro, GarageBand)
set(OKL_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND OKL_FORMATS AU)
endif()

# the screen (web/: the Phase 2 page, Three.js, fonts), zipped and embedded in the plugin
set(OKL_WEB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/web")
file(GLOB_RECURSE OKL_WEB_FILES CONFIGURE_DEPENDS "${OKL_WEB_DIR}/*")
set(OKL_WEB_REL "")
foreach(f IN LISTS OKL_WEB_FILES)
    file(RELATIVE_PATH r "${OKL_WEB_DIR}" "${f}")
    list(APPEND OKL_WEB_REL "${r}")
endforeach()
set(OKL_WEB_ZIP "${CMAKE_CURRENT_BINARY_DIR}/okl_web.zip")
add_custom_command(OUTPUT "${OKL_WEB_ZIP}"
    COMMAND ${CMAKE_COMMAND} -E remove -f "${OKL_WEB_ZIP}"
    COMMAND ${CMAKE_COMMAND} -E tar cf "${OKL_WEB_ZIP}" --format=zip -- ${OKL_WEB_REL}
    WORKING_DIRECTORY "${OKL_WEB_DIR}"
    DEPENDS ${OKL_WEB_FILES}
    COMMENT "Zipping the screen (web/)")
juce_add_binary_data(OkumuLab1Web HEADER_NAME OklWebData.h NAMESPACE OklWeb SOURCES "${OKL_WEB_ZIP}")

juce_add_plugin(OkumuLab1
    COMPANY_NAME "OkumuLab"
    COMPANY_WEBSITE ""
    PRODUCT_NAME "OkumuLab 1"
    DESCRIPTION "Labium - Prinzipal 8' flue pipe physical model"
    VERSION 1.0.0
    PLUGIN_MANUFACTURER_CODE OkmL
    PLUGIN_CODE Lbm1
    BUNDLE_ID com.okumulab.okumulab1.labium
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Instrument Synth
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    FORMATS ${OKL_FORMATS}
    NEEDS_WEBVIEW2 TRUE
    NEEDS_WEB_BROWSER TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(OkumuLab1 PRIVATE
    src/plugin/PluginProcessor.cpp
    src/plugin/PluginEditor.cpp
    src/plugin/WebEditor.cpp
    src/plugin/CockpitLookAndFeel.cpp)

target_compile_definitions(OkumuLab1 PUBLIC
    JUCE_WEB_BROWSER=1
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # the standalone app on Linux: JACK (and PipeWire's JACK) as well as ALSA; libjack is loaded at run time
    target_compile_definitions(OkumuLab1 PUBLIC JUCE_JACK=1)
endif()

target_link_libraries(OkumuLab1
    PRIVATE
        labium_dsp
        OkumuLab1Web
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------- host test (loads the built VST3)
juce_add_console_app(okl_hosttest PRODUCT_NAME "okl_hosttest")
target_sources(okl_hosttest PRIVATE tests/host_test.cpp)
target_compile_definitions(okl_hosttest PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    OKL_VST3_PATH="$<TARGET_FILE_DIR:OkumuLab1_VST3>/../.."
    OKL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(okl_hosttest
    PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
add_dependencies(okl_hosttest OkumuLab1_VST3)
