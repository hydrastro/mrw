# morse-deluxe - CMake build
#
# Targets:
#   morse              static core library (C11, depends only on ds)
#   morsec             command-line tool       (option MORSE_BUILD_CLI)
#   morse-deluxe-gui   Dear ImGui desktop app  (option MORSE_BUILD_GUI)
#   ctest              unit tests              (option MORSE_BUILD_TESTS)
#
# The core library and CLI have no GUI dependencies and always build. The GUI is
# gated behind MORSE_BUILD_GUI so a headless machine can still build everything
# else. Dependencies (ds, Dear ImGui, GLFW, miniaudio) are either found on the
# system / supplied via -D*_DIR (what the Nix flake does) or fetched from GitHub.
cmake_minimum_required(VERSION 3.16)
project(morse_deluxe VERSION 1.0.0 LANGUAGES C CXX)

option(MORSE_BUILD_CLI "Build the morsec command-line tool" ON)
option(MORSE_BUILD_GUI "Build the Dear ImGui desktop GUI" ON)
option(MORSE_BUILD_TESTS "Build and register the unit tests" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)

# --------------------------------------------------------------------------
# ds - hydrastro/ds intrusive data-structure library (no CMake of its own).
#
# ds ships some Unicode-table sources that are produced by a code-generator
# step in its Makefile. libmorse does not use any of that machinery, so rather
# than reproduce ds's codegen we compile exactly the closure of modules that
# libmorse actually references. This list is the transitive set required by the
# library's use of ds_list / ds_hash_table / ds_trie / ds_circular_buffer /
# ds_queue / ds_str; it is self-contained and needs no generated headers.
# (To build the *entire* ds library instead, use ds's own Makefile or its Nix
# flake, which run the generator.)
# --------------------------------------------------------------------------
set(DS_ROOT "" CACHE PATH "Path to a checkout of hydrastro/ds (else fetched)")
if(DS_ROOT STREQUAL "")
  FetchContent_Declare(ds
    GIT_REPOSITORY https://github.com/hydrastro/ds.git
    GIT_TAG master)
  FetchContent_GetProperties(ds)
  if(NOT ds_POPULATED)
    FetchContent_Populate(ds)
  endif()
  set(DS_ROOT "${ds_SOURCE_DIR}")
endif()
message(STATUS "morse-deluxe: using ds from ${DS_ROOT}")

set(DS_MODULES
    circular_buffer context diagnostic error hash_table
    list queue status str trie)
set(DS_SOURCES "")
foreach(m ${DS_MODULES})
  if(NOT EXISTS "${DS_ROOT}/lib/${m}.c")
    message(FATAL_ERROR "Expected ds source ${DS_ROOT}/lib/${m}.c not found. "
                        "Set -DDS_ROOT=/path/to/ds.")
  endif()
  list(APPEND DS_SOURCES "${DS_ROOT}/lib/${m}.c")
endforeach()
add_library(ds STATIC ${DS_SOURCES})
target_include_directories(ds PUBLIC "${DS_ROOT}")
set_target_properties(ds PROPERTIES POSITION_INDEPENDENT_CODE ON)
# ds is third-party C89-era code: build it quietly.
if(NOT MSVC)
  target_compile_options(ds PRIVATE -w)
endif()

# --------------------------------------------------------------------------
# libmorse - the core (pure C, depends only on ds + libm)
# --------------------------------------------------------------------------
file(GLOB MORSE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
add_library(morse STATIC ${MORSE_SOURCES})
target_include_directories(morse
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src") # private morse_alloc.h
target_link_libraries(morse PUBLIC ds)
if(NOT MSVC)
  target_compile_options(morse PRIVATE -Wall -Wextra -Wpedantic)
endif()
include(CheckLibraryExists)
find_library(MORSE_LIBM m)
if(MORSE_LIBM)
  target_link_libraries(morse PUBLIC ${MORSE_LIBM})
endif()

# --------------------------------------------------------------------------
# morsec - command-line front-end
# --------------------------------------------------------------------------
if(MORSE_BUILD_CLI)
  add_executable(morsec app/cli/main.c)
  target_link_libraries(morsec PRIVATE morse)
  if(NOT MSVC)
    target_compile_options(morsec PRIVATE -Wall -Wextra)
  endif()
endif()

# --------------------------------------------------------------------------
# tests (ctest)
# --------------------------------------------------------------------------
if(MORSE_BUILD_TESTS)
  enable_testing()
  set(MORSE_TESTS
      test_timing test_table test_encode_decode test_synth_detect
      test_roundtrip test_fft_tone test_cw test_multi)
  foreach(t ${MORSE_TESTS})
    add_executable(${t} "tests/${t}.c")
    target_link_libraries(${t} PRIVATE morse)
    target_include_directories(${t} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
endif()

# --------------------------------------------------------------------------
# GUI (gated) - Dear ImGui + GLFW + OpenGL + miniaudio (+ optional ffmpeg)
# --------------------------------------------------------------------------
if(MORSE_BUILD_GUI)
  set(OpenGL_GL_PREFERENCE GLVND)
  find_package(OpenGL REQUIRED)

  # GLFW: prefer system, else fetch and build.
  find_package(glfw3 3.3 QUIET)
  if(NOT glfw3_FOUND)
    message(STATUS "morse-deluxe: fetching GLFW")
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
      GIT_REPOSITORY https://github.com/glfw/glfw.git
      GIT_TAG 3.4)
    FetchContent_MakeAvailable(glfw)
  endif()

  # Dear ImGui: supplied via -DIMGUI_DIR (Nix) or fetched.
  set(IMGUI_DIR "" CACHE PATH "Dear ImGui source directory")
  if(IMGUI_DIR STREQUAL "")
    FetchContent_Declare(imgui
      GIT_REPOSITORY https://github.com/ocornut/imgui.git
      GIT_TAG v1.91.5)
    FetchContent_GetProperties(imgui)
    if(NOT imgui_POPULATED)
      FetchContent_Populate(imgui)
    endif()
    set(IMGUI_DIR "${imgui_SOURCE_DIR}")
  endif()
  message(STATUS "morse-deluxe: using Dear ImGui from ${IMGUI_DIR}")

  # miniaudio: supplied via -DMINIAUDIO_DIR (Nix) or fetched.
  set(MINIAUDIO_DIR "" CACHE PATH "Directory containing miniaudio.h")
  if(MINIAUDIO_DIR STREQUAL "")
    FetchContent_Declare(miniaudio
      GIT_REPOSITORY https://github.com/mackron/miniaudio.git
      GIT_TAG master)
    FetchContent_GetProperties(miniaudio)
    if(NOT miniaudio_POPULATED)
      FetchContent_Populate(miniaudio)
    endif()
    set(MINIAUDIO_DIR "${miniaudio_SOURCE_DIR}")
  endif()
  message(STATUS "morse-deluxe: using miniaudio from ${MINIAUDIO_DIR}")

  add_library(imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp")
  target_include_directories(imgui PUBLIC
    "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
  target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

  add_executable(morse-deluxe-gui
    app/gui/main.cpp
    app/gui/app.cpp
    app/gui/audio.cpp
    app/gui/media.cpp
    app/gui/cwio_cpp.cpp
    app/gui/miniaudio_impl.c)
  target_include_directories(morse-deluxe-gui PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/app/gui" "${MINIAUDIO_DIR}")
  target_link_libraries(morse-deluxe-gui PRIVATE morse imgui glfw OpenGL::GL)

  if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(morse-deluxe-gui PRIVATE
      Threads::Threads ${CMAKE_DL_LIBS} m)
  elseif(APPLE)
    target_link_libraries(morse-deluxe-gui PRIVATE
      "-framework CoreFoundation" "-framework CoreAudio"
      "-framework AudioToolbox" "-framework AudioUnit")
  endif()
endif()

# --------------------------------------------------------------------------
# install
# --------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS morse ds ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/morse"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(MORSE_BUILD_CLI)
  install(TARGETS morsec RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(MORSE_BUILD_GUI)
  install(TARGETS morse-deluxe-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

message(STATUS "morse-deluxe configured:"
  " CLI=${MORSE_BUILD_CLI} GUI=${MORSE_BUILD_GUI} TESTS=${MORSE_BUILD_TESTS}")
