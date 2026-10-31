cmake_minimum_required(VERSION 3.15)

project(NoMoreAltF4 CXX)

# Find latest version at https://github.com/OrfeasZ/ZHMModSDK/releases
# Set ZHMMODSDK_DIR variable to a local directory to use a local copy of the ZHMModSDK.
set(ZHMMODSDK_VER "v4.2.1")
include(cmake/setup-zhmmodsdk.cmake)

# Set C++ standard to C++23.
set(CMAKE_CXX_STANDARD 23)

# Create the NoMoreAltF4 mod library.
add_library(NoMoreAltF4 SHARED
    src/NoMoreAltF4.cpp
    src/NoMoreAltF4.h
    src/LogMirror.cpp
    src/LogMirror.h
)

# Set UTF-8 flag.
set_target_properties(NoMoreAltF4 PROPERTIES COMPILE_FLAGS "/utf-8")

# Add SDK and DX headers dependencies.
find_package(directx-headers CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

target_link_libraries(NoMoreAltF4
    ZHMModSDK
    Microsoft::DirectX-Guids
    Microsoft::DirectX-Headers
    # Required even though we never call ImPlot: the SDK's inline
    # IPluginInterface::SetupUI/CleanupUI call ImPlot::SetCurrentContext.
    implot
    ZLIB::ZLIB
)

install(TARGETS NoMoreAltF4
    RUNTIME DESTINATION bin
)

# Dev convenience: Visual Studio runs this install step right before it launches
# the game (launch.vs.json target "NoMoreAltF4.dll (Install)"), and the SDK wipes
# ZHMModLoader.log on every launch. So archive the previous session's raw log
# into <project>/logs/ first. Runs only for the *-Install presets.
if (DEFINED GAME_INSTALL_PATH AND EXISTS "${GAME_INSTALL_PATH}/Retail")
    file(TO_CMAKE_PATH "${GAME_INSTALL_PATH}" NMA_GAME_DIR)
    install(CODE "set(NMA_GAME_DIR \"${NMA_GAME_DIR}\")
set(NMA_ARCHIVE_DIR \"${CMAKE_SOURCE_DIR}/logs\")")
    install(SCRIPT cmake/archive-game-log.cmake)
endif()

# Install the mod to the game folder when the `GAME_INSTALL_PATH` variable is set.
zhmmodsdk_install(NoMoreAltF4)
