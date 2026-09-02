cmake_minimum_required(VERSION 3.21)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC OR NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(FATAL_ERROR
        "Windows requires one unified MSVC C/C++ toolchain; actual compilers are "
        "${CMAKE_C_COMPILER_ID} and ${CMAKE_CXX_COMPILER_ID}")
endif()
if(NOT CMAKE_GENERATOR STREQUAL "Visual Studio 17 2022" OR
   NOT MSVC_TOOLSET_VERSION EQUAL 143 OR
   MSVC_VERSION LESS 1930 OR MSVC_VERSION GREATER_EQUAL 1950)
    message(FATAL_ERROR
        "Windows requires Visual Studio 17 2022 with the v143 x64 toolset; actual generator/compiler/toolset: "
        "${CMAKE_GENERATOR}, ${CMAKE_CXX_COMPILER_VERSION}, ${MSVC_TOOLSET_VERSION}")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Windows requires the x64 architecture")
endif()

# All project and vendored source targets use the dynamic v14 CRT. The exact
# release CRT files are deployed beside the executables below.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

include("${CMAKE_SOURCE_DIR}/cmake/WindowsMlxCuda.cmake")

# Source and UI literals are UTF-8; apply this to executables and vendored
# source libraries as well as the core library.
add_compile_options(/utf-8)

# Collect the v143 CRT selected by this configure. windeployqt only discovers
# it when VCINSTALLDIR happens to be present, which is not true for ordinary
# preset builds using the Visual Studio generator.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
include(InstallRequiredSystemLibraries)
if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
    message(FATAL_ERROR "Cannot locate the MSVC runtime deployment files")
endif()

set(SCANENGINE_DEPENDENCY_PLATFORM "windows-x64")
set(SCANENGINE_COMMON_DEPS "${CMAKE_SOURCE_DIR}/third_party/common")
set(SCANENGINE_PLATFORM_DEPS
    "${CMAKE_SOURCE_DIR}/third_party/${SCANENGINE_DEPENDENCY_PLATFORM}")
if(NOT IS_DIRECTORY "${SCANENGINE_PLATFORM_DEPS}")
    message(FATAL_ERROR
        "Repository dependency bundle is missing: third_party/${SCANENGINE_DEPENDENCY_PLATFORM}")
endif()
message(STATUS "ScanEngine platform: Windows ${SCANENGINE_DEPENDENCY_PLATFORM}")
include("${CMAKE_SOURCE_DIR}/cmake/OnnxRuntime.cmake")

set(SCANENGINE_RAW_DIR "${CMAKE_BINARY_DIR}/raw")
set(SCANENGINE_BIN_DIR "${CMAKE_BINARY_DIR}/bin")
set(SCANENGINE_SDK_DIR "${CMAKE_BINARY_DIR}/sdk")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${SCANENGINE_RAW_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

set(SCANENGINE_QT_ROOT "C:/Qt/Qt5.12.9/5.12.9/msvc2017_64" CACHE PATH
    "Exact Qt 5.12.9 msvc2017_64 SDK root")
if(NOT EXISTS "${SCANENGINE_QT_ROOT}/lib/cmake/Qt5/Qt5Config.cmake")
    message(FATAL_ERROR
        "Qt 5.12.9 msvc2017_64 is missing under ${SCANENGINE_QT_ROOT}")
endif()
list(PREPEND CMAKE_PREFIX_PATH "${SCANENGINE_QT_ROOT}")
find_package(Qt5 5.12.9 EXACT REQUIRED COMPONENTS Core Gui Widgets)
get_target_property(_qt_qmake Qt5::qmake IMPORTED_LOCATION)
get_filename_component(_qt_bin_dir "${_qt_qmake}" DIRECTORY)
get_filename_component(_qt_root_actual "${_qt_bin_dir}" DIRECTORY)
file(REAL_PATH "${SCANENGINE_QT_ROOT}" _qt_root_expected)
file(REAL_PATH "${_qt_root_actual}" _qt_root_actual)
string(TOLOWER "${_qt_root_expected}" _qt_root_expected_lower)
string(TOLOWER "${_qt_root_actual}" _qt_root_actual_lower)
if(NOT _qt_root_expected_lower STREQUAL _qt_root_actual_lower)
    message(FATAL_ERROR
        "Qt headers, libraries and tools must all come from ${_qt_root_expected}; actual root is ${_qt_root_actual}")
endif()
message(STATUS "Using Qt ${Qt5_VERSION} at ${Qt5_DIR}")

if(NOT EXISTS "${SCANENGINE_COMMON_DEPS}/QXlsx/CMakeLists.txt")
    message(FATAL_ERROR "Vendored QXlsx source is missing")
endif()
add_subdirectory("${SCANENGINE_COMMON_DEPS}/QXlsx" "third_party/QXlsx-build"
    EXCLUDE_FROM_ALL)

# fast-langdetect 0.2.5 uses fastText's compressed lid.176.ftz model.  Compile
# the locked upstream v0.9.2 inference source on both hosts so Hybrid artifact
# joining has no Python or machine-global fastText dependency.
set(_fasttext_root "${SCANENGINE_COMMON_DEPS}/fasttext-0.9.2")
foreach(_required
        README.scanengine.md SOURCE.MANIFEST.sha256 LICENSE
        src/args.cc src/args.h
        src/densematrix.cc src/densematrix.h
        src/dictionary.cc src/dictionary.h
        src/fasttext.cc src/fasttext.h
        src/loss.cc src/loss.h
        src/matrix.cc src/matrix.h
        src/meter.cc src/meter.h
        src/model.cc src/model.h
        src/productquantizer.cc src/productquantizer.h
        src/quantmatrix.cc src/quantmatrix.h
        src/real.h src/utils.cc src/utils.h src/vector.cc src/vector.h)
    if(NOT EXISTS "${_fasttext_root}/${_required}")
        message(FATAL_ERROR
            "Vendored fastText v0.9.2 source is incomplete: ${_required}")
    endif()
endforeach()
set(_scanengine_fasttext_sources
    src/args.cc
    src/densematrix.cc
    src/dictionary.cc
    src/fasttext.cc
    src/loss.cc
    src/matrix.cc
    src/meter.cc
    src/model.cc
    src/productquantizer.cc
    src/quantmatrix.cc
    src/utils.cc
    src/vector.cc)
list(TRANSFORM _scanengine_fasttext_sources PREPEND "${_fasttext_root}/")
add_library(scanengine_fasttext_static STATIC ${_scanengine_fasttext_sources})
set_target_properties(scanengine_fasttext_static PROPERTIES AUTOMOC OFF)
target_compile_features(scanengine_fasttext_static PUBLIC cxx_std_17)
target_include_directories(scanengine_fasttext_static SYSTEM PUBLIC
    "${_fasttext_root}/src")
target_compile_options(scanengine_fasttext_static PRIVATE /wd4244 /wd4267)

include("${CMAKE_SOURCE_DIR}/cmake/Sources.cmake")

add_library(scanengine_core STATIC ${SCANENGINE_CORE_SOURCES})
set_target_properties(scanengine_core PROPERTIES AUTOMOC ON)
target_include_directories(scanengine_core PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_include_directories(scanengine_core SYSTEM PRIVATE
    "${SCANENGINE_ORT_INCLUDE_DIR}")
target_compile_definitions(scanengine_core PRIVATE
    ${SCANENGINE_ORT_COMPILE_DEFINITIONS})
target_link_libraries(scanengine_core
    PUBLIC Qt5::Core Qt5::Gui
    PRIVATE QXlsx::QXlsx scanengine_fasttext_static)
target_compile_options(scanengine_core PRIVATE /W4)

# Enable the only Windows extraction path: MLX CUDA.
target_compile_definitions(scanengine_core PRIVATE SCANENGINE_MLX=1 MLX_STATIC)
target_include_directories(scanengine_core PRIVATE "${SCANENGINE_MLX_DIR}/include")
if(TARGET scanengine_mlx_cuda_bridge)
    target_link_libraries(scanengine_core PRIVATE scanengine_mlx_cuda_bridge)
endif()
scanengine_check_windows_mlx_link(scanengine_core)

add_executable(ScanEngine WIN32
    src/app/main.cpp
    src/ui/main_window.cpp
    src/ui/parse_controller.cpp
    src/ui/widgets/control_panel.cpp
    src/ui/widgets/drop_zone.cpp
    src/ui/widgets/preview_pane.cpp
    src/ui/widgets/result_pane.cpp
    src/ui/widgets/status_panel.cpp
    resources/windows/scanengine.rc
    resources/resources.qrc)
set_source_files_properties(resources/windows/scanengine.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/resources/icons/scanengine.ico")
set_target_properties(ScanEngine PROPERTIES AUTOMOC ON AUTORCC ON)
target_include_directories(ScanEngine PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(ScanEngine PRIVATE scanengine_core Qt5::Widgets)

add_executable(ScanEngineTool src/app/contract_cli.cpp)
target_link_libraries(ScanEngineTool PRIVATE scanengine_core)

set(SCANENGINE_PDFIUM_DIR "${SCANENGINE_PLATFORM_DEPS}/pdfium" CACHE PATH
    "Repository directory containing the Windows pdfium runtime")
set(_pdfium_include "${SCANENGINE_COMMON_DEPS}/pdfium/include")
if(NOT EXISTS "${_pdfium_include}/fpdfview_min.h")
    message(FATAL_ERROR "Missing ${_pdfium_include}/fpdfview_min.h")
endif()
find_file(SCANENGINE_PDFIUM_RUNTIME NAMES pdfium.dll
    HINTS "${SCANENGINE_PDFIUM_DIR}" NO_DEFAULT_PATH)
if(NOT SCANENGINE_PDFIUM_RUNTIME)
    message(FATAL_ERROR "Repository pdfium runtime not found under ${SCANENGINE_PDFIUM_DIR}")
endif()
target_include_directories(scanengine_core SYSTEM PRIVATE "${_pdfium_include}")

set(_libjpeg_root "${SCANENGINE_COMMON_DEPS}/libjpeg")
if(NOT EXISTS "${_libjpeg_root}/jpeglib.h" OR
   NOT EXISTS "${_libjpeg_root}/jmemnobs.c")
    message(FATAL_ERROR "Vendored IJG jpeg-9f source is incomplete")
endif()
set(_libjpeg_binary "${CMAKE_BINARY_DIR}/third_party/libjpeg-build")
file(MAKE_DIRECTORY "${_libjpeg_binary}")
configure_file("${CMAKE_SOURCE_DIR}/cmake/jconfig.libjpeg.h"
    "${_libjpeg_binary}/jconfig.h" COPYONLY)
set(_scanengine_libjpeg_sources
    jaricom.c jcapimin.c jcapistd.c jcarith.c jccoefct.c jccolor.c
    jcdctmgr.c jchuff.c jcinit.c jcmainct.c jcmarker.c jcmaster.c
    jcomapi.c jcparam.c jcprepct.c jcsample.c jctrans.c jdapimin.c
    jdapistd.c jdarith.c jdatadst.c jdatasrc.c jdcoefct.c jdcolor.c
    jddctmgr.c jdhuff.c jdinput.c jdmainct.c jdmarker.c jdmaster.c
    jdmerge.c jdpostct.c jdsample.c jdtrans.c jerror.c jfdctflt.c
    jfdctfst.c jfdctint.c jidctflt.c jidctfst.c jidctint.c jquant1.c
    jquant2.c jutils.c jmemmgr.c jmemnobs.c)
list(TRANSFORM _scanengine_libjpeg_sources PREPEND "${_libjpeg_root}/")
add_library(scanengine_libjpeg_static STATIC ${_scanengine_libjpeg_sources})
set_target_properties(scanengine_libjpeg_static PROPERTIES AUTOMOC OFF)
target_include_directories(scanengine_libjpeg_static PUBLIC
    "${_libjpeg_binary}" "${_libjpeg_root}")
target_compile_definitions(scanengine_core PRIVATE SCANENGINE_LIBJPEG=1)
target_link_libraries(scanengine_core PRIVATE scanengine_libjpeg_static)

# Pillow in the pinned MinerU environment rasterizes table-image tokens with
# FreeType 2.14.3.  Build the same minimal upstream source set on every host so
# Qt's bundled (and version-dependent) private FreeType never affects VLM input.
set(_freetype_root "${SCANENGINE_COMMON_DEPS}/freetype-2.14.3")
foreach(_required
        README.scanengine.md LICENSE.TXT docs/FTL.TXT
        include/ft2build.h
        include/freetype/config/scanengine-ftoption.h
        src/base/ftsystem.c src/base/ftdebug.c src/base/ftinit.c
        src/base/ftbase.c src/base/ftbitmap.c src/base/ftglyph.c
        src/base/ftmm.c src/sfnt/sfnt.c src/truetype/truetype.c
        src/smooth/smooth.c)
    if(NOT EXISTS "${_freetype_root}/${_required}")
        message(FATAL_ERROR
            "Vendored FreeType 2.14.3 source is incomplete: ${_required}")
    endif()
endforeach()
set(_scanengine_freetype_sources
    src/base/ftsystem.c
    src/base/ftdebug.c
    src/base/ftinit.c
    src/base/ftbase.c
    src/base/ftbitmap.c
    src/base/ftglyph.c
    src/base/ftmm.c
    src/sfnt/sfnt.c
    src/truetype/truetype.c
    src/smooth/smooth.c)
list(TRANSFORM _scanengine_freetype_sources PREPEND "${_freetype_root}/")
add_library(scanengine_freetype_static STATIC ${_scanengine_freetype_sources})
set_target_properties(scanengine_freetype_static PROPERTIES
    AUTOMOC OFF
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(scanengine_freetype_static SYSTEM PUBLIC
    "${_freetype_root}/include")
target_compile_definitions(scanengine_freetype_static
    PUBLIC "FT_CONFIG_OPTIONS_H=<freetype/config/scanengine-ftoption.h>"
    PRIVATE FT2_BUILD_LIBRARY)
target_compile_definitions(scanengine_core PRIVATE
    SCANENGINE_PINNED_FREETYPE=1)
target_link_libraries(scanengine_core PRIVATE scanengine_freetype_static)

# External SDK boundary. The implementation remains in the existing static
# core so the desktop applications do not change; this thin shared library is
# the only C ABI exposed to third-party hosts.
add_library(ScanEngineCore SHARED src/sdk/scanengine_api.cpp)
set_target_properties(ScanEngineCore PROPERTIES
    OUTPUT_NAME "ScanEngineCore"
    AUTOMOC ON)
target_compile_definitions(ScanEngineCore PRIVATE SCANENGINE_CORE_EXPORTS)
target_include_directories(ScanEngineCore PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(ScanEngineCore PRIVATE scanengine_core)
target_compile_options(ScanEngineCore PRIVATE /W4)

# This bootstrap contains no Qt or inference code. A third-party executable
# links it statically and calls it before the delay-loaded core DLL is touched.
add_library(ScanEngineRuntime STATIC src/sdk/scanengine_runtime.cpp)
target_include_directories(ScanEngineRuntime PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_compile_options(ScanEngineRuntime PRIVATE /W4)

set(_qt_deploy_tool "${_qt_bin_dir}/windeployqt.exe")
if(NOT EXISTS "${_qt_deploy_tool}")
    message(FATAL_ERROR "windeployqt not found: ${_qt_deploy_tool}")
endif()

set(_windows_qt_conf "${CMAKE_BINARY_DIR}/qt.conf")
file(WRITE "${_windows_qt_conf}" "[Paths]\nPrefix = .\n")
set(_qt_deploy_args --release --no-translations --no-compiler-runtime)
set(_runtime_stamp "${SCANENGINE_BIN_DIR}/.runtime.stamp")
add_custom_command(
    OUTPUT "${_runtime_stamp}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${SCANENGINE_BIN_DIR}" "${SCANENGINE_BIN_DIR}/output"
        "${SCANENGINE_BIN_DIR}/licenses/onnxruntime"
        "${SCANENGINE_BIN_DIR}/licenses/magika"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:ScanEngine>"
        "$<TARGET_FILE:ScanEngineTool>"
        "$<TARGET_FILE:ScanEngineCore>"
        "${SCANENGINE_PDFIUM_RUNTIME}"
        "${SCANENGINE_ORT_RUNTIME}"
        "${CMAKE_SOURCE_DIR}/scanengine.json"
        "${_windows_qt_conf}"
        "${SCANENGINE_BIN_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SCANENGINE_ORT_PLATFORM_LICENSE}"
        "${SCANENGINE_ORT_PLATFORM_NOTICES}"
        "${SCANENGINE_BIN_DIR}/licenses/onnxruntime"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SCANENGINE_MAGIKA_LICENSE}"
        "${SCANENGINE_BIN_DIR}/licenses/magika/LICENSE.Apache-2.0.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} "${SCANENGINE_BIN_DIR}"
    COMMAND "${_qt_deploy_tool}"
        ${_qt_deploy_args}
        --dir "${SCANENGINE_BIN_DIR}"
        "${SCANENGINE_BIN_DIR}/$<TARGET_FILE_NAME:ScanEngine>"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_runtime_stamp}"
    DEPENDS
        ScanEngine
        ScanEngineTool
        ScanEngineCore
        "${CMAKE_SOURCE_DIR}/scanengine.json"
        "${SCANENGINE_PDFIUM_RUNTIME}"
        "${SCANENGINE_ORT_RUNTIME}"
        "${SCANENGINE_ORT_PLATFORM_LICENSE}"
        "${SCANENGINE_ORT_PLATFORM_NOTICES}"
        "${SCANENGINE_MAGIKA_LICENSE}"
        "${_windows_qt_conf}"
        ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
        "${CMAKE_CURRENT_LIST_FILE}"
    COMMENT "Deploying Windows runtime into bin/"
    VERBATIM)

file(GLOB_RECURSE _model_files CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${CMAKE_SOURCE_DIR}/models/*")
set(_required_models
    vlm/config.json
    vlm/tokenizer.json
    vlm/model.safetensors.part1
    vlm/model.safetensors.part2
    vlm/preprocessor_config.json
    layout/PP-DocLayoutV2/config.json
    layout/PP-DocLayoutV2/model.safetensors
    ocr/ch_PP-OCRv6_small_det_infer.safetensors
    ocr/ch_PP-OCRv6_small_rec_infer.safetensors
    dict/ppocrv6_dict.txt
    fasttext/lid.176.ftz
    fasttext/NOTICE.MD
    fasttext/LICENSE.CC-BY-SA-3.0.txt
    fasttext/LICENSE.fastText-MIT.txt
    fasttext/LICENSE.fast-langdetect-MIT.txt
    fasttext/LICENSE.fasttext-langdetect-MIT.txt
    fasttext/NOTICE.fast-langdetect.md
    fasttext/README.md
    magika/standard_v3_3/model.onnx
    magika/standard_v3_3/config.min.json
    magika/standard_v3_3/LICENSE.Apache-2.0.txt
    magika/standard_v3_3/README.upstream.md)
foreach(_model IN LISTS _required_models)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/models/${_model}")
        message(FATAL_ERROR "Required model file is missing: models/${_model}")
    endif()
endforeach()
set(_models_stamp "${SCANENGINE_BIN_DIR}/.models.stamp")
add_custom_command(
    OUTPUT "${_models_stamp}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SCANENGINE_BIN_DIR}/models"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SCANENGINE_BIN_DIR}/models"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/models" "${SCANENGINE_BIN_DIR}/models"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_models_stamp}"
    DEPENDS ${_model_files}
    COMMENT "Deploying official models into bin/models (incremental)"
    VERBATIM)

add_custom_target(deploy-bin ALL DEPENDS "${_runtime_stamp}" "${_models_stamp}")
set(_scanengine_cuda_app_private_dlls
        "${_scanengine_cuda_toolkit_root}/bin/cudart64_12.dll"
        "${_scanengine_cuda_toolkit_root}/bin/cublasLt64_12.dll"
        "${_scanengine_cuda_toolkit_root}/bin/nvrtc64_120_0.dll"
        "${_scanengine_cuda_toolkit_root}/bin/nvrtc-builtins64_129.dll"
        "${_scanengine_cuda_toolkit_root}/bin/cudnn64_9.dll"
        "${_scanengine_cuda_toolkit_root}/bin/cudnn_graph64_9.dll"
        "${_scanengine_cuda_toolkit_root}/bin/cudnn_ops64_9.dll"
        "${_scanengine_cuda_toolkit_root}/bin/cudnn_cnn64_9.dll"
        "${_scanengine_cuda_toolkit_root}/bin/cudnn_adv64_9.dll"
        "${_scanengine_cuda_toolkit_root}/bin/cudnn_heuristic64_9.dll"
        "${_scanengine_cuda_toolkit_root}/bin/cudnn_engines_precompiled64_9.dll"
        "${_scanengine_cuda_toolkit_root}/bin/cudnn_engines_runtime_compiled64_9.dll")
foreach(_cuda_dll IN LISTS _scanengine_cuda_app_private_dlls)
    if(NOT EXISTS "${_cuda_dll}")
        message(FATAL_ERROR
            "App-private CUDA/cuDNN runtime is missing: ${_cuda_dll}")
    endif()
endforeach()
set(_scanengine_cccl_include "${SCANENGINE_MLX_CCCL_INCLUDE}")
if(NOT IS_DIRECTORY "${_scanengine_cccl_include}")
    message(FATAL_ERROR
        "CCCL include tree is missing: ${_scanengine_cccl_include}")
endif()
set(_scanengine_cuda_include "${_scanengine_cuda_toolkit_root}/include")
if(NOT IS_DIRECTORY "${_scanengine_cuda_include}")
    message(FATAL_ERROR
        "CUDA toolkit include tree is missing: ${_scanengine_cuda_include}")
endif()
set(_cuda_runtime_stamp "${SCANENGINE_BIN_DIR}/.cuda-runtime.stamp")
add_custom_command(
        OUTPUT "${_cuda_runtime_stamp}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${SCANENGINE_BIN_DIR}/runtimes/cuda"
            "${SCANENGINE_BIN_DIR}/include"
        COMMAND "${CMAKE_COMMAND}" -E rm -f
            "${SCANENGINE_BIN_DIR}/runtimes/cuda/cublas64_12.dll"
            "${SCANENGINE_BIN_DIR}/runtimes/cuda/nvJitLink_120_0.dll"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${_scanengine_cuda_app_private_dlls}
            "${SCANENGINE_BIN_DIR}/runtimes/cuda"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf
            "${SCANENGINE_BIN_DIR}/include/cccl"
            "${SCANENGINE_BIN_DIR}/include/cuda"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${_scanengine_cccl_include}"
            "${SCANENGINE_BIN_DIR}/include/cccl"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${_scanengine_cuda_include}"
            "${SCANENGINE_BIN_DIR}/include/cuda"
        COMMAND "${CMAKE_COMMAND}" -E touch "${_cuda_runtime_stamp}"
        DEPENDS ${_scanengine_cuda_app_private_dlls} deploy-bin
        COMMENT "Deploying app-private CUDA runtime, cuDNN, and JIT headers"
        VERBATIM)
add_custom_target(deploy-cuda-runtime ALL DEPENDS "${_cuda_runtime_stamp}")

set(_scanengine_sdk_config "${CMAKE_BINARY_DIR}/ScanEngineConfig.cmake")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/ScanEngineSDKConfig.cmake.in"
    "${_scanengine_sdk_config}" @ONLY)
set(_scanengine_sdk_stamp "${SCANENGINE_SDK_DIR}/.sdk.stamp")
add_custom_command(
    OUTPUT "${_scanengine_sdk_stamp}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SCANENGINE_SDK_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${SCANENGINE_SDK_DIR}/bin/runtimes/cuda"
        "${SCANENGINE_SDK_DIR}/bin/include"
        "${SCANENGINE_SDK_DIR}/include/scanengine"
        "${SCANENGINE_SDK_DIR}/lib"
        "${SCANENGINE_SDK_DIR}/cmake"
        "${SCANENGINE_SDK_DIR}/doc"
        "${SCANENGINE_SDK_DIR}/examples"
        "${SCANENGINE_SDK_DIR}/licenses/onnxruntime"
        "${SCANENGINE_SDK_DIR}/licenses/magika"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:ScanEngineCore>"
        "${SCANENGINE_PDFIUM_RUNTIME}"
        "${SCANENGINE_ORT_RUNTIME}"
        "${CMAKE_SOURCE_DIR}/scanengine.json"
        "${_windows_qt_conf}"
        ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
        "${SCANENGINE_SDK_DIR}/bin"
    COMMAND "${_qt_deploy_tool}"
        ${_qt_deploy_args}
        --dir "${SCANENGINE_SDK_DIR}/bin"
        "${SCANENGINE_SDK_DIR}/bin/$<TARGET_FILE_NAME:ScanEngineCore>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${_scanengine_cuda_app_private_dlls}
        "${SCANENGINE_SDK_DIR}/bin/runtimes/cuda"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${_scanengine_cccl_include}"
        "${SCANENGINE_SDK_DIR}/bin/include/cccl"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${_scanengine_cuda_include}"
        "${SCANENGINE_SDK_DIR}/bin/include/cuda"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/models"
        "${SCANENGINE_SDK_DIR}/bin/models"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SCANENGINE_ORT_PLATFORM_LICENSE}"
        "${SCANENGINE_ORT_PLATFORM_NOTICES}"
        "${SCANENGINE_SDK_DIR}/licenses/onnxruntime"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SCANENGINE_MAGIKA_LICENSE}"
        "${SCANENGINE_SDK_DIR}/licenses/magika/LICENSE.Apache-2.0.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/include/scanengine/scanengine_api.h"
        "${CMAKE_SOURCE_DIR}/include/scanengine/scanengine.hpp"
        "${CMAKE_SOURCE_DIR}/include/scanengine/scanengine_version.h"
        "${CMAKE_SOURCE_DIR}/include/scanengine/scanengine_runtime.h"
        "${SCANENGINE_SDK_DIR}/include/scanengine"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_LINKER_FILE:ScanEngineCore>"
        "$<TARGET_FILE:ScanEngineRuntime>"
        "${SCANENGINE_SDK_DIR}/lib"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_scanengine_sdk_config}"
        "${SCANENGINE_SDK_DIR}/cmake/ScanEngineConfig.cmake"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/docs/SDK_ABI.md"
        "${SCANENGINE_SDK_DIR}/doc/SDK_ABI.md"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/examples/sdk_smoke"
        "${SCANENGINE_SDK_DIR}/examples/sdk_smoke"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/examples/sdk_cpp_smoke"
        "${SCANENGINE_SDK_DIR}/examples/sdk_cpp_smoke"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_scanengine_sdk_stamp}"
    DEPENDS ScanEngineCore ScanEngineRuntime
        "${CMAKE_SOURCE_DIR}/include/scanengine/scanengine_api.h"
        "${CMAKE_SOURCE_DIR}/include/scanengine/scanengine.hpp"
        "${CMAKE_SOURCE_DIR}/include/scanengine/scanengine_version.h"
        "${CMAKE_SOURCE_DIR}/include/scanengine/scanengine_runtime.h"
        "${CMAKE_SOURCE_DIR}/scanengine.json"
        "${SCANENGINE_PDFIUM_RUNTIME}"
        "${SCANENGINE_ORT_RUNTIME}"
        "${SCANENGINE_ORT_PLATFORM_LICENSE}"
        "${SCANENGINE_ORT_PLATFORM_NOTICES}"
        "${SCANENGINE_MAGIKA_LICENSE}"
        ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
        ${_scanengine_cuda_app_private_dlls}
        ${_model_files}
        "${CMAKE_SOURCE_DIR}/docs/SDK_ABI.md"
        "${CMAKE_SOURCE_DIR}/examples/sdk_smoke/CMakeLists.txt"
        "${CMAKE_SOURCE_DIR}/examples/sdk_smoke/main.cpp"
        "${CMAKE_SOURCE_DIR}/examples/sdk_cpp_smoke/CMakeLists.txt"
        "${CMAKE_SOURCE_DIR}/examples/sdk_cpp_smoke/main.cpp"
        "${_scanengine_sdk_config}"
    COMMENT "Packaging ScanEngine external SDK"
    VERBATIM)
add_custom_target(ScanEngineSDK ALL DEPENDS "${_scanengine_sdk_stamp}")
message(STATUS "Automatic deployment: ${SCANENGINE_BIN_DIR}")
