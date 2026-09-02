set(VOICEENGINE_LLAMA_ROOT
    "${CMAKE_SOURCE_DIR}/third_party/common/llama.cpp" CACHE PATH
    "Pinned llama.cpp source (mtmd-capable)")
if(NOT EXISTS "${VOICEENGINE_LLAMA_ROOT}/CMakeLists.txt" OR
   NOT EXISTS "${VOICEENGINE_LLAMA_ROOT}/include/llama.h" OR
   NOT EXISTS "${VOICEENGINE_LLAMA_ROOT}/tools/mtmd/mtmd.h")
    message(FATAL_ERROR
        "Pinned llama.cpp tree is missing under ${VOICEENGINE_LLAMA_ROOT}")
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "static llama/mtmd into VoiceEngineCore.dll" FORCE)
set(GGML_CUDA ON CACHE BOOL "" FORCE)
set(VOICEENGINE_CUDA_ARCHITECTURES "89;120" CACHE STRING
    "CUDA architectures shipped by VoiceEngine (RTX 40 / RTX 50 series)")
set(CMAKE_CUDA_ARCHITECTURES "${VOICEENGINE_CUDA_ARCHITECTURES}" CACHE STRING
    "RTX 40 sm_89 and RTX 50 sm_120" FORCE)
set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_MTMD ON CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)

enable_language(CUDA)
add_subdirectory("${VOICEENGINE_LLAMA_ROOT}" "${CMAKE_BINARY_DIR}/third_party/llama.cpp" EXCLUDE_FROM_ALL)

if(NOT TARGET llama OR NOT TARGET mtmd)
    message(FATAL_ERROR "llama.cpp did not produce llama and mtmd targets")
endif()

message(STATUS
    "llama.cpp + libmtmd CUDA architectures ${CMAKE_CUDA_ARCHITECTURES} "
    "(RTX 40 / RTX 50; no CPU ASR product path)")
