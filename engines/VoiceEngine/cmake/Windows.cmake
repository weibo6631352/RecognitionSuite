set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC OR NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(FATAL_ERROR
        "Windows requires the MSVC toolchain (same as ScanEngine); actual compilers are "
        "${CMAKE_C_COMPILER_ID} and ${CMAKE_CXX_COMPILER_ID}")
endif()
if(NOT CMAKE_GENERATOR STREQUAL "Visual Studio 17 2022" OR
   NOT MSVC_TOOLSET_VERSION EQUAL 143)
    message(FATAL_ERROR
        "Windows requires Visual Studio 17 2022 with the v143 x64 toolset (same as ScanEngine); "
        "actual generator/toolset: ${CMAKE_GENERATOR} / ${MSVC_TOOLSET_VERSION}")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Windows requires the x64 architecture")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>")

set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
set(CMAKE_INSTALL_OPENMP_LIBRARIES TRUE)
include(InstallRequiredSystemLibraries)
if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
    message(FATAL_ERROR "Cannot locate the MSVC runtime deployment files")
endif()

set(VOICEENGINE_QT_ROOT "C:/Qt/Qt5.12.9/5.12.9/msvc2017_64" CACHE PATH
    "Exact Qt 5.12.9 msvc2017_64 SDK root (same as ScanEngine)")
if(NOT EXISTS "${VOICEENGINE_QT_ROOT}/lib/cmake/Qt5/Qt5Config.cmake")
    message(FATAL_ERROR "Qt 5.12.9 msvc2017_64 is missing under ${VOICEENGINE_QT_ROOT}")
endif()
list(PREPEND CMAKE_PREFIX_PATH "${VOICEENGINE_QT_ROOT}")
find_package(Qt5 5.12.9 EXACT REQUIRED COMPONENTS Core Gui Widgets)
message(STATUS "Using Qt ${Qt5_VERSION} at ${Qt5_DIR}")

set(VOICEENGINE_CUDA_ROOT
    "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.9" CACHE PATH
    "CUDA 12.9 toolkit (same as ScanEngine)")
if(NOT EXISTS "${VOICEENGINE_CUDA_ROOT}/bin/nvcc.exe")
    message(FATAL_ERROR "CUDA 12.9 nvcc is missing under ${VOICEENGINE_CUDA_ROOT}")
endif()

set(VOICEENGINE_BIN_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${VOICEENGINE_BIN_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
