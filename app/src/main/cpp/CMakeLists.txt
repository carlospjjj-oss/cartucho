cmake_minimum_required(VERSION 3.22)
project(cartucho C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The frontend that hosts the libretro cores. The cores themselves are
# separate libraries (libvbam_libretro.so, libazahar_libretro.so) built by CI
# and packaged next to this one; see .github/workflows/build.yml.
# RetroAchievements client (third_party/rcheevos, MIT).
file(GLOB RCHEEVOS_SOURCES CONFIGURE_DEPENDS
    third_party/rcheevos/src/api/*.c
    third_party/rcheevos/src/client/*.c
    third_party/rcheevos/src/hash/*.c
    third_party/rcheevos/src/libretro/*.c
    third_party/rcheevos/src/runtime/*.c
    third_party/rcheevos/src/util/*.c)
add_library(rcheevos STATIC ${RCHEEVOS_SOURCES})
target_include_directories(rcheevos PUBLIC
    third_party/rcheevos/include
    third_party/rcheevos/src/libretro
    ${CMAKE_CURRENT_SOURCE_DIR})   # libretro.h
target_compile_definitions(rcheevos PUBLIC RC_CLIENT_SUPPORTS_HASH RC_HASH_NO_ENCRYPTED)
target_compile_options(rcheevos PRIVATE -O2 -w)

# CHD disc images, for identifying PlayStation games (third_party/libchdr, BSD).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libchdr EXCLUDE_FROM_ALL)

add_library(cartucho SHARED
    achievements.cpp
    chd_reader.cpp
    android_jni.cpp
    audio_output.cpp
    core_host.cpp
    gl_presenter.cpp
    recorder.cpp
    resampler.cpp
    rewind.cpp
    runahead.cpp
    session.cpp
    netplay.cpp
)

target_compile_options(cartucho PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)

target_include_directories(cartucho PRIVATE third_party/libchdr/include)

target_link_libraries(cartucho
    rcheevos
    chdr-static
    GLESv3
    EGL
    aaudio
    android
    log
    dl
    z
)
