set(VOICEENGINE_FFMPEG_ROOT
    "${CMAKE_SOURCE_DIR}/third_party/windows-x64/ffmpeg" CACHE PATH
    "Pinned FFmpeg LGPL shared Windows x64 bundle")

foreach(_h libavcodec/avcodec.h libavformat/avformat.h libavutil/avutil.h libswresample/swresample.h)
    if(NOT EXISTS "${VOICEENGINE_FFMPEG_ROOT}/include/${_h}")
        message(FATAL_ERROR "FFmpeg header missing: ${VOICEENGINE_FFMPEG_ROOT}/include/${_h}")
    endif()
endforeach()
foreach(_lib avcodec avformat avutil swresample)
    if(NOT EXISTS "${VOICEENGINE_FFMPEG_ROOT}/lib/${_lib}.lib")
        message(FATAL_ERROR "FFmpeg import lib missing: ${VOICEENGINE_FFMPEG_ROOT}/lib/${_lib}.lib")
    endif()
endforeach()

set(VOICEENGINE_FFMPEG_DLLS
    "${VOICEENGINE_FFMPEG_ROOT}/bin/avcodec-62.dll"
    "${VOICEENGINE_FFMPEG_ROOT}/bin/avformat-62.dll"
    "${VOICEENGINE_FFMPEG_ROOT}/bin/avutil-60.dll"
    "${VOICEENGINE_FFMPEG_ROOT}/bin/swresample-6.dll")
foreach(_dll IN LISTS VOICEENGINE_FFMPEG_DLLS)
    if(NOT EXISTS "${_dll}")
        message(FATAL_ERROR "FFmpeg runtime DLL missing: ${_dll}")
    endif()
endforeach()

add_library(voiceengine_ffmpeg INTERFACE)
target_include_directories(voiceengine_ffmpeg INTERFACE "${VOICEENGINE_FFMPEG_ROOT}/include")
target_link_directories(voiceengine_ffmpeg INTERFACE "${VOICEENGINE_FFMPEG_ROOT}/lib")
# These pinned GNU-style import libraries produce normal PE imports under
# MSVC; /DELAYLOAD is ignored for them. VoiceEngineCore.dll itself is
# delay-loaded, after the bootstrap adds runtimes/ffmpeg to the DLL search
# path, so the FFmpeg DLLs remain application-private without invalid flags.
target_link_libraries(voiceengine_ffmpeg INTERFACE
    avcodec avformat avutil swresample)

message(STATUS "FFmpeg: ${VOICEENGINE_FFMPEG_ROOT} (avcodec/avformat/avutil/swresample only)")
