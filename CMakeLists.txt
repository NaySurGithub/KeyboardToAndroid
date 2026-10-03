cmake_minimum_required(VERSION 3.16)
project(KeyboardToAndroid LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(KeyboardToAndroid src/main.cpp)

if(MINGW)
    target_link_options(KeyboardToAndroid PRIVATE -static)
endif()
