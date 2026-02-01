cmake_minimum_required(VERSION 3.10)
project(Pixel4aVCam)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Android Setup
if(NOT ANDROID)
    message(WARNING "This project is intended for Android NDK build.")
endif()

# Includes
# Includes
# Includes - FORCE PRIORITY using "BEFORE"
include_directories(BEFORE
    # We put our mock headers FIRST so the compiler finds them before checking the NDK
    ${CMAKE_CURRENT_SOURCE_DIR}/aosp_headers
    ${CMAKE_CURRENT_SOURCE_DIR}/aosp_headers/cutils
    ${CMAKE_CURRENT_SOURCE_DIR}/aosp_headers/system
    
    # Then the source code
    src
    
    # Then the other AOSP headers
    ${CMAKE_CURRENT_SOURCE_DIR}/aosp_headers/libhardware/include
    ${CMAKE_CURRENT_SOURCE_DIR}/aosp_headers/system/media/camera/include
    ${CMAKE_CURRENT_SOURCE_DIR}/aosp_headers/frameworks/av/include
)

# Shared Libraries (Receiver)
add_executable(cameraserver_proxy src/vcam_receiver.cpp)
target_link_libraries(cameraserver_proxy
    android
    log
)

# Wrapper Library (HAL)
add_library(camera.qcom.vcam SHARED src/vcam_wrapper.cpp)
target_link_libraries(camera.qcom.vcam
    android
    log
    dl
)

# We need the hardware/ headers.
# I will create dummy headers in 'common/hardware' if they don't exist, 
# but for a real project, we'd fetch them from AOSP.
# I'll create essential headers in next step for compilation success simulation.
