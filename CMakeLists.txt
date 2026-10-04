cmake_minimum_required(VERSION 3.16)
project(hotline VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Usage: cmake -B build -DHOTLINE_SANITIZE=thread   (or address / undefined)
set(HOTLINE_SANITIZE "" CACHE STRING "Sanitizer to enable: thread | address | undefined")

add_library(hotline_flags INTERFACE)
if(MSVC)
  target_compile_options(hotline_flags INTERFACE /W4)
else()
  target_compile_options(hotline_flags INTERFACE -Wall -Wextra -Wpedantic)
  if(HOTLINE_SANITIZE)
    target_compile_options(hotline_flags INTERFACE -fsanitize=${HOTLINE_SANITIZE} -fno-omit-frame-pointer -g)
    target_link_options(hotline_flags INTERFACE -fsanitize=${HOTLINE_SANITIZE})
  endif()
endif()

find_package(Threads REQUIRED)

add_library(hotline_net STATIC src/udp_socket.cpp)
target_include_directories(hotline_net PUBLIC include)
target_link_libraries(hotline_net PUBLIC hotline_flags Threads::Threads)
if(WIN32)
  target_link_libraries(hotline_net PUBLIC ws2_32)
endif()

# Tools, tests, benchmark: no external dependencies.
add_executable(impair_proxy tools/impair_proxy.cpp)
target_link_libraries(impair_proxy PRIVATE hotline_net)

enable_testing()
add_executable(test_spsc tests/test_spsc.cpp)
target_link_libraries(test_spsc PRIVATE hotline_net)
add_test(NAME spsc COMMAND test_spsc)

add_executable(test_jitter tests/test_jitter.cpp)
target_link_libraries(test_jitter PRIVATE hotline_net)
add_test(NAME jitter COMMAND test_jitter)

add_executable(bench_ring bench/bench_ring.cpp)
target_link_libraries(bench_ring PRIVATE hotline_net)

# The call app needs PortAudio and Opus.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PORTAUDIO IMPORTED_TARGET portaudio-2.0)
  pkg_check_modules(OPUS IMPORTED_TARGET opus)
endif()

if(TARGET PkgConfig::PORTAUDIO AND TARGET PkgConfig::OPUS)
  add_executable(hotline src/main.cpp src/audio_io.cpp)
  target_link_libraries(hotline PRIVATE hotline_net PkgConfig::PORTAUDIO PkgConfig::OPUS)
else()
  message(WARNING "PortAudio and/or Opus not found: building tests and tools only. "
                  "Install libportaudio-dev + libopus-dev (apt) or portaudio + opus (brew).")
endif()
