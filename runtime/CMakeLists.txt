# x2rt: the runtime library shared by every game build.
# Movies need FFmpeg (optional: without it they are skipped).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libswscale libswresample libavutil)
endif()
set(X2_HAVE_FFMPEG ${FFMPEG_FOUND})
add_library(x2rt STATIC
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
  $<IF:$<PLATFORM_ID:Darwin>,src/font_coretext.c,src/font_stub.c>
  $<IF:$<BOOL:${X2_HAVE_FFMPEG}>,src/video_ffmpeg.c,src/video_none.c>
  src/hle/user32.c
  src/hle/d3d9.c
  src/hle/dsound.c
  src/hle/d3dx9.c
  src/hle/advapi32.c
  src/hle/dshow.c
  src/hle/gdi32.c
  src/hle/pending.c
  src/gfx/gfx_gl.c
  src/gfx/gfx_gl_draw.c
  src/gfx/sm3_glsl.c
  src/gfx/hlsl_glsl.c
  src/platform_sdl.c)
target_include_directories(x2rt PUBLIC include)
target_compile_options(x2rt PRIVATE -Wall -Wextra -Wno-unused-parameter)
find_package(Threads REQUIRED)
find_package(SDL3 REQUIRED CONFIG)
find_package(ZLIB REQUIRED)
target_link_libraries(x2rt PUBLIC Threads::Threads m SDL3::SDL3 ZLIB::ZLIB)
if(X2_HAVE_FFMPEG)
  target_link_libraries(x2rt PUBLIC PkgConfig::FFMPEG)
endif()
if(APPLE)
  target_link_libraries(x2rt PUBLIC "-framework OpenGL")
  target_link_libraries(x2rt PUBLIC "-framework CoreGraphics" "-framework CoreFoundation")
  target_link_libraries(x2rt PUBLIC "-framework CoreText")
  # guest call depth maps onto host stacks; the main thread only runs the platform loop
elseif(ANDROID)
  target_link_libraries(x2rt PUBLIC GLESv3)
else()
  find_package(OpenGL REQUIRED)
  target_link_libraries(x2rt PUBLIC OpenGL::GL)
endif()

# x2_add_game(<target> <generated dir>): builds a game from x2recomp output.
function(x2_add_game target gen_dir)
  set(X2_GEN_DIR ${gen_dir})
  include(${gen_dir}/sources.cmake)
  add_executable(${target} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/main.c ${X2_GEN_SOURCES})
  target_include_directories(${target} PRIVATE ${gen_dir} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src)
  target_link_libraries(${target} PRIVATE x2rt)
  # export symbols so fault backtraces name the guest function (f_XXXXXXXX)
  set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
  # Generated code: huge functions, gotos everywhere, flags computed and dropped.
  set(opts "-w;-fno-strict-aliasing;-fwrapv;-O${X2_GEN_OPT}")
  if(X2_FN_TRACE)
    list(APPEND opts "-DX2_FN_TRACE")
  endif()
  if(X2_WATCH)
    list(APPEND opts "-DX2_WATCH")
  endif()
  set_source_files_properties(${X2_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "${opts}")
endfunction()
