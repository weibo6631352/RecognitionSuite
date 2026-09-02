cmake_minimum_required(VERSION 3.25)
project(VoiceEngine VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "VoiceEngine first version is Windows x64 only")
endif()

include("${CMAKE_SOURCE_DIR}/cmake/Windows.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/FFmpeg.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/LlamaCpp.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/CudaRuntime.cmake")

set(VOICEENGINE_CORE_SOURCES
    src/api/voiceengine_api.cpp
    src/api/voiceengine_stream.cpp
    src/core/utf.cpp
    src/core/runtime.cpp
    src/core/cuda_check.cpp
    src/core/normalize.cpp
    src/core/slicer.cpp
    src/core/json_result.cpp
    src/core/engine.cpp
    src/audio/wav_reader.cpp
    src/audio/decode.cpp
    src/audio/ffmpeg/ffmpeg_decoder.cpp
    src/backend/llamacpp/llamacpp_backend.cpp)

add_library(VoiceEngineCore SHARED ${VOICEENGINE_CORE_SOURCES})
target_compile_definitions(VoiceEngineCore PRIVATE VOICEENGINE_CORE_EXPORTS)
target_include_directories(VoiceEngineCore
    PUBLIC "${CMAKE_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_SOURCE_DIR}/src"
            "${VOICEENGINE_LLAMA_ROOT}/include"
            "${VOICEENGINE_LLAMA_ROOT}/ggml/include"
            "${VOICEENGINE_LLAMA_ROOT}/tools/mtmd")
target_link_libraries(VoiceEngineCore PRIVATE voiceengine_ffmpeg llama mtmd)
target_compile_options(VoiceEngineCore PRIVATE /W3)
add_custom_command(TARGET VoiceEngineCore POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:VoiceEngineCore>/runtimes/ffmpeg"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:VoiceEngineCore>/runtimes/cuda"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${VOICEENGINE_FFMPEG_DLLS}
        "$<TARGET_FILE_DIR:VoiceEngineCore>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${VOICEENGINE_FFMPEG_DLLS}
        "$<TARGET_FILE_DIR:VoiceEngineCore>/runtimes/ffmpeg"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${VOICEENGINE_CUDA_RUNTIME_DLLS}
        "$<TARGET_FILE_DIR:VoiceEngineCore>/runtimes/cuda")

add_library(VoiceEngineRuntime STATIC src/app/runtime_bootstrap.cpp)
target_include_directories(VoiceEngineRuntime PUBLIC
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src/app")
target_compile_options(VoiceEngineRuntime PRIVATE /W4)

add_executable(VoiceEngineCli src/cli/main.cpp)
target_link_libraries(VoiceEngineCli PRIVATE VoiceEngineCore VoiceEngineRuntime)
target_include_directories(VoiceEngineCli PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src/app")

add_executable(VoiceEngine WIN32
    src/ui/main.cpp
    src/ui/main_window.cpp
    src/ui/drop_zone.cpp
    src/ui/mic_capture.cpp
    resources/resources.qrc)
set_target_properties(VoiceEngine PROPERTIES AUTOMOC ON AUTORCC ON)
target_include_directories(VoiceEngine PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src/app"
    "${CMAKE_SOURCE_DIR}/src/ui")
target_link_libraries(VoiceEngine PRIVATE
    VoiceEngineCore VoiceEngineRuntime Qt5::Widgets ole32)

# The executable-owned bootstrap must run before Windows resolves the CUDA and
# FFmpeg imports of VoiceEngineCore.dll.
foreach(_voiceengine_app IN ITEMS VoiceEngine VoiceEngineCli)
    target_link_options(${_voiceengine_app} PRIVATE "/DELAYLOAD:VoiceEngineCore.dll")
    target_link_libraries(${_voiceengine_app} PRIVATE delayimp)
endforeach()
include("${CMAKE_SOURCE_DIR}/cmake/Deploy.cmake")
