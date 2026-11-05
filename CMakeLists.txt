cmake_minimum_required(VERSION 3.24)
project(typex2_recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Host tools (recompiler, archive tool). Skipped when cross-compiling the
# runtime for Android/iOS; build them with a host configuration instead.
option(X2_BUILD_TOOLS "Build host tools (x2recomp, arc)" ${PROJECT_IS_TOP_LEVEL})

if(X2_BUILD_TOOLS AND NOT CMAKE_CROSSCOMPILING)
  # capstone: prefer a system install (brew install capstone), otherwise
  # fetch a pinned release with -DX2_FETCH_CAPSTONE=ON.
  option(X2_FETCH_CAPSTONE "Download capstone 5.0.1 instead of using a system install" OFF)
  if(X2_FETCH_CAPSTONE)
    include(FetchContent)
    FetchContent_Declare(capstone
      GIT_REPOSITORY https://github.com/capstone-engine/capstone.git
      GIT_TAG 5.0.1
      GIT_SHALLOW TRUE)
    set(CAPSTONE_ARCHITECTURE_DEFAULT OFF CACHE BOOL "" FORCE)
    set(CAPSTONE_X86_SUPPORT ON CACHE BOOL "" FORCE)
    set(CAPSTONE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CAPSTONE_BUILD_CSTOOL OFF CACHE BOOL "" FORCE)
    set(CAPSTONE_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(capstone)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CAPSTONE REQUIRED IMPORTED_TARGET capstone>=5.0)
    # capstone.pc points at include/capstone; we include <capstone/capstone.h>
    foreach(dir ${CAPSTONE_INCLUDE_DIRS})
      set_property(TARGET PkgConfig::CAPSTONE APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${dir}/..")
    endforeach()
    add_library(capstone::capstone ALIAS PkgConfig::CAPSTONE)
  endif()

  add_subdirectory(recompiler)
  add_subdirectory(tools)
endif()

# Runtime and games. A game is built only after its code has been generated:
#   build/recompiler/x2recomp <path/to/game.exe> games/<game>/gen
set(X2_GEN_OPT 1 CACHE STRING "Optimization level for generated code (0-3)")
add_subdirectory(runtime)
foreach(game kof12 kof13)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/games/${game}/gen/sources.cmake)
    x2_add_game(x2-${game} ${CMAKE_CURRENT_SOURCE_DIR}/games/${game}/gen)
  endif()
endforeach()
