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
    "${CMAKE_CURRENT_SOURCE_DIR}/../../engines/VoiceEngine/build/win-qt5.12.9-msvc-cuda/sdk")
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
    "${CMAKE_CURRENT_SOURCE_DIR}/../../engines/ScanEngine/build/win-qt5.12.9-msvc-mlx-cuda/sdk")
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

find_package(Qt5 5.12.9 EXACT REQUIRED COMPONENTS Widgets)
find_package(VoiceEngine CONFIG REQUIRED
    PATHS "${VOICEENGINE_SDK_ROOT}/cmake" NO_DEFAULT_PATH)
find_package(ScanEngine CONFIG REQUIRED
    PATHS "${SCANENGINE_SDK_ROOT}/cmake" NO_DEFAULT_PATH)

add_executable(RecognitionStudio WIN32
    src/main.cpp
    src/main_window.cpp
    src/main_window.h
    src/mic_capture.cpp
    src/mic_capture.h
    resources/resources.qrc)

target_link_libraries(RecognitionStudio PRIVATE
    Qt5::Widgets
    VoiceEngine::CXX
    ScanEngine::CXX
    ole32)

set_target_properties(RecognitionStudio PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")

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
# directories before their delay-loaded Core DLL is touched.
add_custom_command(TARGET RecognitionStudio POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E rm -rf
        "$<TARGET_FILE_DIR:RecognitionStudio>/components/voiceengine"
        "$<TARGET_FILE_DIR:RecognitionStudio>/components/scanengine"
        "$<TARGET_FILE_DIR:RecognitionStudio>/licenses/voiceengine"
        "$<TARGET_FILE_DIR:RecognitionStudio>/licenses/scanengine"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:RecognitionStudio>/components/voiceengine"
        "$<TARGET_FILE_DIR:RecognitionStudio>/components/scanengine"
        "$<TARGET_FILE_DIR:RecognitionStudio>/licenses/voiceengine"
        "$<TARGET_FILE_DIR:RecognitionStudio>/licenses/scanengine"
        "$<TARGET_FILE_DIR:RecognitionStudio>/output"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${VOICEENGINE_SDK_ROOT}/bin"
        "$<TARGET_FILE_DIR:RecognitionStudio>/components/voiceengine"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${SCANENGINE_SDK_ROOT}/bin"
        "$<TARGET_FILE_DIR:RecognitionStudio>/components/scanengine"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${VOICEENGINE_SDK_ROOT}/licenses"
        "$<TARGET_FILE_DIR:RecognitionStudio>/licenses/voiceengine"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${SCANENGINE_SDK_ROOT}/licenses"
        "$<TARGET_FILE_DIR:RecognitionStudio>/licenses/scanengine"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
        "$<TARGET_FILE_DIR:RecognitionStudio>"
    COMMAND "${_qt_deploy_tool}"
        --release --no-translations --no-compiler-runtime
        --dir "$<TARGET_FILE_DIR:RecognitionStudio>"
        "$<TARGET_FILE:RecognitionStudio>"
    COMMENT "Deploying the two standalone SDK runtimes and licenses"
    VERBATIM)
