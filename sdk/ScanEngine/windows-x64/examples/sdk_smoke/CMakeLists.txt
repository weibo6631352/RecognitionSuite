cmake_minimum_required(VERSION 3.25)
project(scanengine_sdk_smoke LANGUAGES CXX)

find_package(ScanEngine CONFIG REQUIRED)

add_executable(scanengine_sdk_smoke main.cpp)
target_compile_features(scanengine_sdk_smoke PRIVATE cxx_std_17)
target_link_libraries(scanengine_sdk_smoke PRIVATE ScanEngine::SDK)
