cmake_minimum_required(VERSION 3.25)
project(voiceengine_sdk_smoke LANGUAGES CXX)

find_package(VoiceEngine CONFIG REQUIRED)

add_executable(voiceengine_sdk_smoke main.cpp)
target_compile_features(voiceengine_sdk_smoke PRIVATE cxx_std_17)
target_link_libraries(voiceengine_sdk_smoke PRIVATE VoiceEngine::SDK)
