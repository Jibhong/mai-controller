project(mai2io_dll LANGUAGES C)
cmake_minimum_required(VERSION 3.5)

IF(NOT CMAKE_BUILD_TYPE)
    SET(CMAKE_BUILD_TYPE Release)
ENDIF()

IF (NOT WIN32)
    MESSAGE(FATAL_ERROR "mai2io.dll must be built for Windows (it's loaded in-process by the game).")
ENDIF()

# Maimai DX (and its segatools loaders) are 64-bit processes. A DLL built
# for the wrong bitness fails to load with HRESULT 0x800700c1
# (ERROR_BAD_EXE_FORMAT). Force 64-bit here.
IF (MSVC)
    IF (NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64" AND NOT CMAKE_VS_PLATFORM_NAME STREQUAL "x64")
        MESSAGE(WARNING "Building with MSVC: make sure you selected a x64 generator/platform, e.g. cmake -B build -A x64")
    ENDIF()
ELSE()
    # MinGW
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m64")
    SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -m64")
    SET(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -m64")
ENDIF()

ADD_LIBRARY(mai2io SHARED
    mai2io.c
    config.c
    gui.c)

TARGET_LINK_LIBRARIES(mai2io user32 gdi32)

SET_TARGET_PROPERTIES(mai2io PROPERTIES
    OUTPUT_NAME "mai2io"
    PREFIX "")
