# FindOnnxRuntime.cmake
# Locate ONNX Runtime pre-built library.
#
# Sets:
#   OnnxRuntime_FOUND         - TRUE if found
#   OnnxRuntime_INCLUDE_DIRS  - Header directories
#   OnnxRuntime_LIBRARIES     - Library files
#
# Supports:
#   macOS: Homebrew (/opt/homebrew, /usr/local)
#   Linux aarch64: Custom install via CMAKE_PREFIX_PATH or ONNXRUNTIME_ROOT_DIR
#
# Usage:
#   list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
#   find_package(OnnxRuntime REQUIRED)

# Allow user to specify root directory
if(DEFINED ENV{ONNXRUNTIME_ROOT_DIR})
    set(ONNXRUNTIME_ROOT_DIR "$ENV{ONNXRUNTIME_ROOT_DIR}")
endif()

# Search hints
set(_ORT_SEARCH_PATHS
    ${ONNXRUNTIME_ROOT_DIR}
    /opt/homebrew
    /usr/local
    /usr
)

# Find header
find_path(OnnxRuntime_INCLUDE_DIR
    NAMES onnxruntime_c_api.h
    PATH_SUFFIXES
        include/onnxruntime      # Homebrew layout
        include/onnxruntime/core/session  # Some Linux layouts
        include                  # Direct layout
    HINTS ${_ORT_SEARCH_PATHS}
)

# Find library
find_library(OnnxRuntime_LIBRARY
    NAMES onnxruntime
    PATH_SUFFIXES lib lib64
    HINTS ${_ORT_SEARCH_PATHS}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OnnxRuntime
    REQUIRED_VARS OnnxRuntime_LIBRARY OnnxRuntime_INCLUDE_DIR
)

if(OnnxRuntime_FOUND)
    set(OnnxRuntime_INCLUDE_DIRS ${OnnxRuntime_INCLUDE_DIR})
    set(OnnxRuntime_LIBRARIES ${OnnxRuntime_LIBRARY})
    mark_as_advanced(OnnxRuntime_INCLUDE_DIR OnnxRuntime_LIBRARY)
endif()
