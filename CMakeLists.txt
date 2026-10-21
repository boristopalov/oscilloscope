cmake_minimum_required(VERSION 3.24)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/plugin-identity.json")

# The MVP deliberately ships only an Apple Silicon macOS VST3.
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "MVP architecture")
    set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0 CACHE STRING "Minimum macOS")
endif()
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" OSCILLOSCOPE_VERSION LIMIT_COUNT 1)
if(NOT OSCILLOSCOPE_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "VERSION must contain a numeric semantic version (major.minor.patch)")
endif()
project(Oscilloscope VERSION ${OSCILLOSCOPE_VERSION} LANGUAGES C CXX)
if(NOT APPLE OR NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    message(FATAL_ERROR "The MVP targets macOS arm64 only")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(OSCILLOSCOPE_DEV_PROBES "Include development-only host diagnostics" OFF)
set(OSCILLOSCOPE_JUCE_PATH "" CACHE PATH "Existing JUCE source tree (optional)")
if(OSCILLOSCOPE_JUCE_PATH)
    add_subdirectory("${OSCILLOSCOPE_JUCE_PATH}" "${CMAKE_BINARY_DIR}/juce")
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        URL https://codeload.github.com/juce-framework/JUCE/tar.gz/refs/tags/8.0.12
        URL_HASH SHA256=dec1a8baee5aaec4502b717a421b850e257ab7e9c2efd03766185ec1957cf102
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# These identifiers are the compatibility contract with existing DAW projects.
# Keep them stable across versions; packaging reads the same identity file.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/packaging/plugin-identity.json" oscilloscope_identity)
foreach(identity_field company_name bundle_identifier manufacturer_code plugin_code product_name)
    string(JSON OSCILLOSCOPE_${identity_field} GET "${oscilloscope_identity}" ${identity_field})
endforeach()
juce_add_plugin(Oscilloscope
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "${OSCILLOSCOPE_company_name}"
    BUNDLE_ID "${OSCILLOSCOPE_bundle_identifier}"
    PLUGIN_MANUFACTURER_CODE "${OSCILLOSCOPE_manufacturer_code}"
    PLUGIN_CODE "${OSCILLOSCOPE_plugin_code}"
    FORMATS VST3
    PRODUCT_NAME "${OSCILLOSCOPE_product_name}"
    DESCRIPTION "Stereo passthrough with shared stereo capture"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Analyzer)
set(oscilloscope_sources
    Source/CaptureQueue.h Source/SourceRegistry.h Source/SourceRegistry.cpp
    Source/DisplayService.h Source/DisplayService.cpp
    Source/SourceHistory.h Source/TimelineMapper.h Source/ViewerModel.h Source/ViewerModel.cpp
    Source/PresentationClock.h Source/WaveformSurface.h Source/WaveformSurface.mm Source/WaveformShaders.h
    Source/SampleExtrema.h Source/ChannelProjector.h Source/WaveformRenderer.h Source/WaveformRenderer.cpp
    Source/ProbeData.h Source/ProbeService.h Source/ProbeService.cpp
    Source/PluginProcessor.h Source/PluginProcessor.cpp
    Source/PluginEditor.h Source/PluginEditor.cpp Source/ScopeLookAndFeel.h)
# Compile only this translation unit as ARC Objective-C++. Keep JUCE under its
# normal C++ target visibility settings so host/plugin JUCE symbols stay isolated.
set_source_files_properties(Source/WaveformSurface.mm PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-x;objective-c++;-fobjc-arc")
target_sources(Oscilloscope PRIVATE ${oscilloscope_sources})
target_compile_definitions(Oscilloscope PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
    OSCILLOSCOPE_DEV_PROBES=$<BOOL:${OSCILLOSCOPE_DEV_PROBES}>)
target_include_directories(Oscilloscope PUBLIC Source)
target_link_libraries(Oscilloscope PRIVATE
    juce::juce_audio_utils
    juce::juce_recommended_warning_flags
    PUBLIC juce::juce_recommended_config_flags)
# Keep machine-specific source paths out of __FILE__ strings and debug metadata
# in artifacts that may be shared publicly. This also covers JUCE's wrapper.
foreach(scope_target Oscilloscope Oscilloscope_VST3)
    target_compile_options(${scope_target} PRIVATE
        "-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=."
        "-ffile-prefix-map=${CMAKE_BINARY_DIR}=build"
        "-ffile-prefix-map=${JUCE_SOURCE_DIR}=JUCE")
endforeach()
# JUCE's manifest helper adds a resource after its initial signing step. Seal the
# complete local development bundle, including moduleinfo.json, for arm64 loading.
add_custom_command(TARGET Oscilloscope_VST3 POST_BUILD
    COMMAND /usr/bin/codesign --force --sign - "$<TARGET_BUNDLE_DIR:Oscilloscope_VST3>"
    VERBATIM)

include(CTest)
if(BUILD_TESTING)
    juce_add_console_app(OscilloscopeRenderingTests PRODUCT_NAME "Oscilloscope rendering tests")
    target_sources(OscilloscopeRenderingTests PRIVATE Tests/RenderingTests.cpp ${oscilloscope_sources})
    target_include_directories(OscilloscopeRenderingTests PRIVATE Source)
    target_compile_definitions(OscilloscopeRenderingTests PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
        OSCILLOSCOPE_DEV_PROBES=$<BOOL:${OSCILLOSCOPE_DEV_PROBES}>)
    target_link_libraries(OscilloscopeRenderingTests PRIVATE juce::juce_audio_utils
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    add_test(NAME milestone4 COMMAND OscilloscopeRenderingTests)

    juce_add_console_app(OscilloscopeCaptureTests PRODUCT_NAME "Oscilloscope capture tests")
    target_sources(OscilloscopeCaptureTests PRIVATE Tests/CaptureTests.cpp ${oscilloscope_sources})
    target_include_directories(OscilloscopeCaptureTests PRIVATE Source)
    target_compile_definitions(OscilloscopeCaptureTests PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
        OSCILLOSCOPE_DEV_PROBES=$<BOOL:${OSCILLOSCOPE_DEV_PROBES}>)
    target_link_libraries(OscilloscopeCaptureTests PRIVATE juce::juce_audio_utils
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    add_test(NAME milestone2 COMMAND OscilloscopeCaptureTests)

    juce_add_console_app(OscilloscopeTimelineTests PRODUCT_NAME "Oscilloscope timing tests")
    target_sources(OscilloscopeTimelineTests PRIVATE Tests/TimelineTests.cpp ${oscilloscope_sources})
    target_include_directories(OscilloscopeTimelineTests PRIVATE Source)
    target_compile_definitions(OscilloscopeTimelineTests PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
        OSCILLOSCOPE_DEV_PROBES=$<BOOL:${OSCILLOSCOPE_DEV_PROBES}>)
    target_link_libraries(OscilloscopeTimelineTests PRIVATE juce::juce_audio_utils
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    add_test(NAME milestone3 COMMAND OscilloscopeTimelineTests)
endif()
if(BUILD_TESTING AND OSCILLOSCOPE_DEV_PROBES)
    juce_add_console_app(OscilloscopeTests PRODUCT_NAME "Oscilloscope tests")
    target_sources(OscilloscopeTests PRIVATE Tests/ProbeTests.cpp ${oscilloscope_sources})
    target_include_directories(OscilloscopeTests PRIVATE Source)
    target_compile_definitions(OscilloscopeTests PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 OSCILLOSCOPE_DEV_PROBES=1)
    target_link_libraries(OscilloscopeTests PRIVATE juce::juce_audio_utils
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    add_test(NAME milestone1 COMMAND OscilloscopeTests)

    # Load the actual .vst3, rather than testing discovery only in linked shared code.
    juce_add_console_app(OscilloscopeModuleTests PRODUCT_NAME "Oscilloscope module tests")
    target_sources(OscilloscopeModuleTests PRIVATE Tests/ModuleTests.cpp)
    target_compile_definitions(OscilloscopeModuleTests PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1)
    target_link_libraries(OscilloscopeModuleTests PRIVATE juce::juce_audio_utils)
    add_dependencies(OscilloscopeModuleTests Oscilloscope_VST3)
    add_test(NAME vst3_module COMMAND OscilloscopeModuleTests
        "$<TARGET_BUNDLE_DIR:Oscilloscope_VST3>")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME probe_scripts COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s Tests -p "test_*.py" -v)
    set_tests_properties(probe_scripts PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

# Native display pacing uses CVDisplayLink host timestamps (macOS 12+).
foreach(scope_target Oscilloscope OscilloscopeRenderingTests OscilloscopeCaptureTests OscilloscopeTimelineTests OscilloscopeTests)
    if(TARGET ${scope_target})
        target_link_libraries(${scope_target} PRIVATE "-framework CoreVideo")
    endif()
endforeach()
