set(VOICEENGINE_DEPLOY_DIR "${CMAKE_SOURCE_DIR}/VoiceEngineWindowsX64"
    CACHE PATH "VoiceEngine portable deployment directory")
get_target_property(_qt_qmake Qt5::qmake IMPORTED_LOCATION)
get_filename_component(_qt_bin_dir "${_qt_qmake}" DIRECTORY)
set(_qt_deploy_tool "${_qt_bin_dir}/windeployqt.exe")
if(NOT EXISTS "${_qt_deploy_tool}")
    message(FATAL_ERROR "windeployqt not found: ${_qt_deploy_tool}")
endif()

set(_qt_conf "${CMAKE_BINARY_DIR}/qt.conf")
file(WRITE "${_qt_conf}" "[Paths]\nPrefix = .\n")

set(_deploy_stamp "${VOICEENGINE_DEPLOY_DIR}/.deploy.stamp")
set(_voiceengine_stale_root_cuda_dlls)
foreach(_cuda_dll IN LISTS VOICEENGINE_CUDA_RUNTIME_DLLS)
    get_filename_component(_cuda_name "${_cuda_dll}" NAME)
    list(APPEND _voiceengine_stale_root_cuda_dlls
        "${VOICEENGINE_DEPLOY_DIR}/${_cuda_name}")
endforeach()
add_custom_command(
    OUTPUT "${_deploy_stamp}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${VOICEENGINE_DEPLOY_DIR}"
        "${VOICEENGINE_DEPLOY_DIR}/runtimes/ffmpeg"
        "${VOICEENGINE_DEPLOY_DIR}/runtimes/cuda"
        "${VOICEENGINE_DEPLOY_DIR}/models/qwen3-asr-1.7b"
        "${VOICEENGINE_DEPLOY_DIR}/licenses/ffmpeg"
        "${VOICEENGINE_DEPLOY_DIR}/licenses/llama.cpp"
        "${VOICEENGINE_DEPLOY_DIR}/output"
        "${VOICEENGINE_BIN_DIR}/runtimes/ffmpeg"
        "${VOICEENGINE_BIN_DIR}/runtimes/cuda"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:VoiceEngine>"
        "$<TARGET_FILE:VoiceEngineCore>"
        "$<TARGET_FILE:VoiceEngineCli>"
        "${CMAKE_SOURCE_DIR}/voiceengine.json"
        "${_qt_conf}"
        "${CMAKE_SOURCE_DIR}/script/windows/启动.bat"
        "${VOICEENGINE_DEPLOY_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${VOICEENGINE_FFMPEG_DLLS}
        "${CMAKE_SOURCE_DIR}/third_party/windows-x64/ffmpeg/LICENSE.txt"
        "${VOICEENGINE_DEPLOY_DIR}/runtimes/ffmpeg"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${VOICEENGINE_FFMPEG_DLLS}
        "${VOICEENGINE_BIN_DIR}/runtimes/ffmpeg"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${VOICEENGINE_FFMPEG_DLLS}
        "${VOICEENGINE_DEPLOY_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${VOICEENGINE_CUDA_RUNTIME_DLLS}
        "${VOICEENGINE_DEPLOY_DIR}/runtimes/cuda"
    COMMAND "${CMAKE_COMMAND}" -E rm -f
        ${_voiceengine_stale_root_cuda_dlls}
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${VOICEENGINE_CUDA_RUNTIME_DLLS}
        "${VOICEENGINE_BIN_DIR}/runtimes/cuda"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
        "${VOICEENGINE_DEPLOY_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/windows-x64/ffmpeg/LICENSE.txt"
        "${VOICEENGINE_DEPLOY_DIR}/licenses/ffmpeg"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/common/llama.cpp/LICENSE"
        "${VOICEENGINE_DEPLOY_DIR}/licenses/llama.cpp"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/models/qwen3-asr-1.7b"
        "${VOICEENGINE_DEPLOY_DIR}/models/qwen3-asr-1.7b"
    COMMAND "${CMAKE_SOURCE_DIR}/script/windows/assemble-model.cmd"
        "${VOICEENGINE_DEPLOY_DIR}/models/qwen3-asr-1.7b"
    COMMAND "${CMAKE_COMMAND}" -E rm -f
        "${VOICEENGINE_DEPLOY_DIR}/models/qwen3-asr-1.7b/Qwen3-ASR-1.7B-bf16.gguf.part1"
        "${VOICEENGINE_DEPLOY_DIR}/models/qwen3-asr-1.7b/Qwen3-ASR-1.7B-bf16.gguf.part2"
    COMMAND "${_qt_deploy_tool}" --release --no-translations --no-compiler-runtime
        --dir "${VOICEENGINE_DEPLOY_DIR}"
        "${VOICEENGINE_DEPLOY_DIR}/$<TARGET_FILE_NAME:VoiceEngine>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:VoiceEngineCore>"
        "$<TARGET_FILE:VoiceEngineCli>"
        ${VOICEENGINE_FFMPEG_DLLS}
        "${VOICEENGINE_BIN_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/models/qwen3-asr-1.7b"
        "${VOICEENGINE_BIN_DIR}/models/qwen3-asr-1.7b"
    COMMAND "${CMAKE_SOURCE_DIR}/script/windows/assemble-model.cmd"
        "${VOICEENGINE_BIN_DIR}/models/qwen3-asr-1.7b"
    COMMAND "${CMAKE_COMMAND}" -E rm -f
        "${VOICEENGINE_BIN_DIR}/models/qwen3-asr-1.7b/Qwen3-ASR-1.7B-bf16.gguf.part1"
        "${VOICEENGINE_BIN_DIR}/models/qwen3-asr-1.7b/Qwen3-ASR-1.7B-bf16.gguf.part2"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_deploy_stamp}"
    DEPENDS VoiceEngine VoiceEngineCore VoiceEngineCli
        "${CMAKE_SOURCE_DIR}/voiceengine.json"
        "${CMAKE_SOURCE_DIR}/script/windows/启动.bat"
        "${CMAKE_SOURCE_DIR}/script/windows/assemble-model.cmd"
    COMMENT "Deploying VoiceEngineWindowsX64"
    VERBATIM)
add_custom_target(deploy-bin ALL DEPENDS "${_deploy_stamp}")

set(_voiceengine_sdk_config "${CMAKE_BINARY_DIR}/VoiceEngineConfig.cmake")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/VoiceEngineSDKConfig.cmake.in"
    "${_voiceengine_sdk_config}" @ONLY)
set(VOICEENGINE_SDK_DIR "${CMAKE_BINARY_DIR}/sdk")
file(GLOB_RECURSE _voiceengine_sdk_model_files CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    "${CMAKE_SOURCE_DIR}/models/qwen3-asr-1.7b/*")
set(_voiceengine_sdk_stamp "${VOICEENGINE_SDK_DIR}/.sdk.stamp")
add_custom_command(
    OUTPUT "${_voiceengine_sdk_stamp}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${VOICEENGINE_SDK_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${VOICEENGINE_SDK_DIR}/bin/runtimes/ffmpeg"
        "${VOICEENGINE_SDK_DIR}/bin/runtimes/cuda"
        "${VOICEENGINE_SDK_DIR}/bin/models/qwen3-asr-1.7b"
        "${VOICEENGINE_SDK_DIR}/include/voiceengine"
        "${VOICEENGINE_SDK_DIR}/lib"
        "${VOICEENGINE_SDK_DIR}/cmake"
        "${VOICEENGINE_SDK_DIR}/doc"
        "${VOICEENGINE_SDK_DIR}/examples"
        "${VOICEENGINE_SDK_DIR}/licenses/ffmpeg"
        "${VOICEENGINE_SDK_DIR}/licenses/llama.cpp"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:VoiceEngineCore>"
        "${CMAKE_SOURCE_DIR}/voiceengine.json"
        ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
        "${VOICEENGINE_SDK_DIR}/bin"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${VOICEENGINE_FFMPEG_DLLS}
        "${VOICEENGINE_SDK_DIR}/bin/runtimes/ffmpeg"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${VOICEENGINE_CUDA_RUNTIME_DLLS}
        "${VOICEENGINE_SDK_DIR}/bin/runtimes/cuda"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/models/qwen3-asr-1.7b"
        "${VOICEENGINE_SDK_DIR}/bin/models/qwen3-asr-1.7b"
    COMMAND "${CMAKE_SOURCE_DIR}/script/windows/assemble-model.cmd"
        "${VOICEENGINE_SDK_DIR}/bin/models/qwen3-asr-1.7b"
    COMMAND "${CMAKE_COMMAND}" -E rm -f
        "${VOICEENGINE_SDK_DIR}/bin/models/qwen3-asr-1.7b/Qwen3-ASR-1.7B-bf16.gguf.part1"
        "${VOICEENGINE_SDK_DIR}/bin/models/qwen3-asr-1.7b/Qwen3-ASR-1.7B-bf16.gguf.part2"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/include/voiceengine/voiceengine_api.h"
        "${CMAKE_SOURCE_DIR}/include/voiceengine/voiceengine.hpp"
        "${CMAKE_SOURCE_DIR}/include/voiceengine/voiceengine_version.h"
        "${CMAKE_SOURCE_DIR}/include/voiceengine/voiceengine_runtime.h"
        "${VOICEENGINE_SDK_DIR}/include/voiceengine"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_LINKER_FILE:VoiceEngineCore>"
        "$<TARGET_FILE:VoiceEngineRuntime>"
        "${VOICEENGINE_SDK_DIR}/lib"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_voiceengine_sdk_config}"
        "${VOICEENGINE_SDK_DIR}/cmake/VoiceEngineConfig.cmake"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/doc/SDK_ABI.md"
        "${VOICEENGINE_SDK_DIR}/doc/SDK_ABI.md"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/examples/sdk_smoke"
        "${VOICEENGINE_SDK_DIR}/examples/sdk_smoke"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/examples/sdk_cpp_smoke"
        "${VOICEENGINE_SDK_DIR}/examples/sdk_cpp_smoke"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/windows-x64/ffmpeg/LICENSE.txt"
        "${VOICEENGINE_SDK_DIR}/licenses/ffmpeg"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/common/llama.cpp/LICENSE"
        "${VOICEENGINE_SDK_DIR}/licenses/llama.cpp"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_voiceengine_sdk_stamp}"
    DEPENDS VoiceEngineCore VoiceEngineRuntime
        "${CMAKE_SOURCE_DIR}/include/voiceengine/voiceengine_api.h"
        "${CMAKE_SOURCE_DIR}/include/voiceengine/voiceengine.hpp"
        "${CMAKE_SOURCE_DIR}/include/voiceengine/voiceengine_version.h"
        "${CMAKE_SOURCE_DIR}/include/voiceengine/voiceengine_runtime.h"
        "${CMAKE_SOURCE_DIR}/voiceengine.json"
        "${CMAKE_SOURCE_DIR}/script/windows/assemble-model.cmd"
        ${VOICEENGINE_FFMPEG_DLLS}
        ${VOICEENGINE_CUDA_RUNTIME_DLLS}
        ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
        ${_voiceengine_sdk_model_files}
        "${CMAKE_SOURCE_DIR}/doc/SDK_ABI.md"
        "${CMAKE_SOURCE_DIR}/examples/sdk_smoke/CMakeLists.txt"
        "${CMAKE_SOURCE_DIR}/examples/sdk_smoke/main.cpp"
        "${CMAKE_SOURCE_DIR}/examples/sdk_cpp_smoke/CMakeLists.txt"
        "${CMAKE_SOURCE_DIR}/examples/sdk_cpp_smoke/main.cpp"
        "${_voiceengine_sdk_config}"
    COMMENT "Packaging VoiceEngine external SDK"
    VERBATIM)
add_custom_target(sdk-bin ALL DEPENDS "${_voiceengine_sdk_stamp}")
