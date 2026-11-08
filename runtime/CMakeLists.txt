# The runtime, shared by every game build:
#   x2rt        standalone platform (SDL3 window/audio/input): desktop and the Android app
#   x2rt_retro  libretro platform: the RetroArch core (x2_add_libretro_core)
# Movies need FFmpeg (optional: without it they are skipped).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT CMAKE_CROSSCOMPILING) # host pkg-config would find the host's FFmpeg
  pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libswscale libswresample libavutil)
endif()
set(X2_HAVE_FFMPEG OFF)
if(FFMPEG_FOUND AND NOT CMAKE_CROSSCOMPILING)
  set(X2_HAVE_FFMPEG ON)
elseif(ANDROID)
  # static FFmpeg cross-built by tools/android/build_ffmpeg.sh (WMV/WMA only)
  set(X2_FFMPEG_ANDROID ${CMAKE_SOURCE_DIR}/third_party/ffmpeg-android CACHE PATH "Android FFmpeg prefix")
  if(EXISTS ${X2_FFMPEG_ANDROID}/lib/libavcodec.a)
    add_library(x2_ffmpeg INTERFACE)
    target_include_directories(x2_ffmpeg INTERFACE ${X2_FFMPEG_ANDROID}/include)
    foreach(l avformat avcodec swscale swresample avutil)
      target_link_libraries(x2_ffmpeg INTERFACE ${X2_FFMPEG_ANDROID}/lib/lib${l}.a)
    endforeach()
    target_link_libraries(x2_ffmpeg INTERFACE m)
    add_library(PkgConfig::FFMPEG ALIAS x2_ffmpeg)
    set(X2_HAVE_FFMPEG ON)
  else()
    message(STATUS "Android FFmpeg not found in ${X2_FFMPEG_ANDROID}: movies disabled "
                   "(build it with tools/android/build_ffmpeg.sh)")
  endif()
endif()
message(STATUS "x2: movies ${X2_HAVE_FFMPEG}")

# GDI text: CoreText on Apple by default, stb_truetype (system TrueType/OpenType fonts) elsewhere
if(APPLE)
  option(X2_FONT_CORETEXT "Use CoreText for GDI fonts (else stb_truetype)" ON)
else()
  set(X2_FONT_CORETEXT OFF)
endif()
# everything but the platform layer, compiled once for both runtimes
add_library(x2rt_common OBJECT
  src/boot.c
  src/png.c
  src/natives.c
  src/padmap.c
  src/mem.c
  src/loader.c
  src/dispatch.c
  src/thread.c
  src/hle/k32_sync.c
  src/hle/k32_file.c
  src/hle/k32_misc.c
  src/com.c
  src/jvs.c
  src/image.c
  $<IF:$<BOOL:${X2_FONT_CORETEXT}>,src/font_coretext.c,src/font_stb.c>
  $<IF:$<BOOL:${X2_HAVE_FFMPEG}>,src/video_ffmpeg.c,src/video_none.c>
  src/hle/user32.c
  src/hle/d3d9.c
  src/hle/dsound.c
  src/hle/d3dx9.c
  src/hle/d3dx_effect.c
  src/gfx/fx.c
  src/hle/advapi32.c
  src/txac.c
  src/hle/dshow.c
  src/hle/gdi32.c
  src/hle/net.c
  src/hle/pending.c
  src/gfx/gfx_gl.c
  src/gfx/gfx_gl_draw.c
  src/gfx/sm3_glsl.c
  src/gfx/d3dasm.c
  src/prof.c
  src/gfx/hlsl_glsl.c)
set_target_properties(x2rt_common PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(x2rt_common PUBLIC include)
target_compile_options(x2rt_common PRIVATE -Wall -Wextra -Wno-unused-parameter)
# X2_ASAN=ON: AddressSanitizer on the runtime only (the generated code stays uninstrumented)
option(X2_ASAN "AddressSanitizer for the runtime" OFF)
if(X2_ASAN)
  target_compile_options(x2rt_common PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(x2rt_common PUBLIC -fsanitize=address)
endif()

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
target_link_libraries(x2rt_common PUBLIC Threads::Threads m ZLIB::ZLIB)
if(X2_HAVE_FFMPEG)
  target_link_libraries(x2rt_common PUBLIC PkgConfig::FFMPEG)
endif()
if(APPLE)
  target_link_libraries(x2rt_common PUBLIC "-framework OpenGL")
  target_link_libraries(x2rt_common PUBLIC "-framework CoreGraphics" "-framework CoreFoundation")
  target_link_libraries(x2rt_common PUBLIC "-framework CoreText")
elseif(ANDROID)
  target_link_libraries(x2rt_common PUBLIC GLESv3 EGL log android)
else()
  find_package(OpenGL REQUIRED)
  target_link_libraries(x2rt_common PUBLIC OpenGL::GL)
endif()

# ---- standalone runtime (SDL3) ------------------------------------------------
option(X2_STANDALONE "Build the standalone executables / Android app (needs SDL3)" ON)
if(X2_STANDALONE)
  if(ANDROID)
    # SDL ships its Android Java glue in the source tree; build it from source.
    # Uses third_party/SDL when present, otherwise fetches the pinned release:
    #   git clone -b release-3.4.20 --depth 1 https://github.com/libsdl-org/SDL.git third_party/SDL
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(X2_SDL3_LOCAL ${CMAKE_SOURCE_DIR}/third_party/SDL)
    if(EXISTS ${X2_SDL3_LOCAL}/CMakeLists.txt)
      add_subdirectory(${X2_SDL3_LOCAL} ${CMAKE_BINARY_DIR}/sdl3 EXCLUDE_FROM_ALL)
      set(X2_SDL3_SOURCE_DIR ${X2_SDL3_LOCAL} CACHE INTERNAL "")
    else()
      include(FetchContent)
      FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.20
        GIT_SHALLOW TRUE)
      FetchContent_MakeAvailable(SDL3)
      set(X2_SDL3_SOURCE_DIR ${sdl3_SOURCE_DIR} CACHE INTERNAL "")
    endif()
  else()
    find_package(SDL3 REQUIRED CONFIG)
  endif()
  add_library(x2rt STATIC src/platform_sdl.c)
  target_compile_options(x2rt PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(x2rt PUBLIC x2rt_common SDL3::SDL3)
endif()

# ---- libretro runtime ---------------------------------------------------------
option(X2_LIBRETRO "Build the libretro core (typex2_libretro)" ON)
if(X2_LIBRETRO)
  add_library(x2rt_retro STATIC src/platform_libretro.c)
  set_target_properties(x2rt_retro PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_options(x2rt_retro PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_include_directories(x2rt_retro PRIVATE src ${CMAKE_BINARY_DIR}/libretro)
  target_compile_definitions(x2rt_retro PRIVATE "X2_VERSION=\"${PROJECT_VERSION}\"")
  target_link_libraries(x2rt_retro PUBLIC x2rt_common ${CMAKE_DL_LIBS})
endif()

# x2_add_game(<target> <generated dir>): builds a game from x2recomp output.
# Its generated code becomes an object library (x2gen_<game>) used by the
# standalone build and by the libretro core. table.c is compiled through a
# wrapper that renames its tables to x2g_<game>_* and adds the descriptor
# x2g_<game> (X2GameTables), so several games can live in one binary.
function(x2_add_game target gen_dir)
  set(X2_GEN_DIR ${gen_dir})
  include(${gen_dir}/sources.cmake)
  string(REGEX REPLACE "^x2-" "" game ${target})
  list(FILTER X2_GEN_SOURCES EXCLUDE REGEX "/table\\.c$")

  set(p x2g_${game})
  set(w "/* generated by runtime/CMakeLists.txt: ${game}'s tables as ${p} */\n")
  foreach(s func_table func_count import_table import_count image tls_callbacks)
    string(APPEND w "#define x2_${s} ${p}_${s}\n")
  endforeach()
  string(APPEND w "#include \"${gen_dir}/table.c\"\n")
  string(APPEND w "const X2GameTables ${p} = {x2_func_table, x2_func_count, x2_import_table, x2_import_count, &x2_image, x2_tls_callbacks};\n")
  file(WRITE ${CMAKE_BINARY_DIR}/games/${p}.c.tmp "${w}")
  configure_file(${CMAKE_BINARY_DIR}/games/${p}.c.tmp ${CMAKE_BINARY_DIR}/games/${p}.c COPYONLY)

  add_library(x2gen_${game} OBJECT ${X2_GEN_SOURCES} ${CMAKE_BINARY_DIR}/games/${p}.c)
  set_target_properties(x2gen_${game} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(x2gen_${game} PRIVATE ${gen_dir} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/include)
  # Generated code: huge functions, gotos everywhere, flags computed and dropped.
  set(opts "-w;-fno-strict-aliasing;-fwrapv;-fno-semantic-interposition;-O${X2_GEN_OPT};${X2_GEN_FLAGS}")
  if(X2_FN_TRACE)
    list(APPEND opts "-DX2_FN_TRACE")
  endif()
  if(X2_WATCH)
    list(APPEND opts "-DX2_WATCH")
  endif()
  target_compile_options(x2gen_${game} PRIVATE ${opts})

  set(label "${game}")
  if(game STREQUAL "kof12")
    set(label "The King of Fighters XII")
  elseif(game STREQUAL "kof13")
    set(label "The King of Fighters XIII")
  elseif(game STREQUAL "kof13cl")
    set(label "The King of Fighters XIII Climax")
  elseif(game STREQUAL "ssf4ae")
    set(label "Super Street Fighter IV Arcade Edition")
  elseif(game STREQUAL "bbcs")
    set(label "BlazBlue Continuum Shift")
  endif()
  set_property(GLOBAL APPEND PROPERTY X2_GAMES_BUILT ${game})
  set_property(GLOBAL PROPERTY X2_GAME_LABEL_${game} "${label}")

  if(NOT X2_STANDALONE)
    return()
  endif()
  set(main ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/main.c)
  if(ANDROID)
    # libx2_<game>.so: the game with the runtime, entered through x2_game_main()
    # by the launcher (libmain.so, see x2_add_android_app)
    add_library(${target} SHARED ${main} $<TARGET_OBJECTS:x2gen_${game}>)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME x2_${game})
    target_compile_definitions(${target} PRIVATE X2_GAME_LIB)
    # calls between generated functions bind locally (no PLT), names stay exported
    target_link_options(${target} PRIVATE -Wl,-Bsymbolic)
    # record name + image identity (from the generated x2_image) for the launcher
    file(STRINGS ${gen_dir}/table.c img REGEX "const X2ImageInfo x2_image")
    string(REGEX MATCH "\\{(0x[0-9a-f]+)u, (0x[0-9a-f]+)u, (0x[0-9a-f]+)u" _ "${img}")
    set_property(GLOBAL APPEND PROPERTY X2_ANDROID_GAMES ${target})
    set_property(GLOBAL APPEND PROPERTY X2_ANDROID_GAME_ROWS
      "{\"libx2_${game}.so\", \"${label}\", ${CMAKE_MATCH_1}u, ${CMAKE_MATCH_2}u, ${CMAKE_MATCH_3}u}")
  else()
    add_executable(${target} ${main} $<TARGET_OBJECTS:x2gen_${game}>)
  endif()
  target_compile_definitions(${target} PRIVATE X2_GAME_TABLES=${p})
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src)
  target_link_libraries(${target} PRIVATE x2rt)
  # export symbols so fault backtraces name the guest function (f_XXXXXXXX)
  set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
endfunction()

# x2_add_libretro_core(): after every x2_add_game, the RetroArch core holding all
# games: typex2_libretro.dylib / .so, typex2_libretro_android.so on Android.
# Each game's objects are partially linked into one object whose f_XXXXXXXX
# symbols are made local, so games with overlapping addresses can coexist.
function(x2_add_libretro_core)
  get_property(games GLOBAL PROPERTY X2_GAMES_BUILT)
  if(NOT games OR NOT X2_LIBRETRO)
    return()
  endif()
  set(h "/* generated by runtime/CMakeLists.txt */\n#include \"x2/recomp.h\"\n")
  set(rows "")
  set(objs "")
  foreach(g ${games})
    get_property(label GLOBAL PROPERTY X2_GAME_LABEL_${g})
    string(APPEND h "extern const X2GameTables x2g_${g};\n")
    string(APPEND rows "    {\"${label}\", &x2g_${g}},\n")
    set(o ${CMAKE_BINARY_DIR}/libretro/x2core_${g}.o)
    if(APPLE)
      set(cmd ${CMAKE_LINKER} -r -o ${o} $<TARGET_OBJECTS:x2gen_${g}> -unexported_symbol "_f_*")
    else()
      find_program(X2_OBJCOPY NAMES llvm-objcopy objcopy HINTS ${ANDROID_TOOLCHAIN_ROOT}/bin REQUIRED)
      set(cmd ${CMAKE_LINKER} -r -o ${o} $<TARGET_OBJECTS:x2gen_${g}>
        COMMAND ${X2_OBJCOPY} --wildcard --localize-symbol=f_* ${o})
    endif()
    add_custom_command(OUTPUT ${o}
      COMMAND ${cmd}
      DEPENDS x2gen_${g} $<TARGET_OBJECTS:x2gen_${g}>
      COMMENT "Partially linking ${g} for the libretro core"
      COMMAND_EXPAND_LISTS
      VERBATIM)
    set_source_files_properties(${o} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    list(APPEND objs ${o})
  endforeach()
  string(APPEND h "struct X2CoreGame { const char *name; const X2GameTables *tables; };\n")
  string(APPEND h "static const struct X2CoreGame x2_core_games[] = {\n${rows}};\n")
  file(WRITE ${CMAKE_BINARY_DIR}/libretro/x2_core_games.h.tmp "${h}")
  configure_file(${CMAKE_BINARY_DIR}/libretro/x2_core_games.h.tmp ${CMAKE_BINARY_DIR}/libretro/x2_core_games.h COPYONLY)

  add_library(typex2_libretro SHARED ${objs})
  set_target_properties(typex2_libretro PROPERTIES PREFIX "" LINKER_LANGUAGE C)
  if(ANDROID)
    set_target_properties(typex2_libretro PROPERTIES OUTPUT_NAME typex2_libretro_android)
  endif()
  # whole x2rt_retro: its retro_* entry points are only referenced by the frontend
  target_link_libraries(typex2_libretro PRIVATE $<LINK_LIBRARY:WHOLE_ARCHIVE,x2rt_retro>)
  if(APPLE)
    target_link_options(typex2_libretro PRIVATE "LINKER:-exported_symbol,_retro_*")
  else()
    set(vs ${CMAKE_BINARY_DIR}/libretro/exports.map)
    file(WRITE ${vs} "{ global: retro_*; local: *; };\n")
    target_link_options(typex2_libretro PRIVATE "LINKER:--version-script=${vs}" "LINKER:-Bsymbolic" "LINKER:--gc-sections")
    if(ANDROID AND CMAKE_STRIP) # debug info is ~90% of the file; RetroArch installs copy it
      add_custom_command(TARGET typex2_libretro POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:typex2_libretro> $<TARGET_FILE:typex2_libretro>.sym
                         COMMAND ${CMAKE_STRIP} --strip-unneeded $<TARGET_FILE:typex2_libretro> VERBATIM)
    endif()
  endif()
endfunction()

# x2_add_android_app(): after every x2_add_game, build the launcher (libmain.so,
# which SDL's activity loads) and the `apk` target: one "Type X2" app holding all
# games, picked at launch from game.exe (see src/launcher.c).
function(x2_add_android_app)
  get_property(games GLOBAL PROPERTY X2_ANDROID_GAMES)
  get_property(rows GLOBAL PROPERTY X2_ANDROID_GAME_ROWS)
  if(NOT games)
    return()
  endif()
  set(h "/* generated by runtime/CMakeLists.txt */\n#include <stdint.h>\n")
  string(APPEND h "struct X2Game { const char *lib, *name; uint32_t base, size, entry; };\n")
  string(APPEND h "static const struct X2Game x2_games[] = {\n")
  foreach(r ${rows})
    string(APPEND h "    ${r},\n")
  endforeach()
  string(APPEND h "};\n")
  file(WRITE ${CMAKE_BINARY_DIR}/launcher/x2_games.h.tmp "${h}")
  configure_file(${CMAKE_BINARY_DIR}/launcher/x2_games.h.tmp ${CMAKE_BINARY_DIR}/launcher/x2_games.h COPYONLY)
  add_library(x2launcher SHARED ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/launcher.c)
  set_target_properties(x2launcher PROPERTIES OUTPUT_NAME main)
  target_include_directories(x2launcher PRIVATE ${CMAKE_BINARY_DIR}/launcher)
  target_link_libraries(x2launcher PRIVATE SDL3::SDL3 log android dl)

  get_filename_component(sdk ${ANDROID_NDK}/../.. ABSOLUTE)
  file(GLOB bts LIST_DIRECTORIES true ${sdk}/build-tools/*)
  list(SORT bts COMPARE NATURAL)
  list(GET bts -1 bt)
  file(GLOB jars ${sdk}/platforms/android-*/android.jar)
  list(SORT jars COMPARE NATURAL)
  list(GET jars -1 jar)
  set(libs $<TARGET_FILE:x2launcher> $<TARGET_FILE:SDL3::SDL3-shared>)
  foreach(g ${games})
    list(APPEND libs $<TARGET_FILE:${g}>)
  endforeach()
  set(root ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..)
  add_custom_target(apk
    COMMAND ${CMAKE_COMMAND} -DSDK=${sdk} -DBUILD_TOOLS=${bt} -DPLATFORM_JAR=${jar} -DSDL_SRC=${X2_SDL3_SOURCE_DIR}
      "-DLIBS=$<JOIN:${libs},|>" -DAPP_ID=com.typex2.recomp "-DAPP_LABEL=Type X2"
      -DMANIFEST_IN=${root}/tools/android/AndroidManifest.xml.in -DOUT_DIR=${CMAKE_BINARY_DIR} -DSTRIP=${CMAKE_STRIP}
      -DAPK=${CMAKE_BINARY_DIR}/typex2.apk -P ${root}/tools/android/package.cmake
    DEPENDS x2launcher SDL3::SDL3-shared ${games}
    COMMENT "Packaging typex2.apk"
    VERBATIM)
endfunction()
