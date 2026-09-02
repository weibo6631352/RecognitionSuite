# Locked ONNX Runtime C API payload used only by Magika code-language inference.
# The runtime is loaded explicitly at first code block; it is never registered
# with the VLM GPU/Metal path and it does not create a second toolchain ABI.

function(scanengine_assert_locked_file path expected_size expected_sha256)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Locked dependency file is missing: ${path}")
    endif()
    file(SIZE "${path}" _actual_size)
    if(NOT _actual_size EQUAL expected_size)
        message(FATAL_ERROR
            "Locked dependency size drifted: ${path}; expected ${expected_size}, actual ${_actual_size}")
    endif()
    file(SHA256 "${path}" _actual_sha256)
    if(NOT _actual_sha256 STREQUAL expected_sha256)
        message(FATAL_ERROR
            "Locked dependency SHA-256 drifted: ${path}; expected ${expected_sha256}, actual ${_actual_sha256}")
    endif()
endfunction()

set(SCANENGINE_ORT_VERSION "1.28.0")
set(SCANENGINE_ORT_COMMON_DIR
    "${SCANENGINE_COMMON_DEPS}/onnxruntime-${SCANENGINE_ORT_VERSION}")
set(SCANENGINE_ORT_INCLUDE_DIR "${SCANENGINE_ORT_COMMON_DIR}/include")
set(SCANENGINE_ORT_LICENSE "${SCANENGINE_ORT_COMMON_DIR}/LICENSE")
set(SCANENGINE_ORT_NOTICES "${SCANENGINE_ORT_COMMON_DIR}/ThirdPartyNotices.txt")
set(SCANENGINE_MAGIKA_MODEL_DIR
    "${CMAKE_SOURCE_DIR}/models/magika/standard_v3_3")
set(SCANENGINE_MAGIKA_LICENSE
    "${SCANENGINE_MAGIKA_MODEL_DIR}/LICENSE.Apache-2.0.txt")

scanengine_assert_locked_file(
    "${SCANENGINE_ORT_INCLUDE_DIR}/onnxruntime_c_api.h" 403673
    "e3a01cdb382b2ba52267bf8a1a40d60b1197a1bac4da55667fe4d84e244f589a")
scanengine_assert_locked_file(
    "${SCANENGINE_ORT_INCLUDE_DIR}/cpu_provider_factory.h" 416
    "e2f658eeace78d79a8df82f233dcdeea135db498a77766156054bfc0d1d06c21")
scanengine_assert_locked_file(
    "${SCANENGINE_ORT_LICENSE}" 1094
    "c250d6278f0b47a6439fb7592b08b58a55eb9f535aa49a1db63211c3f982b674")
scanengine_assert_locked_file(
    "${SCANENGINE_ORT_NOTICES}" 331175
    "fb0af774b4d7cffc5b9d046f2aaeade2f37df2f80abf8033c95dfffcc77a8866")
scanengine_assert_locked_file(
    "${SCANENGINE_MAGIKA_MODEL_DIR}/model.onnx" 3163737
    "fe2d2eb49c5f88a9e0a6c048e15d6ffdf86235519c2afc535044de433169ec8c")
scanengine_assert_locked_file(
    "${SCANENGINE_MAGIKA_MODEL_DIR}/config.min.json" 2142
    "991a6fb760c0431e5c0350a487ba4bd89dee43b74d53b77cc88106469d99617a")
scanengine_assert_locked_file(
    "${SCANENGINE_MAGIKA_LICENSE}" 11357
    "58d1e17ffe5109a7ae296caafcadfdbe6a7d176f0bc4ab01e12a689b0499d8bd")

if(WIN32)
    set(SCANENGINE_ORT_RUNTIME
        "${SCANENGINE_PLATFORM_DEPS}/onnxruntime-${SCANENGINE_ORT_VERSION}/bin/onnxruntime.dll")
    set(SCANENGINE_ORT_IMPORT_LIBRARY
        "${SCANENGINE_PLATFORM_DEPS}/onnxruntime-${SCANENGINE_ORT_VERSION}/lib/onnxruntime.lib")
    set(SCANENGINE_ORT_RUNTIME_NAME "onnxruntime.dll")
    set(SCANENGINE_ORT_RUNTIME_SIZE 15809848)
    set(SCANENGINE_ORT_RUNTIME_SHA256
        "18370c375f07357fa5874344a9d9ac17e6b6fe1eb18b1dd209d79483b4470257")
    scanengine_assert_locked_file(
        "${SCANENGINE_ORT_IMPORT_LIBRARY}" 2124
        "b9fc3cd678257d88a111b0773ede4bfceaf0fe95daab4379f2b2b37348a68781")
    set(SCANENGINE_ORT_PLATFORM_LICENSE "${SCANENGINE_ORT_LICENSE}")
    set(SCANENGINE_ORT_PLATFORM_NOTICES "${SCANENGINE_ORT_NOTICES}")
elseif(APPLE)
    set(SCANENGINE_ORT_RUNTIME
        "${SCANENGINE_PLATFORM_DEPS}/onnxruntime-${SCANENGINE_ORT_VERSION}/lib/libonnxruntime.1.28.0.dylib")
    # The official LC_ID is @rpath/libonnxruntime.1.dylib. deploy-bin copies
    # the one physical, fully-versioned source payload to this major-name.
    set(SCANENGINE_ORT_RUNTIME_NAME "libonnxruntime.1.dylib")
    set(SCANENGINE_ORT_RUNTIME_SIZE 39312136)
    set(SCANENGINE_ORT_RUNTIME_SHA256
        "dc19bbcb2f5c9fb3c68b4f9248aa0a35065ff702c5dbeae75eac54a74da97b6d")
    set(SCANENGINE_ORT_PLATFORM_LICENSE
        "${SCANENGINE_PLATFORM_DEPS}/onnxruntime-${SCANENGINE_ORT_VERSION}/LICENSE")
    set(SCANENGINE_ORT_PLATFORM_NOTICES
        "${SCANENGINE_PLATFORM_DEPS}/onnxruntime-${SCANENGINE_ORT_VERSION}/ThirdPartyNotices.txt")
    scanengine_assert_locked_file(
        "${SCANENGINE_ORT_PLATFORM_LICENSE}" 1073
        "2f07c72751aed99790b8a4869cf2311df85a860b22ded05fa22803587a48922c")
    scanengine_assert_locked_file(
        "${SCANENGINE_ORT_PLATFORM_NOTICES}" 325054
        "0e07b95f3a8d6230037707c5c4a2b554d12c4cb67369669ac255635528ffcee2")
else()
    message(FATAL_ERROR "No locked ONNX Runtime payload for this platform")
endif()

scanengine_assert_locked_file(
    "${SCANENGINE_ORT_RUNTIME}" ${SCANENGINE_ORT_RUNTIME_SIZE}
    "${SCANENGINE_ORT_RUNTIME_SHA256}")

set(SCANENGINE_ORT_COMPILE_DEFINITIONS
    "SCANENGINE_ORT_RUNTIME_SHA256=\"${SCANENGINE_ORT_RUNTIME_SHA256}\""
    "SCANENGINE_ORT_RUNTIME_SIZE=${SCANENGINE_ORT_RUNTIME_SIZE}"
    "SCANENGINE_ORT_RUNTIME_NAME=\"${SCANENGINE_ORT_RUNTIME_NAME}\"")
