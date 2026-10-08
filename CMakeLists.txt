cmake_minimum_required(VERSION 3.20)
project(itch_lob_sim VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# -march=native gives the best numbers locally, but binaries are not portable.
# CI turns it OFF.
option(ITCH_NATIVE "Tune for the host CPU (-march=native)" ON)

if(MSVC)
  add_compile_options(/W4)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(ITCH_NATIVE)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-march=native HAS_MARCH_NATIVE)
    if(HAS_MARCH_NATIVE)
      add_compile_options(-march=native)
    endif()
  endif()
endif()

# Core library: order book + latency recorder (parser/pipeline are header-only).
add_library(itchsim STATIC src/order_book.cpp src/latency.cpp)
target_include_directories(itchsim PUBLIC include)

add_executable(itch_sim src/main.cpp)
target_link_libraries(itch_sim PRIVATE itchsim)

add_executable(gen_synthetic tools/gen_synthetic.cpp)

enable_testing()
add_executable(itch_tests tests/test_main.cpp)
target_link_libraries(itch_tests PRIVATE itchsim)
add_test(NAME itch_tests COMMAND itch_tests)
