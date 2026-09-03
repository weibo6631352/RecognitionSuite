cmake_minimum_required(VERSION 3.25)
project(voiceengine_sdk_cpp_smoke LANGUAGES CXX)

find_package(VoiceEngine CONFIG REQUIRED)

add_executable(voiceengine_sdk_cpp_smoke main.cpp)
target_compile_options(voiceengine_sdk_cpp_smoke PRIVATE /W4 /permissive-)
target_link_libraries(voiceengine_sdk_cpp_smoke PRIVATE VoiceEngine::CXX)
