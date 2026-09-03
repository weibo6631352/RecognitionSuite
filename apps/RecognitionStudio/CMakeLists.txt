cmake_minimum_required(VERSION 3.25)

project(RecognitionStudio VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "RecognitionStudio requires Windows x64 and MSVC")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CONFIGURATION_TYPES Release CACHE STRING "Supported configurations" FORCE)
add_compile_options(/utf-8 /W4 /permissive-)

set(VOICEENGINE_SDK_ROOT "" CACHE PATH "Path to the standalone VoiceEngine SDK")
set(SCANENGINE_SDK_ROOT "" CACHE PATH "Path to the standalone ScanEngine SDK")

if(NOT VOICEENGINE_SDK_ROOT)
  set(_voiceengine_candidates
    "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/VoiceEngine/windows-x64")
    foreach(_candidate IN LISTS _voiceengine_candidates)
        if(EXISTS "${_candidate}/cmake/VoiceEngineConfig.cmake")
            set(VOICEENGINE_SDK_ROOT "${_candidate}" CACHE PATH
                "Path to the standalone VoiceEngine SDK" FORCE)
            break()
        endif()
    endforeach()
endif()

if(NOT SCANENGINE_SDK_ROOT)
  set(_scanengine_candidates
    "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/ScanEngine/windows-x64")
    foreach(_candidate IN LISTS _scanengine_candidates)
        if(EXISTS "${_candidate}/cmake/ScanEngineConfig.cmake")
            set(SCANENGINE_SDK_ROOT "${_candidate}" CACHE PATH
                "Path to the standalone ScanEngine SDK" FORCE)
            break()
        endif()
    endforeach()
endif()

foreach(_sdk_root VOICEENGINE_SDK_ROOT SCANENGINE_SDK_ROOT)
    if(NOT DEFINED ${_sdk_root} OR "${${_sdk_root}}" STREQUAL "")
        message(FATAL_ERROR "${_sdk_root} is required")
    endif()
endforeach()

set(_voice_sdk_checksums "${VOICEENGINE_SDK_ROOT}/SHA256SUMS.txt")
set(_scan_sdk_checksums "${SCANENGINE_SDK_ROOT}/SHA256SUMS.txt")
set(_voice_sdk_manifest "${VOICEENGINE_SDK_ROOT}/SDK_MANIFEST.json")
set(_scan_sdk_manifest "${SCANENGINE_SDK_ROOT}/SDK_MANIFEST.json")
foreach(_checksums_file _voice_sdk_checksums _scan_sdk_checksums)
    if(NOT EXISTS "${${_checksums_file}}")
        message(FATAL_ERROR
            "Published SDK checksum manifest is missing: ${${_checksums_file}}")
    endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_voice_sdk_checksums}"
    "${_scan_sdk_checksums}"
    "${_voice_sdk_manifest}"
    "${_scan_sdk_manifest}")
file(SHA256 "${_voice_sdk_checksums}" _voice_sdk_checksums_sha256)
file(SHA256 "${_scan_sdk_checksums}" _scan_sdk_checksums_sha256)
file(SHA256 "${_voice_sdk_manifest}" _voice_sdk_manifest_sha256)
file(SHA256 "${_scan_sdk_manifest}" _scan_sdk_manifest_sha256)
set(_runtime_baseline_definitions
    VOICEENGINE_SDK_CHECKSUMS_SHA256="${_voice_sdk_checksums_sha256}"
    SCANENGINE_SDK_CHECKSUMS_SHA256="${_scan_sdk_checksums_sha256}"
    VOICEENGINE_SDK_MANIFEST_SHA256="${_voice_sdk_manifest_sha256}"
    SCANENGINE_SDK_MANIFEST_SHA256="${_scan_sdk_manifest_sha256}")

find_package(Qt5 5.12.9 EXACT REQUIRED COMPONENTS Widgets)
find_package(VoiceEngine CONFIG REQUIRED
    PATHS "${VOICEENGINE_SDK_ROOT}/cmake" NO_DEFAULT_PATH)
find_package(ScanEngine CONFIG REQUIRED
    PATHS "${SCANENGINE_SDK_ROOT}/cmake" NO_DEFAULT_PATH)

add_executable(RecognitionStudioScanWorker
    src/scan_worker_main.cpp)
target_link_libraries(RecognitionStudioScanWorker PRIVATE
    Qt5::Core
    ScanEngine::CXX)

add_executable(RecognitionStudio WIN32
    src/main.cpp
    src/main_window.cpp
    src/main_window.h
    src/acceptance_evidence_panel.cpp
    src/acceptance_evidence_panel.h
    src/acceptance_validation.cpp
    src/acceptance_validation.h
    src/mic_capture.cpp
    src/mic_capture.h
    resources/resources.qrc)

target_link_libraries(RecognitionStudio PRIVATE
    Qt5::Widgets
    VoiceEngine::CXX
    ole32)
target_compile_definitions(RecognitionStudio PRIVATE
    ${_runtime_baseline_definitions})
add_dependencies(RecognitionStudio RecognitionStudioScanWorker)

include(CTest)
if(BUILD_TESTING)
    add_executable(RecognitionStudioAcceptanceTests
        tests/acceptance_validation_test.cpp
        src/acceptance_validation.cpp
        src/acceptance_validation.h)
    target_include_directories(RecognitionStudioAcceptanceTests PRIVATE src)
    target_compile_definitions(RecognitionStudioAcceptanceTests PRIVATE
        RECOGNITION_SUITE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_link_libraries(RecognitionStudioAcceptanceTests PRIVATE
        Qt5::Core Qt5::Gui)
    set_target_properties(RecognitionStudioAcceptanceTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME RecognitionStudioAcceptanceTests
        COMMAND $<TARGET_FILE:RecognitionStudioAcceptanceTests>)

    add_executable(RecognitionStudioUiTests
        tests/main_window_test.cpp
        src/main_window.cpp
        src/main_window.h
        src/acceptance_evidence_panel.cpp
        src/acceptance_evidence_panel.h
        src/acceptance_validation.cpp
        src/acceptance_validation.h
        src/mic_capture.cpp
        src/mic_capture.h)
    target_include_directories(RecognitionStudioUiTests PRIVATE src)
    target_link_libraries(RecognitionStudioUiTests PRIVATE
        Qt5::Widgets VoiceEngine::CXX ole32)
    target_compile_definitions(RecognitionStudioUiTests PRIVATE
        ${_runtime_baseline_definitions})
    set_target_properties(RecognitionStudioUiTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME RecognitionStudioUiTests
        COMMAND $<TARGET_FILE:RecognitionStudioUiTests>)
    add_dependencies(RecognitionStudioUiTests RecognitionStudioScanWorker)

    add_test(NAME RecognitionStudioScanWorkerCliTest
        COMMAND $<TARGET_FILE:RecognitionStudioScanWorker> --help)
    set_tests_properties(RecognitionStudioScanWorkerCliTest PROPERTIES
        PASS_REGULAR_EXPRESSION "RecognitionStudio isolated ScanEngine worker")
endif()

set_target_properties(
    RecognitionStudio
    RecognitionStudioScanWorker
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")

set(_studio_bin_dir "${CMAKE_BINARY_DIR}/bin")
set(_voice_model_materializer
    "${VOICEENGINE_SDK_ROOT}/tools/materialize-voice-model.ps1")
if(NOT EXISTS "${_voice_model_materializer}")
    message(FATAL_ERROR
        "VoiceEngine SDK model materializer is missing: ${_voice_model_materializer}")
endif()
find_program(_powershell_executable NAMES powershell.exe REQUIRED)

# The SDK payloads are about 9.6 GiB. Deploy them only when a committed SDK
# manifest changes, not after every RecognitionStudio relink.
set(_sdk_runtime_stamp "${_studio_bin_dir}/components/.sdk-runtime.stamp")
set(_voice_runtime_model_dir
    "${_studio_bin_dir}/components/voiceengine/models/qwen3-asr-1.7b")
set(_voice_runtime_core
    "${_studio_bin_dir}/components/voiceengine/VoiceEngineCore.dll")
set(_scan_runtime_core
    "${_studio_bin_dir}/components/scanengine/ScanEngineCore.dll")
set(_voice_runtime_sdk_manifest
    "${_studio_bin_dir}/components/voiceengine/SDK_MANIFEST.json")
set(_voice_runtime_sdk_checksums
    "${_studio_bin_dir}/components/voiceengine/SDK_SHA256SUMS.txt")
set(_scan_runtime_sdk_manifest
    "${_studio_bin_dir}/components/scanengine/SDK_MANIFEST.json")
set(_scan_runtime_sdk_checksums
    "${_studio_bin_dir}/components/scanengine/SDK_SHA256SUMS.txt")
set(_voice_runtime_model
    "${_voice_runtime_model_dir}/Qwen3-ASR-1.7B-bf16.gguf")
add_custom_command(
    OUTPUT
        "${_sdk_runtime_stamp}"
        "${_voice_runtime_core}"
        "${_scan_runtime_core}"
        "${_voice_runtime_sdk_manifest}"
        "${_voice_runtime_sdk_checksums}"
        "${_scan_runtime_sdk_manifest}"
        "${_scan_runtime_sdk_checksums}"
        "${_voice_runtime_model}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf
        "${_studio_bin_dir}/components/voiceengine"
        "${_studio_bin_dir}/components/scanengine"
        "${_studio_bin_dir}/licenses/voiceengine"
        "${_studio_bin_dir}/licenses/scanengine"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${_studio_bin_dir}/components/voiceengine"
        "${_studio_bin_dir}/components/scanengine"
        "${_studio_bin_dir}/licenses/voiceengine"
        "${_studio_bin_dir}/licenses/scanengine"
        "${_studio_bin_dir}/output"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${VOICEENGINE_SDK_ROOT}/bin"
        "${_studio_bin_dir}/components/voiceengine"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${VOICEENGINE_SDK_ROOT}/SDK_MANIFEST.json"
        "${_voice_runtime_sdk_manifest}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${VOICEENGINE_SDK_ROOT}/SHA256SUMS.txt"
        "${_voice_runtime_sdk_checksums}"
    COMMAND "${_powershell_executable}"
        -NoProfile -NonInteractive -ExecutionPolicy Bypass
        -File "${_voice_model_materializer}"
        -ModelDirectory "${_voice_runtime_model_dir}"
        -RemoveParts
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${SCANENGINE_SDK_ROOT}/bin"
        "${_studio_bin_dir}/components/scanengine"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SCANENGINE_SDK_ROOT}/SDK_MANIFEST.json"
        "${_scan_runtime_sdk_manifest}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SCANENGINE_SDK_ROOT}/SHA256SUMS.txt"
        "${_scan_runtime_sdk_checksums}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${VOICEENGINE_SDK_ROOT}/licenses"
        "${_studio_bin_dir}/licenses/voiceengine"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${SCANENGINE_SDK_ROOT}/licenses"
        "${_studio_bin_dir}/licenses/scanengine"
    COMMAND "${CMAKE_COMMAND}" -E touch
        "${_sdk_runtime_stamp}"
        "${_voice_runtime_core}"
        "${_scan_runtime_core}"
        "${_voice_runtime_model}"
    DEPENDS
        "${VOICEENGINE_SDK_ROOT}/SDK_MANIFEST.json"
        "${VOICEENGINE_SDK_ROOT}/SHA256SUMS.txt"
        "${SCANENGINE_SDK_ROOT}/SDK_MANIFEST.json"
        "${SCANENGINE_SDK_ROOT}/SHA256SUMS.txt"
        "${_voice_model_materializer}"
    COMMENT "Deploying committed engine SDK runtimes"
    VERBATIM)
add_custom_target(RecognitionStudioSdkRuntime ALL
    DEPENDS
        "${_sdk_runtime_stamp}"
        "${_voice_runtime_core}"
        "${_scan_runtime_core}"
        "${_voice_runtime_sdk_manifest}"
        "${_voice_runtime_sdk_checksums}"
        "${_scan_runtime_sdk_manifest}"
        "${_scan_runtime_sdk_checksums}"
        "${_voice_runtime_model}")
add_dependencies(RecognitionStudio RecognitionStudioSdkRuntime)
add_dependencies(RecognitionStudioScanWorker RecognitionStudioSdkRuntime)
if(BUILD_TESTING)
    add_dependencies(RecognitionStudioUiTests RecognitionStudioSdkRuntime)
endif()

get_target_property(_qt_qmake Qt5::qmake IMPORTED_LOCATION)
get_filename_component(_qt_bin_dir "${_qt_qmake}" DIRECTORY)
set(_qt_deploy_tool "${_qt_bin_dir}/windeployqt.exe")
if(NOT EXISTS "${_qt_deploy_tool}")
    message(FATAL_ERROR "windeployqt was not found: ${_qt_deploy_tool}")
endif()

set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
include(InstallRequiredSystemLibraries)

# Each SDK keeps its own runtime root. Both bootstrap libraries register these
# directories before their delay-loaded Core DLL is touched. The large SDK
# trees are deployed by RecognitionStudioSdkRuntime above; this post-build step
# only refreshes the application-owned runtime.
add_custom_command(TARGET RecognitionStudio POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:RecognitionStudio>/output"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
        "$<TARGET_FILE_DIR:RecognitionStudio>"
    COMMAND "${_qt_deploy_tool}"
        --release --no-translations --no-compiler-runtime
        --dir "$<TARGET_FILE_DIR:RecognitionStudio>"
        "$<TARGET_FILE:RecognitionStudio>"
    COMMENT "Deploying the RecognitionStudio application runtime"
    VERBATIM)
