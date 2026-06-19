# SPDX-License-Identifier: AGPL-3.0-only
# Part of Alkahest. See LICENSE for terms.
cmake_minimum_required(VERSION 3.16)

project(alkahest
  VERSION 0.1.0
  DESCRIPTION "The universal solvent for text — a recipe-driven CLI text multitool"
  LANGUAGES CXX)

# ---- standard / flags ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
  add_compile_options(/W4 /utf-8 /EHsc)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---- core library (everything except main/cli for easy unit testing) ----
add_library(alkahest_core STATIC
  src/util.cpp
  src/hash.cpp
  src/json.cpp
  src/expr.cpp
  src/registry.cpp
  src/ops_encoding.cpp
  src/ops_text.cpp
  src/ops_data.cpp
  src/ops_analysis.cpp
  src/ops_all.cpp
  src/server.cpp
  src/tui.cpp
)
target_include_directories(alkahest_core
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ---- the `alk` executable ----
add_executable(alk
  src/main.cpp
  src/cli.cpp
)
target_link_libraries(alk PRIVATE alkahest_core)
target_include_directories(alk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---- tests ----
enable_testing()
add_executable(alk_tests tests/test_main.cpp)
target_link_libraries(alk_tests PRIVATE alkahest_core)
add_test(NAME alk_tests COMMAND alk_tests)

# ---- install ----
include(GNUInstallDirs)
install(TARGETS alk RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
