set(VOICEENGINE_CUDA_RUNTIME_DLLS
    "${VOICEENGINE_CUDA_ROOT}/bin/cudart64_12.dll"
    "${VOICEENGINE_CUDA_ROOT}/bin/cublas64_12.dll"
    "${VOICEENGINE_CUDA_ROOT}/bin/cublasLt64_12.dll")
foreach(_dll IN LISTS VOICEENGINE_CUDA_RUNTIME_DLLS)
    if(NOT EXISTS "${_dll}")
        message(FATAL_ERROR "CUDA runtime DLL missing: ${_dll}")
    endif()
endforeach()
