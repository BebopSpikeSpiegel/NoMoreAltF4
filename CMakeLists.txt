cmake_minimum_required(VERSION 3.15)

project(NoMoreAltF4 CXX)

# Find latest version at https://github.com/OrfeasZ/ZHMModSDK/releases
# Set ZHMMODSDK_DIR variable to a local directory to use a local copy of the ZHMModSDK.
set(ZHMMODSDK_VER "v4.0.0-rc.3")
include(cmake/setup-zhmmodsdk.cmake)

# Set C++ standard to C++23.
set(CMAKE_CXX_STANDARD 23)

# Create the NoMoreAltF4 mod library.
add_library(NoMoreAltF4 SHARED
    src/NoMoreAltF4.cpp
    src/NoMoreAltF4.h
)

# Set UTF-8 flag.
set_target_properties(NoMoreAltF4 PROPERTIES COMPILE_FLAGS "/utf-8")

# Add SDK and DX headers dependencies.
find_package(directx-headers CONFIG REQUIRED)

target_link_libraries(NoMoreAltF4
    ZHMModSDK
    Microsoft::DirectX-Guids
    Microsoft::DirectX-Headers
    implot
)

install(TARGETS NoMoreAltF4
    RUNTIME DESTINATION bin
)

# Install the mod to the game folder when the `GAME_INSTALL_PATH` variable is set.
zhmmodsdk_install(NoMoreAltF4)
