cmake_minimum_required(VERSION 3.16)
project(raspi-eye LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ASan（Debug 构建）
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

# GStreamer（pkg-config）
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0)

# Google Test（FetchContent）
include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0)
FetchContent_MakeAvailable(googletest)

# 主程序库（供测试和 main 共用）
add_library(pipeline_manager STATIC src/pipeline_manager.cpp)
target_include_directories(pipeline_manager PUBLIC src ${GST_INCLUDE_DIRS})
target_link_directories(pipeline_manager PUBLIC ${GST_LIBRARY_DIRS})
target_link_libraries(pipeline_manager PUBLIC ${GST_LIBRARIES})
target_compile_options(pipeline_manager PUBLIC ${GST_CFLAGS_OTHER})

# 可执行文件
add_executable(raspi-eye src/main.cpp)
target_link_libraries(raspi-eye PRIVATE pipeline_manager)

# 测试
enable_testing()
add_executable(smoke_test tests/smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE pipeline_manager GTest::gtest_main)
add_test(NAME smoke_test COMMAND smoke_test)
