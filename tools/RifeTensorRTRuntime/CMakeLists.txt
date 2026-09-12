cmake_minimum_required(VERSION 3.25)

project(MPCVRRifeRuntime LANGUAGES CXX CUDA)

if(NOT WIN32)
    message(FATAL_ERROR "MPCVR RIFE runtime is Windows-only")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "MPCVR RIFE runtime is x64-only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(TENSORRT_ROOT "$ENV{TENSORRT_ROOT}" CACHE PATH "TensorRT installation root")
if(NOT TENSORRT_ROOT)
    message(FATAL_ERROR "Set TENSORRT_ROOT to a TensorRT 10.14+ or 11.x installation")
endif()

find_package(CUDAToolkit REQUIRED)

set(_TENSORRT_INCLUDE_HINTS
    "${TENSORRT_ROOT}/include"
    "${TENSORRT_ROOT}"
)
set(_TENSORRT_LIBRARY_HINTS
    "${TENSORRT_ROOT}/lib"
    "${TENSORRT_ROOT}/lib/x64"
    "${TENSORRT_ROOT}"
)

find_path(TENSORRT_INCLUDE_DIR
    NAMES NvInfer.h
    HINTS ${_TENSORRT_INCLUDE_HINTS}
    PATH_SUFFIXES include)
find_library(TENSORRT_NVINFER
    NAMES nvinfer nvinfer_11 nvinfer_10
    HINTS ${_TENSORRT_LIBRARY_HINTS}
    PATH_SUFFIXES lib lib/x64)
find_library(TENSORRT_ONNXPARSER
    NAMES nvonnxparser nvonnxparser_11 nvonnxparser_10
    HINTS ${_TENSORRT_LIBRARY_HINTS}
    PATH_SUFFIXES lib lib/x64)

message(STATUS "TensorRT root: ${TENSORRT_ROOT}")
message(STATUS "TensorRT include: ${TENSORRT_INCLUDE_DIR}")
message(STATUS "TensorRT nvinfer: ${TENSORRT_NVINFER}")
message(STATUS "TensorRT nvonnxparser: ${TENSORRT_ONNXPARSER}")

if(NOT TENSORRT_INCLUDE_DIR OR NOT TENSORRT_NVINFER OR NOT TENSORRT_ONNXPARSER)
    file(GLOB _TENSORRT_LIB_DIAGNOSTIC
        "${TENSORRT_ROOT}/lib/*.lib"
        "${TENSORRT_ROOT}/lib/x64/*.lib")
    message(STATUS "TensorRT import libraries found: ${_TENSORRT_LIB_DIAGNOSTIC}")
    message(FATAL_ERROR "TensorRT headers/libraries were not found below TENSORRT_ROOT=${TENSORRT_ROOT}")
endif()

add_library(MPCVRRifeRuntime64 SHARED
    RifeTensorRTRuntime.cpp
    RifeKernels.cu
)

target_include_directories(MPCVRRifeRuntime64 PRIVATE
    "${TENSORRT_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Source"
)

target_compile_definitions(MPCVRRifeRuntime64 PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(MPCVRRifeRuntime64 PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive->
    $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/W4>
)

set_target_properties(MPCVRRifeRuntime64 PROPERTIES
    OUTPUT_NAME "MPCVRRifeRuntime64"
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_ARCHITECTURES "75;80;86;89"
)

target_link_libraries(MPCVRRifeRuntime64 PRIVATE
    CUDA::cudart
    "${TENSORRT_NVINFER}"
    "${TENSORRT_ONNXPARSER}"
    d3d11
    dxgi
    bcrypt
)

message(STATUS "CUDA toolkit: ${CUDAToolkit_VERSION}")
