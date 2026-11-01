cmake_minimum_required(VERSION 3.20)
project(cartucho_desktop C CXX)

# Cartucho for PC (Windows; also builds on Linux for testing). The emulation
# engine is the Android app's (app/src/main/cpp); this adds the window,
# sound, controls and the console-style interface on top, with SDL2 and
# Dear ImGui. OpenGL ES 3.0 through glad, like on the phone.

set(CARTUCHO_VERSION "0.0.0-dev" CACHE STRING "Version shown in the app")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(ENGINE ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp)
set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SDL2 REQUIRED)

# zlib: the system one on Linux, built here for Windows.
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
    include(FetchContent)
    FetchContent_Declare(zlib URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zlib)
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
    target_include_directories(zlibstatic PUBLIC ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
endif()

# RetroAchievements client and CHD reader, shared with Android.
file(GLOB RCHEEVOS_SOURCES CONFIGURE_DEPENDS
    ${ENGINE}/third_party/rcheevos/src/api/*.c
    ${ENGINE}/third_party/rcheevos/src/client/*.c
    ${ENGINE}/third_party/rcheevos/src/hash/*.c
    ${ENGINE}/third_party/rcheevos/src/libretro/*.c
    ${ENGINE}/third_party/rcheevos/src/runtime/*.c
    ${ENGINE}/third_party/rcheevos/src/util/*.c)
add_library(rcheevos STATIC ${RCHEEVOS_SOURCES})
target_include_directories(rcheevos PUBLIC
    ${ENGINE}/third_party/rcheevos/include
    ${ENGINE}/third_party/rcheevos/src/libretro
    ${ENGINE})
target_compile_definitions(rcheevos PUBLIC RC_CLIENT_SUPPORTS_HASH RC_HASH_NO_ENCRYPTED)
target_compile_options(rcheevos PRIVATE -O2 -w)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_SYSTEM_ZLIB OFF CACHE BOOL "" FORCE)
add_subdirectory(${ENGINE}/third_party/libchdr libchdr EXCLUDE_FROM_ALL)

add_library(glad STATIC ${TP}/glad/src/gles2.c)
target_include_directories(glad PUBLIC ${TP}/glad/include ${TP}/gles_shim)

add_library(imgui STATIC
    ${TP}/imgui/imgui.cpp ${TP}/imgui/imgui_draw.cpp ${TP}/imgui/imgui_tables.cpp
    ${TP}/imgui/imgui_widgets.cpp
    ${TP}/imgui/backends/imgui_impl_sdl2.cpp ${TP}/imgui/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${TP}/imgui ${TP}/imgui/backends)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
target_link_libraries(imgui PUBLIC SDL2::SDL2 glad)

add_library(engine STATIC
    ${ENGINE}/achievements.cpp
    ${ENGINE}/chd_reader.cpp
    ${ENGINE}/core_host.cpp
    ${ENGINE}/gl_presenter.cpp
    ${ENGINE}/netplay.cpp
    ${ENGINE}/recorder.cpp
    ${ENGINE}/resampler.cpp
    ${ENGINE}/rewind.cpp
    ${ENGINE}/runahead.cpp
    ${ENGINE}/session.cpp)
target_include_directories(engine PUBLIC ${ENGINE})
target_link_libraries(engine PUBLIC glad rcheevos chdr-static ZLIB::ZLIB)
if(WIN32)
    target_link_libraries(engine PUBLIC ws2_32)
else()
    target_link_libraries(engine PUBLIC dl pthread)
endif()

file(GLOB APP_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
if(WIN32)
    list(APPEND APP_SOURCES win/cartucho.rc)
    add_executable(cartucho WIN32 ${APP_SOURCES})
    target_link_libraries(cartucho PRIVATE SDL2::SDL2main)
    target_link_options(cartucho PRIVATE -static-libgcc -static-libstdc++ -static)
    target_link_libraries(cartucho PRIVATE iphlpapi winhttp shell32 ole32 comdlg32)
else()
    add_executable(cartucho ${APP_SOURCES})
endif()
target_include_directories(cartucho PRIVATE src ${TP}/stb ${TP}/json)
target_link_libraries(cartucho PRIVATE engine imgui SDL2::SDL2)
target_compile_definitions(cartucho PRIVATE CARTUCHO_VERSION="${CARTUCHO_VERSION}")

# Next to the program: assets/ (fonts, sounds), assets/photos (the console
# photos the Android app uses), assets/meta (game facts, when built by CI).
add_custom_command(TARGET cartucho POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:cartucho>/assets
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:cartucho>/assets/photos
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/res/drawable-nodpi $<TARGET_FILE_DIR:cartucho>/assets/photos)
