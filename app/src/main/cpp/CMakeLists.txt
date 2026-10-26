cmake_minimum_required(VERSION 3.22)
project(cartucho CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The frontend that hosts the libretro cores. The cores themselves are
# separate libraries (libvbam_libretro.so, libazahar_libretro.so) built by CI
# and packaged next to this one; see .github/workflows/build.yml.
add_library(cartucho SHARED
    android_jni.cpp
    audio_output.cpp
    core_host.cpp
    gl_presenter.cpp
    resampler.cpp
    rewind.cpp
    runahead.cpp
    session.cpp
)

target_compile_options(cartucho PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)

target_link_libraries(cartucho
    GLESv3
    EGL
    aaudio
    android
    log
    dl
    z
)
