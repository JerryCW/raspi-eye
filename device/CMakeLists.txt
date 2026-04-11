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
pkg_check_modules(GST_APP REQUIRED gstreamer-app-1.0)

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

# nlohmann/json（FetchContent，header-only）
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

# libcurl（系统包）
find_package(CURL REQUIRED)

# 日志模块库
add_library(log_module STATIC src/json_formatter.cpp src/log_init.cpp)
target_include_directories(log_module PUBLIC src)
target_link_libraries(log_module PUBLIC spdlog::spdlog)

# 主程序库（供测试和 main 共用）
add_library(pipeline_manager STATIC src/pipeline_manager.cpp src/pipeline_builder.cpp src/camera_source.cpp src/pipeline_health.cpp src/shutdown_handler.cpp src/app_context.cpp)
target_include_directories(pipeline_manager PUBLIC src ${GST_INCLUDE_DIRS})
target_link_directories(pipeline_manager PUBLIC ${GST_LIBRARY_DIRS})
target_link_libraries(pipeline_manager PUBLIC ${GST_LIBRARIES} log_module)
target_compile_options(pipeline_manager PUBLIC ${GST_CFLAGS_OTHER})

# GST_APP include/link for pipeline_manager (appsink support)
target_include_directories(pipeline_manager PUBLIC ${GST_APP_INCLUDE_DIRS})
target_link_directories(pipeline_manager PUBLIC ${GST_APP_LIBRARY_DIRS})
target_link_libraries(pipeline_manager PUBLIC ${GST_APP_LIBRARIES})

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

add_executable(shutdown_test tests/shutdown_test.cpp)
target_link_libraries(shutdown_test PRIVATE pipeline_manager GTest::gtest_main rapidcheck rapidcheck_gtest)
add_test(NAME shutdown_test COMMAND shutdown_test)

# 凭证模块库
add_library(credential_module STATIC src/credential_provider.cpp)
target_include_directories(credential_module PUBLIC src)
target_link_libraries(credential_module PUBLIC
    nlohmann_json::nlohmann_json
    CURL::libcurl
    spdlog::spdlog
    log_module)

# 凭证测试
add_executable(credential_test tests/credential_test.cpp)
target_link_libraries(credential_test PRIVATE
    credential_module
    GTest::gtest_main
    rapidcheck
    rapidcheck_gtest)
add_test(NAME credential_test COMMAND credential_test
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

# KVS sink factory module
add_library(kvs_module STATIC src/kvs_sink_factory.cpp)
target_include_directories(kvs_module PUBLIC src ${GST_INCLUDE_DIRS})
target_link_directories(kvs_module PUBLIC ${GST_LIBRARY_DIRS})
target_link_libraries(kvs_module PUBLIC ${GST_LIBRARIES} spdlog::spdlog credential_module)

# Link kvs_module to pipeline_manager
target_link_libraries(pipeline_manager PUBLIC kvs_module)

# KVS test
add_executable(kvs_test tests/kvs_test.cpp)
target_link_libraries(kvs_test PRIVATE
    kvs_module
    pipeline_manager
    GTest::gtest
    rapidcheck
    rapidcheck_gtest)
add_test(NAME kvs_test COMMAND kvs_test
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

# YOLO detector module (optional, requires ONNX Runtime)
option(ENABLE_YOLO "Build YOLO detector module (requires ONNX Runtime)" ON)

if(ENABLE_YOLO)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    find_package(OnnxRuntime QUIET)

    if(OnnxRuntime_FOUND)
        set(YOLO_MODEL_PATH_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/models/yolo11s.onnx")
        set(YOLO_MODEL_PATH_NANO "${CMAKE_CURRENT_SOURCE_DIR}/models/yolo11n.onnx")
        set(YOLO_MODEL_PATH_SMALL_INT8 "${CMAKE_CURRENT_SOURCE_DIR}/models/yolo11s-int8.onnx")
        set(YOLO_MODEL_PATH_NANO_INT8 "${CMAKE_CURRENT_SOURCE_DIR}/models/yolo11n-int8.onnx")

        add_library(yolo_module STATIC src/yolo_detector.cpp)
        target_include_directories(yolo_module PUBLIC src ${OnnxRuntime_INCLUDE_DIRS})
        target_link_libraries(yolo_module PUBLIC ${OnnxRuntime_LIBRARIES} log_module)

        add_executable(yolo_test tests/yolo_test.cpp)
        target_link_libraries(yolo_test PRIVATE yolo_module GTest::gtest_main
                              rapidcheck rapidcheck_gtest)
        target_compile_definitions(yolo_test PRIVATE
            YOLO_MODEL_PATH_SMALL="${YOLO_MODEL_PATH_SMALL}"
            YOLO_MODEL_PATH_NANO="${YOLO_MODEL_PATH_NANO}"
            YOLO_MODEL_PATH_SMALL_INT8="${YOLO_MODEL_PATH_SMALL_INT8}"
            YOLO_MODEL_PATH_NANO_INT8="${YOLO_MODEL_PATH_NANO_INT8}")
        add_test(NAME yolo_test COMMAND yolo_test)

        message(STATUS "YOLO module: ENABLED (ONNX Runtime found)")
    else()
        message(STATUS "YOLO module: DISABLED (ONNX Runtime not found)")
    endif()
else()
    message(STATUS "YOLO module: DISABLED (ENABLE_YOLO=OFF)")
endif()

# WebRTC signaling module
add_library(webrtc_module STATIC src/webrtc_signaling.cpp)
target_include_directories(webrtc_module PUBLIC src)
target_link_libraries(webrtc_module PUBLIC spdlog::spdlog credential_module)

# WebRTC media module
add_library(webrtc_media_module STATIC src/webrtc_media.cpp)
target_include_directories(webrtc_media_module PUBLIC src)
target_link_libraries(webrtc_media_module PUBLIC spdlog::spdlog webrtc_module)

# Link webrtc_media_module to pipeline_manager
target_link_libraries(pipeline_manager PUBLIC webrtc_media_module)

# Linux: find KVS WebRTC C SDK
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # User can specify SDK path via -DKVS_WEBRTC_SDK_DIR=/path/to/sdk
    set(KVS_WEBRTC_SEARCH_PATHS /usr/local/lib /opt/kvs-webrtc/lib)
    set(KVS_WEBRTC_INCLUDE_SEARCH_PATHS /usr/local/include /opt/kvs-webrtc/include)
    # Also search SDK open-source/lib for transitive dependencies (kvsCommonLws, libwebsockets)
    set(KVS_WEBRTC_OPEN_SOURCE_LIB_PATHS)
    if(KVS_WEBRTC_SDK_DIR)
        list(INSERT KVS_WEBRTC_SEARCH_PATHS 0 "${KVS_WEBRTC_SDK_DIR}/lib")
        list(INSERT KVS_WEBRTC_INCLUDE_SEARCH_PATHS 0 "${KVS_WEBRTC_SDK_DIR}/include")
    endif()

    find_library(KVS_WEBRTC_SIGNALING_LIB kvsWebrtcSignalingClient
        PATHS ${KVS_WEBRTC_SEARCH_PATHS}
        NO_DEFAULT_PATH)
    find_library(KVS_WEBRTC_CLIENT_LIB kvsWebrtcClient
        PATHS ${KVS_WEBRTC_SEARCH_PATHS}
        NO_DEFAULT_PATH)
    find_library(KVS_COMMON_LWS_LIB kvsCommonLws
        PATHS ${KVS_WEBRTC_SEARCH_PATHS}
        NO_DEFAULT_PATH)
    find_path(KVS_WEBRTC_INCLUDE_DIR
        com/amazonaws/kinesis/video/webrtcclient/Include.h
        PATHS ${KVS_WEBRTC_INCLUDE_SEARCH_PATHS}
        NO_DEFAULT_PATH)

    if(KVS_WEBRTC_SIGNALING_LIB AND KVS_WEBRTC_CLIENT_LIB AND KVS_WEBRTC_INCLUDE_DIR)
        # Collect all SDK libs (kvsCommonLws may or may not be found separately)
        set(KVS_WEBRTC_ALL_LIBS ${KVS_WEBRTC_SIGNALING_LIB} ${KVS_WEBRTC_CLIENT_LIB})
        if(KVS_COMMON_LWS_LIB)
            list(APPEND KVS_WEBRTC_ALL_LIBS ${KVS_COMMON_LWS_LIB})
        endif()

        target_compile_definitions(webrtc_module PRIVATE HAVE_KVS_WEBRTC_SDK=1)
        target_include_directories(webrtc_module PRIVATE ${KVS_WEBRTC_INCLUDE_DIR})
        target_link_libraries(webrtc_module PRIVATE ${KVS_WEBRTC_ALL_LIBS})
        target_compile_definitions(webrtc_media_module PRIVATE HAVE_KVS_WEBRTC_SDK=1)
        target_include_directories(webrtc_media_module PRIVATE ${KVS_WEBRTC_INCLUDE_DIR})
        target_link_libraries(webrtc_media_module PRIVATE ${KVS_WEBRTC_ALL_LIBS})
        message(STATUS "WebRTC module: ENABLED (KVS WebRTC C SDK found)")
    else()
        message(WARNING "KVS WebRTC C SDK not found, using stub on Linux")
    endif()
else()
    message(STATUS "WebRTC module: using stub (non-Linux platform)")
endif()

# WebRTC test
add_executable(webrtc_test tests/webrtc_test.cpp)
target_link_libraries(webrtc_test PRIVATE
    webrtc_module
    GTest::gtest_main
    rapidcheck
    rapidcheck_gtest)
add_test(NAME webrtc_test COMMAND webrtc_test
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

# WebRTC media test
add_executable(webrtc_media_test tests/webrtc_media_test.cpp)
target_link_libraries(webrtc_media_test PRIVATE
    webrtc_media_module
    pipeline_manager
    GTest::gtest
    rapidcheck
    rapidcheck_gtest)
add_test(NAME webrtc_media_test COMMAND webrtc_media_test
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
