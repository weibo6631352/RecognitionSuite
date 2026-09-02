include_guard(GLOBAL)

if(NOT SCANENGINE_ENABLE_MLX_CUDA)
    return()
endif()

if(CMAKE_VERSION VERSION_LESS "3.25")
    message(FATAL_ERROR
        "Windows MLX CUDA requires CMake >= 3.25; actual CMake is ${CMAKE_VERSION}")
endif()
if(NOT WIN32 OR NOT MSVC OR
   NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(FATAL_ERROR "Windows MLX CUDA requires the unified MSVC toolchain")
endif()
if(NOT CMAKE_GENERATOR STREQUAL "Visual Studio 17 2022")
    message(FATAL_ERROR
        "Windows MLX CUDA requires the Visual Studio 17 2022 generator; "
        "actual generator is ${CMAKE_GENERATOR}")
endif()
if(NOT MSVC_TOOLSET_VERSION EQUAL 143)
    message(FATAL_ERROR
        "Windows MLX CUDA requires the v143 toolset; actual toolset is "
        "${MSVC_TOOLSET_VERSION}")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Windows MLX CUDA requires an x64 target")
endif()

set(_scanengine_expected_msvc_runtime
    "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
if(NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL
       "${_scanengine_expected_msvc_runtime}")
    message(FATAL_ERROR
        "Windows MLX CUDA requires the dynamic v14 CRT (/MD for Release); "
        "CMAKE_MSVC_RUNTIME_LIBRARY is ${CMAKE_MSVC_RUNTIME_LIBRARY}")
endif()

set(_scanengine_mlx_vendor
    "${CMAKE_SOURCE_DIR}/third_party/windows-x64/mlx")
set(SCANENGINE_MLX_DIR "${_scanengine_mlx_vendor}" CACHE PATH
    "Vendored Windows MLX 0.31.1 CUDA static library and headers")
if(SCANENGINE_MLX_DIR STREQUAL "")
    set(SCANENGINE_MLX_DIR "${_scanengine_mlx_vendor}")
endif()
set(_scanengine_mlx_include "${SCANENGINE_MLX_DIR}/include")
set(_scanengine_mlx_lib "${SCANENGINE_MLX_DIR}/lib/mlx.lib")
set(_scanengine_mlx_dl_lib "${SCANENGINE_MLX_DIR}/lib/dl.lib")
set(_scanengine_mlx_cccl "${SCANENGINE_MLX_DIR}/jit-include/cccl")
set(SCANENGINE_MLX_CCCL_INCLUDE "${_scanengine_mlx_cccl}")
if(NOT EXISTS "${_scanengine_mlx_include}/mlx/mlx.h" OR
   NOT EXISTS "${_scanengine_mlx_include}/mlx/version.h" OR
   NOT EXISTS "${_scanengine_mlx_include}/mlx/backend/cuda/allocator.h" OR
   NOT EXISTS "${_scanengine_mlx_lib}" OR
   NOT EXISTS "${_scanengine_mlx_dl_lib}" OR
   NOT EXISTS "${_scanengine_mlx_cccl}/cuda/std/version")
    message(FATAL_ERROR
        "Incomplete Windows MLX CUDA vendor under ${SCANENGINE_MLX_DIR}; "
        "need include/mlx, lib/mlx.lib, lib/dl.lib, and jit-include/cccl")
endif()

file(STRINGS "${_scanengine_mlx_include}/mlx/version.h"
    _scanengine_mlx_version_lines
    REGEX "^#define MLX_VERSION_(MAJOR|MINOR|PATCH) [0-9]+$")
string(JOIN "\n" _scanengine_mlx_version_text
    ${_scanengine_mlx_version_lines})
foreach(_part MAJOR MINOR PATCH)
    string(REGEX MATCH "MLX_VERSION_${_part} [0-9]+"
        _scanengine_mlx_${_part}_match "${_scanengine_mlx_version_text}")
    if("${_scanengine_mlx_${_part}_match}" STREQUAL "")
        message(FATAL_ERROR
            "Cannot read MLX_VERSION_${_part} from "
            "${_scanengine_mlx_include}/mlx/version.h")
    endif()
    string(REGEX REPLACE ".* ([0-9]+)$" "\\1"
        _scanengine_mlx_${_part} "${_scanengine_mlx_${_part}_match}")
endforeach()
set(_scanengine_mlx_version
    "${_scanengine_mlx_MAJOR}.${_scanengine_mlx_MINOR}.${_scanengine_mlx_PATCH}")
if(NOT _scanengine_mlx_version STREQUAL "0.31.1")
    message(FATAL_ERROR
        "Windows GPU product is locked to MLX 0.31.1; headers report "
        "${_scanengine_mlx_version}")
endif()

function(_scanengine_require_file_identity label file expected_size expected_sha256)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "${label} is missing: ${file}")
    endif()
    file(SIZE "${file}" _actual_size)
    file(SHA256 "${file}" _actual_sha256)
    string(TOUPPER "${_actual_sha256}" _actual_sha256)
    string(TOUPPER "${expected_sha256}" _expected_sha256)
    if(NOT _actual_size EQUAL expected_size OR
       NOT _actual_sha256 STREQUAL _expected_sha256)
        message(FATAL_ERROR
            "${label} identity mismatch: expected ${expected_size} bytes / "
            "${_expected_sha256}, got ${_actual_size} bytes / "
            "${_actual_sha256} at ${file}")
    endif()
endfunction()

set(_scanengine_mlx_lib_size "531625456")
set(_scanengine_mlx_lib_sha256
    "49085D977406ABA9DDB3AAC62B37759332B83024957259ABB0F9A65C51635C0D")
set(_scanengine_mlx_dl_lib_size "16704")
set(_scanengine_mlx_dl_lib_sha256
    "15FBDF17F582C8FCEE4C8C5A7F95AE32DE608F19FDDB3FF4F0508FA5360E46C5")
_scanengine_require_file_identity(
    "Vendored Windows MLX static library"
    "${_scanengine_mlx_lib}"
    "${_scanengine_mlx_lib_size}"
    "${_scanengine_mlx_lib_sha256}")
_scanengine_require_file_identity(
    "Vendored Windows dlfcn-win32 static library"
    "${_scanengine_mlx_dl_lib}"
    "${_scanengine_mlx_dl_lib_size}"
    "${_scanengine_mlx_dl_lib_sha256}")

set(_scanengine_cuda_architectures
    "89-real;89-virtual;120a-real;120-virtual")
foreach(_arch_var CMAKE_CUDA_ARCHITECTURES MLX_CUDA_ARCHITECTURES)
    if(DEFINED ${_arch_var} AND
       NOT "${${_arch_var}}" STREQUAL "" AND
       NOT "${${_arch_var}}" STREQUAL "${_scanengine_cuda_architectures}")
        message(FATAL_ERROR
            "${_arch_var} must be ${_scanengine_cuda_architectures}; actual "
            "value is ${${_arch_var}}")
    endif()
    set(${_arch_var} "${_scanengine_cuda_architectures}" CACHE STRING
        "RTX 40 sm_89 and RTX 50 sm_120 native/PTX architectures" FORCE)
endforeach()

find_package(CUDAToolkit 12.9 REQUIRED)
if(CUDAToolkit_VERSION VERSION_LESS "12.9" OR
   NOT CUDAToolkit_VERSION VERSION_LESS "12.10")
    message(FATAL_ERROR
        "Windows GPU product requires CUDA Toolkit 12.9.x; actual version is "
        "${CUDAToolkit_VERSION}")
endif()
if(NOT DEFINED CUDAToolkit_BIN_DIR OR CUDAToolkit_BIN_DIR STREQUAL "")
    message(FATAL_ERROR
        "FindCUDAToolkit did not report CUDAToolkit_BIN_DIR for CUDA 12.9")
endif()
file(REAL_PATH "${CUDAToolkit_BIN_DIR}/.." _scanengine_cuda_toolkit_root)

set(_scanengine_cudnn_version_header
    "${_scanengine_cuda_toolkit_root}/include/cudnn_version.h")
set(_scanengine_cudnn_runtime
    "${_scanengine_cuda_toolkit_root}/bin/cudnn64_9.dll")
if(NOT EXISTS "${_scanengine_cudnn_version_header}")
    message(FATAL_ERROR
        "CUDA 12.9 Toolkit root is missing cuDNN version headers: "
        "${_scanengine_cudnn_version_header}")
endif()
file(STRINGS "${_scanengine_cudnn_version_header}"
    _scanengine_cudnn_version_lines
    REGEX "^#define CUDNN_(MAJOR|MINOR|PATCHLEVEL)[ \\t]+[0-9]+$")
string(JOIN "\n" _scanengine_cudnn_version_text
    ${_scanengine_cudnn_version_lines})
foreach(_part MAJOR MINOR PATCHLEVEL)
    string(REGEX MATCH "CUDNN_${_part}[ \\t]+([0-9]+)"
        _scanengine_cudnn_${_part}_match "${_scanengine_cudnn_version_text}")
    if("${_scanengine_cudnn_${_part}_match}" STREQUAL "")
        message(FATAL_ERROR
            "Cannot read CUDNN_${_part} from "
            "${_scanengine_cudnn_version_header}")
    endif()
    set(_scanengine_cudnn_${_part} "${CMAKE_MATCH_1}")
endforeach()
set(_scanengine_cudnn_header_version
    "${_scanengine_cudnn_MAJOR}.${_scanengine_cudnn_MINOR}.${_scanengine_cudnn_PATCHLEVEL}")
if(NOT _scanengine_cudnn_header_version STREQUAL "9.9.0")
    message(FATAL_ERROR
        "Windows GPU product requires cuDNN headers 9.9.0; actual version is "
        "${_scanengine_cudnn_header_version}")
endif()
if(NOT EXISTS "${_scanengine_cudnn_runtime}")
    message(FATAL_ERROR
        "CUDA 12.9 Toolkit root is missing cuDNN runtime: "
        "${_scanengine_cudnn_runtime}")
endif()
file(SIZE "${_scanengine_cudnn_runtime}" _scanengine_cudnn_runtime_size)
file(SHA256 "${_scanengine_cudnn_runtime}" _scanengine_cudnn_runtime_sha256)
string(TOUPPER "${_scanengine_cudnn_runtime_sha256}"
    _scanengine_cudnn_runtime_sha256)
set(_scanengine_cudnn_runtime_expected_size "265760")
set(_scanengine_cudnn_runtime_expected_sha256
    "89FCB0F37BB0983BE7F5D98DBA2A23DE0539DA9F7F0E1994DEB0EF002ABD764C")
set(_scanengine_cudnn_runtime_version "9.9.0.52")
if(NOT _scanengine_cudnn_runtime_size EQUAL
       _scanengine_cudnn_runtime_expected_size OR
   NOT _scanengine_cudnn_runtime_sha256 STREQUAL
       _scanengine_cudnn_runtime_expected_sha256)
    message(FATAL_ERROR
        "cudnn64_9.dll identity mismatch under the selected CUDA 12.9 root; "
        "expected cuDNN 9.9.0.52 size "
        "${_scanengine_cudnn_runtime_expected_size} SHA-256 "
        "${_scanengine_cudnn_runtime_expected_sha256}, got size "
        "${_scanengine_cudnn_runtime_size} SHA-256 "
        "${_scanengine_cudnn_runtime_sha256} at ${_scanengine_cudnn_runtime}")
endif()

# MLX imports only the public cudnn64_9 entry points. That dispatcher loads its
# split graph/ops/engine DLLs itself, so linking their import libraries here
# adds no imports and only produces ignored /DELAYLOAD warnings.
set(_scanengine_cudnn_components cudnn)
set(_scanengine_cudnn_libs "")
foreach(_component IN LISTS _scanengine_cudnn_components)
    set(_lib "${_scanengine_cuda_toolkit_root}/lib/x64/${_component}.lib")
    if(NOT EXISTS "${_lib}")
        message(FATAL_ERROR "CUDA 12.9 cuDNN library is missing: ${_lib}")
    endif()
    file(REAL_PATH "${_lib}" _lib)
    set("_scanengine_cudnn_${_component}_imported_location" "${_lib}")
    list(APPEND _scanengine_cudnn_libs "${_lib}")
endforeach()
file(REAL_PATH "${_scanengine_cuda_toolkit_root}/include"
    _scanengine_mlx_cudnn_include_dir)

add_library(dl STATIC IMPORTED GLOBAL)
set_target_properties(dl PROPERTIES
    IMPORTED_LOCATION "${_scanengine_mlx_dl_lib}"
    IMPORTED_LOCATION_RELEASE "${_scanengine_mlx_dl_lib}"
    IMPORTED_LOCATION_RELWITHDEBINFO "${_scanengine_mlx_dl_lib}"
    IMPORTED_LOCATION_MINSIZEREL "${_scanengine_mlx_dl_lib}"
    IMPORTED_LOCATION_DEBUG "${_scanengine_mlx_dl_lib}")

add_library(mlx STATIC IMPORTED GLOBAL)
set_target_properties(mlx PROPERTIES
    IMPORTED_LOCATION "${_scanengine_mlx_lib}"
    IMPORTED_LOCATION_RELEASE "${_scanengine_mlx_lib}"
    IMPORTED_LOCATION_RELWITHDEBINFO "${_scanengine_mlx_lib}"
    IMPORTED_LOCATION_MINSIZEREL "${_scanengine_mlx_lib}"
    IMPORTED_LOCATION_DEBUG "${_scanengine_mlx_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_scanengine_mlx_include}"
    INTERFACE_COMPILE_DEFINITIONS "MLX_STATIC")

set(_scanengine_mlx_delayload_dlls
    cudart64_12.dll
    cublasLt64_12.dll
    nvrtc64_120_0.dll
    cudnn64_9.dll)
set(_scanengine_mlx_link_options "/INCLUDE:__pfnDliNotifyHook2")
foreach(_dll IN LISTS _scanengine_mlx_delayload_dlls)
    list(APPEND _scanengine_mlx_link_options "/DELAYLOAD:${_dll}")
endforeach()
set_property(TARGET mlx PROPERTY
    INTERFACE_LINK_OPTIONS "${_scanengine_mlx_link_options}")
target_link_libraries(mlx INTERFACE
    dl
    delayimp.lib
    CUDA::cublasLt
    CUDA::nvrtc
    CUDA::cuda_driver
    ${_scanengine_cudnn_libs})
target_include_directories(mlx SYSTEM INTERFACE
    "${CUDAToolkit_INCLUDE_DIRS}")

add_library(scanengine_mlx_cuda_bridge STATIC
    "${CMAKE_SOURCE_DIR}/src/gpu/mlx_cuda_patch_embedding_bridge.cpp"
    "${CMAKE_SOURCE_DIR}/src/gpu/mlx_batch_norm_eval.cpp")
add_library(scanengine::mlx_cuda_bridge ALIAS scanengine_mlx_cuda_bridge)
set_target_properties(scanengine_mlx_cuda_bridge PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    MSVC_RUNTIME_LIBRARY "${_scanengine_expected_msvc_runtime}")
target_compile_features(scanengine_mlx_cuda_bridge PRIVATE cxx_std_20)
target_include_directories(scanengine_mlx_cuda_bridge
    PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_compile_options(scanengine_mlx_cuda_bridge PRIVATE /W4 /utf-8)
target_link_libraries(scanengine_mlx_cuda_bridge PRIVATE
    mlx CUDA::cudart)
target_compile_definitions(scanengine_mlx_cuda_bridge INTERFACE MLX_STATIC)
add_custom_target(scanengine-mlx-cuda-backend ALL
    DEPENDS mlx scanengine_mlx_cuda_bridge)

function(scanengine_check_windows_mlx_link core_target)
    if(NOT TARGET "${core_target}")
        message(FATAL_ERROR "Windows MLX CUDA: missing ${core_target}")
    endif()
    get_target_property(_core_standard "${core_target}" CXX_STANDARD)
    if(NOT _core_standard EQUAL 17)
        message(FATAL_ERROR
            "${core_target} must remain C++17; actual CXX_STANDARD is "
            "${_core_standard}")
    endif()
    if(NOT TARGET scanengine_mlx_cuda_bridge)
        message(FATAL_ERROR "Windows MLX CUDA bridge target is missing")
    endif()
    get_target_property(_core_direct_links "${core_target}" LINK_LIBRARIES)
    if(NOT _core_direct_links OR _core_direct_links MATCHES "-NOTFOUND$")
        message(FATAL_ERROR
            "${core_target} must link scanengine_mlx_cuda_bridge")
    endif()
    set(_core_has_bridge FALSE)
    foreach(_link_item IN LISTS _core_direct_links)
        string(REGEX MATCHALL "[A-Za-z0-9_:.+-]+" _link_tokens
            "${_link_item}")
        foreach(_link_token IN LISTS _link_tokens)
            if(_link_token STREQUAL "mlx")
                message(FATAL_ERROR
                    "${core_target} must not link MLX directly")
            endif()
            if(_link_token STREQUAL "scanengine_mlx_cuda_bridge" OR
               _link_token STREQUAL "scanengine::mlx_cuda_bridge")
                set(_core_has_bridge TRUE)
            endif()
        endforeach()
    endforeach()
    if(NOT _core_has_bridge)
        message(FATAL_ERROR
            "${core_target} must link scanengine_mlx_cuda_bridge")
    endif()
    message(STATUS
        "Windows MLX CUDA: prebuilt ${_scanengine_mlx_version}, static, "
        "CUDA ${CUDAToolkit_VERSION}, arch "
        "${_scanengine_cuda_architectures}, vendor ${SCANENGINE_MLX_DIR}")
endfunction()
