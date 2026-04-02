cmake_minimum_required(VERSION 3.20)
project(NDIReceiverPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

# ============================================================
# Paths - adjust these to your machine
# ============================================================

# DaVinci Resolve OFX SDK (ships with Resolve)
set(OFX_SDK_ROOT "C:/ProgramData/Blackmagic Design/DaVinci Resolve/Support/Developer/OpenFX")
set(OFX_INCLUDE_DIR "${OFX_SDK_ROOT}/OpenFX-1.4/include")
set(OFX_SUPPORT_INCLUDE "${OFX_SDK_ROOT}/Support/include")
set(OFX_SUPPORT_SRC "${OFX_SDK_ROOT}/Support/Library")

# NDI SDK headers (bundled with UE 5.7 NDIMedia plugin)
set(NDI_INCLUDE_DIR "C:/Program Files/Epic Games/UE_5.7/Engine/Plugins/Media/NDIMedia/Source/ThirdParty/NDI/Include")

# ============================================================
# OFX Support library sources
# ============================================================

file(GLOB OFX_SUPPORT_SOURCES "${OFX_SUPPORT_SRC}/*.cpp")
# Exclude BMD-specific HWND interact extension (not part of standard OFX, needs BMD-private headers)
list(FILTER OFX_SUPPORT_SOURCES EXCLUDE REGEX "ofxsHWNDInteract")

# ============================================================
# Plugin target
# ============================================================

add_library(NDIReceiverPlugin SHARED
    NDIReceiverPlugin.h
    NDIReceiverPlugin.cpp
    ${OFX_SUPPORT_SOURCES}
)

target_include_directories(NDIReceiverPlugin PRIVATE
    ${OFX_INCLUDE_DIR}
    ${OFX_SUPPORT_INCLUDE}
    ${NDI_INCLUDE_DIR}
)

# NDI uses dynamic loading at runtime via NDIlib_initialize().
# The runtime DLL ships with UE's NDI plugin - no separate install needed.
set(NDI_RUNTIME_DIR "C:/Program Files/Epic Games/UE_5.7/Engine/Plugins/Media/NDIMedia/Binaries/ThirdParty/Win64")

target_compile_definitions(NDIReceiverPlugin PRIVATE
    _WIN64
    WIN64
)

# ============================================================
# Post-build: create OFX bundle structure
# ============================================================

set(BUNDLE_DIR "${CMAKE_BINARY_DIR}/NDIReceiverPlugin.ofx.bundle/Contents/Win64")

add_custom_command(TARGET NDIReceiverPlugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BUNDLE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:NDIReceiverPlugin>" "${BUNDLE_DIR}/NDIReceiverPlugin.ofx"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${NDI_RUNTIME_DIR}/Processing.NDI.Lib.x64.dll"
        "${BUNDLE_DIR}/Processing.NDI.Lib.x64.dll"
    COMMENT "Creating OFX bundle and copying NDI runtime DLL"
)

# ============================================================
# Install: copy bundle to system OFX plugins directory
# ============================================================

set(OFX_PLUGINS_DIR "C:/Program Files/Common Files/OFX/Plugins")

install(DIRECTORY "${CMAKE_BINARY_DIR}/NDIReceiverPlugin.ofx.bundle"
    DESTINATION "${OFX_PLUGINS_DIR}"
)

message(STATUS "=== NDI Receiver OFX Plugin ===")
message(STATUS "OFX SDK: ${OFX_SDK_ROOT}")
message(STATUS "NDI Headers: ${NDI_INCLUDE_DIR}")
message(STATUS "Bundle output: ${BUNDLE_DIR}")
message(STATUS "Install to: ${OFX_PLUGINS_DIR}/NDIReceiverPlugin.ofx.bundle")
