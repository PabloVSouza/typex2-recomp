cmake_minimum_required(VERSION 3.24)
project(typex2_recomp VERSION 0.1.0 LANGUAGES C CXX)

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
if(ANDROID)
  # measured on a Cortex-A76 (Helio G99): -O2 is ~14% faster than -O1 on heavy frames
  set(X2_GEN_OPT 2 CACHE STRING "Optimization level for generated code (0-3)")
else()
  set(X2_GEN_OPT 1 CACHE STRING "Optimization level for generated code (0-3)")
endif()
# Guest memory ordering on ARM (runtime/include/x2/recomp.h): 1 = acquire loads +
# release stores (full x86 TSO), 2 = release stores only, 0 = none. Android
# defaults to 2: acquire loads cost ~25% of a heavy frame on Cortex-A76 cores.
if(ANDROID)
  set(X2_TSO 2 CACHE STRING "Guest memory ordering on ARM: 0 none, 1 full TSO, 2 release stores")
else()
  set(X2_TSO "" CACHE STRING "Guest memory ordering on ARM: 0 none, 1 full TSO, 2 release stores (empty = 1 on ARM)")
endif()
if(NOT X2_TSO STREQUAL "")
  add_compile_definitions(X2_TSO=${X2_TSO})
endif()
set(X2_GEN_FLAGS "" CACHE STRING "Extra compile flags for generated code (e.g. -DX2_TSO=0)")
add_subdirectory(runtime)
# every games/<name>/gen produced by x2recomp becomes an x2-<name> executable
set(X2_GAMES "" CACHE STRING "Games to build (e.g. kof12;kof13); empty = every games/*/gen")
file(GLOB X2_GAME_GENS LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/games/*/gen)
foreach(gen ${X2_GAME_GENS})
  if(EXISTS ${gen}/sources.cmake)
    get_filename_component(game_dir ${gen} DIRECTORY)
    get_filename_component(game ${game_dir} NAME)
    if(X2_GAMES AND NOT game IN_LIST X2_GAMES)
      continue()
    endif()
    x2_add_game(x2-${game} ${gen})
  endif()
endforeach()
if(ANDROID)
  x2_add_android_app() # launcher + `apk` target (all games in one app)
endif()
x2_add_libretro_core() # typex2_libretro: every game in one RetroArch core
