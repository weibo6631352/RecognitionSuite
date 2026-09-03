cmake_minimum_required(VERSION 3.25)
project(scanengine_sdk_cpp_smoke LANGUAGES CXX)

find_package(ScanEngine CONFIG REQUIRED)

add_executable(scanengine_sdk_cpp_smoke main.cpp)
target_compile_options(scanengine_sdk_cpp_smoke PRIVATE /W4 /permissive-)
target_link_libraries(scanengine_sdk_cpp_smoke PRIVATE ScanEngine::CXX)
