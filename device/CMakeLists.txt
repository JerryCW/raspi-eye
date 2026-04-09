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

# spdlog（FetchContent）
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.0)
FetchContent_MakeAvailable(spdlog)

# RapidCheck（FetchContent）
FetchContent_Declare(rapidcheck
    GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
    GIT_TAG master
    GIT_SUBMODULES "")
set(RC_ENABLE_GTEST ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(rapidcheck)

# 日志模块库
add_library(log_module STATIC src/json_formatter.cpp src/log_init.cpp)
target_include_directories(log_module PUBLIC src)
target_link_libraries(log_module PUBLIC spdlog::spdlog)

# 主程序库（供测试和 main 共用）
add_library(pipeline_manager STATIC src/pipeline_manager.cpp src/pipeline_builder.cpp src/camera_source.cpp src/pipeline_health.cpp)
target_include_directories(pipeline_manager PUBLIC src ${GST_INCLUDE_DIRS})
target_link_directories(pipeline_manager PUBLIC ${GST_LIBRARY_DIRS})
target_link_libraries(pipeline_manager PUBLIC ${GST_LIBRARIES} log_module)
target_compile_options(pipeline_manager PUBLIC ${GST_CFLAGS_OTHER})

# 可执行文件
add_executable(raspi-eye src/main.cpp)
target_link_libraries(raspi-eye PRIVATE pipeline_manager)

# 测试
enable_testing()
add_executable(smoke_test tests/smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE pipeline_manager GTest::gtest_main)
add_test(NAME smoke_test COMMAND smoke_test)

add_executable(log_test tests/log_test.cpp)
target_link_libraries(log_test PRIVATE log_module GTest::gtest_main rapidcheck rapidcheck_gtest)
add_test(NAME log_test COMMAND log_test)

add_executable(tee_test tests/tee_test.cpp)
target_link_libraries(tee_test PRIVATE pipeline_manager GTest::gtest)
add_test(NAME tee_test COMMAND tee_test)

add_executable(camera_test tests/camera_test.cpp)
target_link_libraries(camera_test PRIVATE pipeline_manager GTest::gtest)
add_test(NAME camera_test COMMAND camera_test)

add_executable(health_test tests/health_test.cpp)
target_link_libraries(health_test PRIVATE pipeline_manager GTest::gtest rapidcheck rapidcheck_gtest)
add_test(NAME health_test COMMAND health_test)
