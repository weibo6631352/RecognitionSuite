# Build-time macOS helper for deploy-bin. It collects Qt frameworks, fixes the
# flattened bin/ directory's load paths, and applies ad-hoc signatures.

cmake_minimum_required(VERSION 3.21)

if(SCANENGINE_MACOS_DEPLOY)
    foreach(_required BIN_DIR GUI_EXE TOOL_EXE MODEL_MANIFEST ICON_FILE PDFIUM_RUNTIME
                      ORT_RUNTIME ORT_LICENSE ORT_NOTICES MAGIKA_LICENSE)
        if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
            message(FATAL_ERROR "DeployMacOS: missing -D${_required}=...")
        endif()
    endforeach()
    foreach(_source GUI_EXE TOOL_EXE MODEL_MANIFEST ICON_FILE PDFIUM_RUNTIME
                    ORT_RUNTIME ORT_LICENSE ORT_NOTICES MAGIKA_LICENSE)
        if(NOT EXISTS "${${_source}}")
            message(FATAL_ERROR "DeployMacOS: missing ${_source}: ${${_source}}")
        endif()
    endforeach()

    file(MAKE_DIRECTORY "${BIN_DIR}" "${BIN_DIR}/output")

    function(copy_file source destination)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${source}" "${destination}"
            RESULT_VARIABLE _result)
        if(NOT _result EQUAL 0)
            message(FATAL_ERROR "DeployMacOS: failed to copy ${source} to ${destination}")
        endif()
    endfunction()

    function(run_checked label)
        execute_process(
            COMMAND ${ARGN}
            RESULT_VARIABLE _result
            OUTPUT_VARIABLE _stdout
            ERROR_VARIABLE _stderr)
        if(NOT _result EQUAL 0)
            message(FATAL_ERROR
                "DeployMacOS: ${label} failed (${_result})\n${_stdout}\n${_stderr}")
        endif()
    endfunction()

    function(rewrite_macos_qt_path file_path)
        execute_process(
            COMMAND otool -L "${file_path}"
            RESULT_VARIABLE _result
            OUTPUT_VARIABLE _links
            ERROR_QUIET)
        if(NOT _result EQUAL 0)
            return()
        endif()
        string(REPLACE "\n" ";" _lines "${_links}")
        foreach(_line IN LISTS _lines)
            string(STRIP "${_line}" _line)
            if(_line MATCHES "^(@(executable|loader)_path/\\.\\./Frameworks/[^ ]+)")
                set(_old "${CMAKE_MATCH_1}")
                string(REPLACE "/../Frameworks" "/Frameworks" _new "${_old}")
                run_checked("rewrite Qt path in ${file_path}"
                    install_name_tool -change "${_old}" "${_new}" "${file_path}")
            endif()
        endforeach()
    endfunction()

    function(normalize_macos_rpath file_path)
        execute_process(
            COMMAND otool -l "${file_path}"
            RESULT_VARIABLE _result
            OUTPUT_VARIABLE _load_commands
            ERROR_VARIABLE _stderr)
        if(NOT _result EQUAL 0)
            message(FATAL_ERROR "DeployMacOS: failed to inspect ${file_path}\n${_stderr}")
        endif()
        foreach(_old "@loader_path/../Frameworks" "@executable_path/../Frameworks")
            string(FIND "${_load_commands}" "path ${_old} " _position)
            if(NOT _position EQUAL -1)
                run_checked("remove app-bundle rpath from ${file_path}"
                    install_name_tool -delete_rpath "${_old}" "${file_path}")
            endif()
        endforeach()
        string(FIND "${_load_commands}" "path @executable_path/Frameworks " _position)
        if(_position EQUAL -1)
            run_checked("add flattened runtime rpath to ${file_path}"
                install_name_tool -add_rpath "@executable_path/Frameworks" "${file_path}")
        endif()
    endfunction()

    if(NOT DEFINED QT_DEPLOY_TOOL OR NOT EXISTS "${QT_DEPLOY_TOOL}")
        message(FATAL_ERROR "DeployMacOS: macdeployqt not found: ${QT_DEPLOY_TOOL}")
    endif()

    set(_app_root "${BIN_DIR}/.deploy-tmp")
    set(_app "${_app_root}/ScanEngine.app")
    file(REMOVE_RECURSE "${_app_root}")
    file(MAKE_DIRECTORY "${_app}/Contents/MacOS" "${_app}/Contents/Resources")
    file(WRITE "${_app}/Contents/Info.plist"
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict>\n"
        "<key>CFBundleExecutable</key><string>ScanEngine</string>\n"
        "<key>CFBundleIdentifier</key><string>local.scanengine</string>\n"
        "<key>CFBundlePackageType</key><string>APPL</string>\n"
        "<key>CFBundleIconFile</key><string>ScanEngine</string>\n"
        "</dict></plist>\n")
    copy_file("${GUI_EXE}" "${_app}/Contents/MacOS/ScanEngine")
    copy_file("${TOOL_EXE}" "${_app}/Contents/MacOS/ScanEngineTool")
    copy_file("${ICON_FILE}" "${_app}/Contents/Resources/ScanEngine.icns")
    run_checked("macdeployqt"
        "${QT_DEPLOY_TOOL}" "${_app}"
        "-executable=${_app}/Contents/MacOS/ScanEngineTool" -no-strip)

    if(NOT IS_DIRECTORY "${_app}/Contents/Frameworks" OR
       NOT IS_DIRECTORY "${_app}/Contents/PlugIns")
        message(FATAL_ERROR "DeployMacOS: macdeployqt produced an incomplete app bundle")
    endif()
    file(REMOVE_RECURSE "${BIN_DIR}/Frameworks" "${BIN_DIR}/PlugIns")
    file(COPY "${_app}/Contents/Frameworks/" DESTINATION "${BIN_DIR}/Frameworks")
    file(COPY "${_app}/Contents/PlugIns/" DESTINATION "${BIN_DIR}/PlugIns")
    copy_file("${_app}/Contents/MacOS/ScanEngine" "${BIN_DIR}/ScanEngine")
    copy_file("${_app}/Contents/MacOS/ScanEngineTool" "${BIN_DIR}/ScanEngineTool")
    copy_file("${ICON_FILE}" "${BIN_DIR}/ScanEngine.icns")
    copy_file("${PDFIUM_RUNTIME}" "${BIN_DIR}/Frameworks/libpdfium.dylib")
    copy_file("${ORT_RUNTIME}" "${BIN_DIR}/Frameworks/libonnxruntime.1.dylib")
    file(MAKE_DIRECTORY "${BIN_DIR}/licenses/onnxruntime"
                        "${BIN_DIR}/licenses/magika")
    copy_file("${ORT_LICENSE}" "${BIN_DIR}/licenses/onnxruntime/LICENSE")
    copy_file("${ORT_NOTICES}"
              "${BIN_DIR}/licenses/onnxruntime/ThirdPartyNotices.txt")
    copy_file("${MAGIKA_LICENSE}"
              "${BIN_DIR}/licenses/magika/LICENSE.Apache-2.0.txt")

    if(DEFINED MLX_LIBRARY AND NOT "${MLX_LIBRARY}" STREQUAL "")
        if(NOT EXISTS "${MLX_LIBRARY}" OR
           NOT DEFINED MLX_METALLIB OR NOT EXISTS "${MLX_METALLIB}")
            message(FATAL_ERROR "DeployMacOS: incomplete MLX deployment inputs")
        endif()
        copy_file("${MLX_LIBRARY}" "${BIN_DIR}/Frameworks/libmlx.dylib")
        copy_file("${MLX_METALLIB}" "${BIN_DIR}/Frameworks/mlx.metallib")
    endif()
    file(WRITE "${BIN_DIR}/qt.conf" "[Paths]\nPlugins = PlugIns\n")

    set(_rewrite_files "${BIN_DIR}/ScanEngine" "${BIN_DIR}/ScanEngineTool")
    file(GLOB_RECURSE _deployed_libraries LIST_DIRECTORIES false
        "${BIN_DIR}/Frameworks/*.dylib"
        "${BIN_DIR}/Frameworks/*.framework/*"
        "${BIN_DIR}/PlugIns/*.dylib")
    list(APPEND _rewrite_files ${_deployed_libraries})
    foreach(_file IN LISTS _rewrite_files)
        rewrite_macos_qt_path("${_file}")
    endforeach()

    foreach(_exe "${BIN_DIR}/ScanEngine" "${BIN_DIR}/ScanEngineTool")
        normalize_macos_rpath("${_exe}")
    endforeach()

    file(GLOB _frameworks LIST_DIRECTORIES true "${BIN_DIR}/Frameworks/*.framework")
    file(GLOB_RECURSE _sign_files LIST_DIRECTORIES false
        "${BIN_DIR}/Frameworks/*.dylib" "${BIN_DIR}/PlugIns/*.dylib")
    # Preserve Microsoft's signed ONNX Runtime bytes: runtime identity is
    # checked before first code inference, and ad-hoc re-signing would change
    # both its LC_CODE_SIGNATURE payload and locked SHA-256.
    list(FILTER _sign_files EXCLUDE REGEX
        "/Frameworks/libonnxruntime\\.1\\.dylib$")
    foreach(_item IN LISTS _frameworks _sign_files)
        run_checked("sign ${_item}"
            codesign --force --deep --sign - --timestamp=none "${_item}")
    endforeach()
    run_checked("sign ScanEngine"
        codesign --force --sign - --timestamp=none "${BIN_DIR}/ScanEngine")
    run_checked("sign ScanEngineTool"
        codesign --force --sign - --timestamp=none "${BIN_DIR}/ScanEngineTool")
    file(REMOVE_RECURSE "${_app_root}")

    copy_file("${MODEL_MANIFEST}" "${BIN_DIR}/scanengine.json")
    message(STATUS "Deployed runnable macOS product directory: ${BIN_DIR}")
    return()
endif()

# Normal configure mode -------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_OSX_DEPLOYMENT_TARGET OR
   CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "14.0")
    message(FATAL_ERROR
        "Locked ONNX Runtime 1.28.0 arm64 requires macOS deployment target >= 14.0")
endif()

set(_scanengine_arch "${CMAKE_OSX_ARCHITECTURES}")
if(NOT _scanengine_arch)
    set(_scanengine_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(NOT _scanengine_arch STREQUAL "arm64")
    message(FATAL_ERROR
        "Locked ONNX Runtime 1.28.0 payload supports native macOS arm64 only; actual architecture: ${_scanengine_arch}")
endif()
set(SCANENGINE_DEPENDENCY_PLATFORM "macos-${_scanengine_arch}")
set(SCANENGINE_COMMON_DEPS "${CMAKE_SOURCE_DIR}/third_party/common")
set(SCANENGINE_PLATFORM_DEPS
    "${CMAKE_SOURCE_DIR}/third_party/${SCANENGINE_DEPENDENCY_PLATFORM}")
if(NOT IS_DIRECTORY "${SCANENGINE_PLATFORM_DEPS}")
    message(FATAL_ERROR
        "Repository dependency bundle is missing: third_party/${SCANENGINE_DEPENDENCY_PLATFORM}")
endif()
message(STATUS "ScanEngine platform: macOS ${_scanengine_arch}")
include("${CMAKE_SOURCE_DIR}/cmake/OnnxRuntime.cmake")

set(SCANENGINE_RAW_DIR "${CMAKE_BINARY_DIR}/raw")
set(SCANENGINE_BIN_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${SCANENGINE_RAW_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

set(SCANENGINE_MACOS_QT_ROOT "${SCANENGINE_MACOS_QT_ROOT}" CACHE PATH
    "Native arm64 Qt 5.15.2 prefix")
if(NOT SCANENGINE_MACOS_QT_ROOT OR
   NOT EXISTS "${SCANENGINE_MACOS_QT_ROOT}/lib/cmake/Qt5/Qt5Config.cmake")
    message(FATAL_ERROR
        "Set SCANENGINE_MACOS_QT_ROOT to the locked native arm64 Qt 5.15.2 prefix; "
        "Homebrew and unverified Qt kits are not formal macOS product inputs")
endif()
list(PREPEND CMAKE_PREFIX_PATH "${SCANENGINE_MACOS_QT_ROOT}")
find_package(Qt5 5.15.2 EXACT REQUIRED COMPONENTS Core Gui Widgets)
message(STATUS "Using Qt ${Qt5_VERSION} at ${Qt5_DIR}")
if(NOT Qt5_VERSION STREQUAL "5.15.2")
    message(FATAL_ERROR "Formal macOS preset requires the locked Qt 5.15.2 kit")
endif()
get_target_property(_scanengine_qtcore Qt5::Core IMPORTED_LOCATION_RELEASE)
if(NOT _scanengine_qtcore)
    get_target_property(_scanengine_qtcore Qt5::Core IMPORTED_LOCATION)
endif()
if(NOT EXISTS "${_scanengine_qtcore}")
    message(FATAL_ERROR "Locked Qt Core framework location is unavailable")
endif()
execute_process(
    COMMAND lipo -archs "${_scanengine_qtcore}"
    RESULT_VARIABLE _scanengine_qt_arch_result
    OUTPUT_VARIABLE _scanengine_qt_arches
    ERROR_VARIABLE _scanengine_qt_arch_error)
string(STRIP "${_scanengine_qt_arches}" _scanengine_qt_arches)
if(NOT _scanengine_qt_arch_result EQUAL 0 OR
   NOT _scanengine_qt_arches STREQUAL "arm64")
    message(FATAL_ERROR
        "Locked Qt Core must be exactly native arm64; got '${_scanengine_qt_arches}' "
        "(${_scanengine_qt_arch_error})")
endif()

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
set_target_properties(scanengine_fasttext_static PROPERTIES
    AUTOMOC OFF
    POSITION_INDEPENDENT_CODE ON)
target_compile_features(scanengine_fasttext_static PUBLIC cxx_std_17)
target_include_directories(scanengine_fasttext_static SYSTEM PUBLIC
    "${_fasttext_root}/src")

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
target_compile_options(scanengine_core PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_executable(ScanEngine
    src/app/main.cpp
    src/ui/main_window.cpp
    src/ui/parse_controller.cpp
    src/ui/widgets/control_panel.cpp
    src/ui/widgets/drop_zone.cpp
    src/ui/widgets/preview_pane.cpp
    src/ui/widgets/result_pane.cpp
    src/ui/widgets/status_panel.cpp
    resources/resources.qrc)
set_target_properties(ScanEngine PROPERTIES AUTOMOC ON AUTORCC ON)
target_include_directories(ScanEngine PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(ScanEngine PRIVATE scanengine_core Qt5::Widgets)

add_executable(ScanEngineTool src/app/contract_cli.cpp)
target_link_libraries(ScanEngineTool PRIVATE scanengine_core)

set(SCANENGINE_PDFIUM_DIR "${SCANENGINE_PLATFORM_DEPS}/pdfium" CACHE PATH
    "Repository directory containing the macOS pdfium runtime")
set(_pdfium_include "${SCANENGINE_COMMON_DEPS}/pdfium/include")
if(NOT EXISTS "${_pdfium_include}/fpdfview_min.h")
    message(FATAL_ERROR "Missing ${_pdfium_include}/fpdfview_min.h")
endif()
find_library(SCANENGINE_PDFIUM_RUNTIME NAMES pdfium libpdfium
    HINTS "${SCANENGINE_PDFIUM_DIR}" NO_DEFAULT_PATH)
if(NOT SCANENGINE_PDFIUM_RUNTIME)
    message(FATAL_ERROR "Repository pdfium runtime not found under ${SCANENGINE_PDFIUM_DIR}")
endif()
target_include_directories(scanengine_core SYSTEM PRIVATE "${_pdfium_include}")
target_link_libraries(scanengine_core PRIVATE "${SCANENGINE_PDFIUM_RUNTIME}")

set(_mlx_default OFF)
if(_scanengine_arch STREQUAL "arm64")
    set(_mlx_default ON)
endif()
option(SCANENGINE_ENABLE_MLX "Enable the official MLX Metal backend" ${_mlx_default})
set(SCANENGINE_MLX_DIR "${SCANENGINE_PLATFORM_DEPS}/mlx" CACHE PATH
    "Repository directory containing the macOS MLX runtime")
if(SCANENGINE_ENABLE_MLX)
    # The byte-locked repository libmlx.dylib has LC_BUILD_VERSION minos 26.0.
    # ORT alone supports 14.0, but a product that links MLX must advertise the
    # maximum deployment target of every required Mach-O payload.
    if(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "26.0")
        message(FATAL_ERROR
            "Locked MLX runtime requires macOS deployment target >= 26.0; use the no-MLX macos-qt5 preset for the ORT-only macOS 14 development build")
    endif()
    find_file(SCANENGINE_MLX_LIBRARY NAMES libmlx.dylib
        HINTS "${SCANENGINE_MLX_DIR}/lib" NO_DEFAULT_PATH)
    find_file(SCANENGINE_MLX_METALLIB NAMES mlx.metallib
        HINTS "${SCANENGINE_MLX_DIR}/lib" NO_DEFAULT_PATH)
    if(NOT EXISTS "${SCANENGINE_MLX_DIR}/include/mlx/mlx.h" OR
       NOT SCANENGINE_MLX_LIBRARY OR NOT SCANENGINE_MLX_METALLIB)
        message(FATAL_ERROR "Incomplete MLX runtime under ${SCANENGINE_MLX_DIR}")
    endif()
    target_compile_definitions(scanengine_core PRIVATE SCANENGINE_MLX=1)
    target_include_directories(scanengine_core SYSTEM PRIVATE "${SCANENGINE_MLX_DIR}/include")
    target_link_libraries(scanengine_core PRIVATE "${SCANENGINE_MLX_LIBRARY}")
else()
    set(SCANENGINE_MLX_LIBRARY "")
    set(SCANENGINE_MLX_METALLIB "")
endif()

find_library(ACCELERATE_LIBRARY Accelerate REQUIRED)
target_link_libraries(scanengine_core PRIVATE "${ACCELERATE_LIBRARY}")

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

set_target_properties(ScanEngine ScanEngineTool PROPERTIES
    BUILD_RPATH "${SCANENGINE_MLX_DIR}/lib;${SCANENGINE_PDFIUM_DIR}")

get_target_property(_qt_qmake Qt5::qmake IMPORTED_LOCATION)
get_filename_component(_qt_bin_dir "${_qt_qmake}" DIRECTORY)
set(_qt_deploy_tool "${_qt_bin_dir}/macdeployqt")
if(NOT EXISTS "${_qt_deploy_tool}")
    message(FATAL_ERROR "macdeployqt not found: ${_qt_deploy_tool}")
endif()

set(_runtime_stamp "${SCANENGINE_BIN_DIR}/.runtime.stamp")
set(_runtime_dependencies
    ScanEngine
    ScanEngineTool
    "${CMAKE_SOURCE_DIR}/scanengine.json"
    "${CMAKE_SOURCE_DIR}/resources/icons/scanengine.icns"
    "${CMAKE_CURRENT_LIST_FILE}"
    "${SCANENGINE_PDFIUM_RUNTIME}"
    "${SCANENGINE_ORT_RUNTIME}"
    "${SCANENGINE_ORT_PLATFORM_LICENSE}"
    "${SCANENGINE_ORT_PLATFORM_NOTICES}"
    "${SCANENGINE_MAGIKA_LICENSE}")
if(SCANENGINE_ENABLE_MLX)
    list(APPEND _runtime_dependencies
        "${SCANENGINE_MLX_LIBRARY}" "${SCANENGINE_MLX_METALLIB}")
endif()
add_custom_command(
    OUTPUT "${_runtime_stamp}"
    COMMAND "${CMAKE_COMMAND}"
        -DSCANENGINE_MACOS_DEPLOY=ON
        "-DBIN_DIR=${SCANENGINE_BIN_DIR}"
        "-DGUI_EXE=$<TARGET_FILE:ScanEngine>"
        "-DTOOL_EXE=$<TARGET_FILE:ScanEngineTool>"
        "-DMODEL_MANIFEST=${CMAKE_SOURCE_DIR}/scanengine.json"
        "-DICON_FILE=${CMAKE_SOURCE_DIR}/resources/icons/scanengine.icns"
        "-DQT_DEPLOY_TOOL=${_qt_deploy_tool}"
        "-DPDFIUM_RUNTIME=${SCANENGINE_PDFIUM_RUNTIME}"
        "-DORT_RUNTIME=${SCANENGINE_ORT_RUNTIME}"
        "-DORT_LICENSE=${SCANENGINE_ORT_PLATFORM_LICENSE}"
        "-DORT_NOTICES=${SCANENGINE_ORT_PLATFORM_NOTICES}"
        "-DMAGIKA_LICENSE=${SCANENGINE_MAGIKA_LICENSE}"
        "-DMLX_LIBRARY=${SCANENGINE_MLX_LIBRARY}"
        "-DMLX_METALLIB=${SCANENGINE_MLX_METALLIB}"
        -P "${CMAKE_CURRENT_LIST_FILE}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_runtime_stamp}"
    DEPENDS ${_runtime_dependencies}
    COMMENT "Deploying macOS runtime into bin/"
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
message(STATUS "Automatic deployment: ${SCANENGINE_BIN_DIR}")
