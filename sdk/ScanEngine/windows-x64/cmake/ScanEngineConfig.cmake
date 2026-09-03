if(TARGET ScanEngine::SDK)
    return()
endif()

get_filename_component(_scanengine_sdk_root
    "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

add_library(ScanEngine::Core SHARED IMPORTED)
set_target_properties(ScanEngine::Core PROPERTIES
    IMPORTED_LOCATION "${_scanengine_sdk_root}/bin/ScanEngineCore.dll"
    IMPORTED_IMPLIB "${_scanengine_sdk_root}/lib/ScanEngineCore.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${_scanengine_sdk_root}/include")

add_library(ScanEngine::Runtime STATIC IMPORTED)
set_target_properties(ScanEngine::Runtime PROPERTIES
    IMPORTED_LOCATION "${_scanengine_sdk_root}/lib/ScanEngineRuntime.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${_scanengine_sdk_root}/include")

add_library(ScanEngine::SDK INTERFACE IMPORTED)
set_target_properties(ScanEngine::SDK PROPERTIES
    INTERFACE_LINK_LIBRARIES "ScanEngine::Runtime;ScanEngine::Core;delayimp"
    INTERFACE_LINK_OPTIONS "/DELAYLOAD:ScanEngineCore.dll")

add_library(ScanEngine::CXX INTERFACE IMPORTED)
set_target_properties(ScanEngine::CXX PROPERTIES
    INTERFACE_LINK_LIBRARIES "ScanEngine::SDK"
    INTERFACE_COMPILE_FEATURES "cxx_std_17")

set(SCANENGINE_RUNTIME_ROOT "${_scanengine_sdk_root}/bin")
