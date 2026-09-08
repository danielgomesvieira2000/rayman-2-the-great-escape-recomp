cmake_minimum_required(VERSION 3.20)
project(rayman2-recomp LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# rayman-2-the-great-escape-recomp
#
# Builds the runtime host and links it against the recompiled game code.
#
# The recompiled C in RecompiledFuncs/ is NOT in this repository: it is
# generated on the builder's machine from the builder's own ROM by
# scripts/recompile.sh. Configuring without it is a hard error with an
# explanation rather than a wall of missing-file messages.
# ---------------------------------------------------------------------------

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(RAYMAN2_ENABLE_FRONTEND "Build with the RecompFrontend launcher, settings and input UI" ON)

# ---------------------------------------------------------------------------
# Recompiled game code
# ---------------------------------------------------------------------------
file(GLOB RECOMPILED_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/RecompiledFuncs/*.c")
if(NOT RECOMPILED_SOURCES)
    message(FATAL_ERROR
        "RecompiledFuncs/ contains no generated C.\n"
        "The recompiled game code is produced from YOUR ROM and is never committed. Run:\n"
        "  scripts/setup-splat.sh && scripts/split-rom.sh && scripts/build-elf.sh\n"
        "  scripts/build-recompiler.sh && scripts/recompile.sh\n"
        "See BUILDING.md.")
endif()

add_library(rayman2_recompiled STATIC ${RECOMPILED_SOURCES})
target_include_directories(rayman2_recompiled PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/RecompiledFuncs"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/N64Recomp/include")

# port_runtime.h declares the handful of libultra routines N64Recomp drops and
# leaves to the port. The generated sources include only recomp.h and funcs.h
# and must never be hand-edited, so the declaration is forced in.
if(MSVC)
    target_compile_options(rayman2_recompiled PRIVATE
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/include/port_runtime.h"
        /wd4102 /wd4013 /wd4116)
else()
    target_compile_options(rayman2_recompiled PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/include/port_runtime.h"
        -fno-strict-aliasing
        -Wno-unused-variable -Wno-unused-but-set-variable -Wno-parentheses)
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
foreach(dep RT64 N64ModernRuntime)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/${dep}/CMakeLists.txt")
        message(FATAL_ERROR "lib/${dep} is empty. Run: git submodule update --init --recursive")
    endif()
endforeach()

# The bundled SDL 2.26.3 header defines _m_prefetch, which clang treats as a
# builtin and rejects redefining. RT64 works around it by defining the include
# guard of clang's own prfchwintrin.h, but does so with add_compile_definitions
# inside its own directory, so anything else that includes SDL.h -- the frontend
# and this port -- hits the error. Defining it here covers every target.
#
# This is the fourth setting RT64 keeps at directory scope that a consumer needs
# (see also DXC, the DXC_*_OPTS and the SDL2 paths below). They are grouped here
# rather than patched into the submodule, which stays a clean upstream checkout.
if(WIN32)
    add_compile_definitions(NOMINMAX __PRFCHWINTRIN_H)
endif()

# RT64 bundles SDL2 on Windows and points at it with directory-scoped variables,
# which -- like DXC below -- are invisible to sibling directories and to this
# one. RecompFrontend includes SDL.h, and so does src/main.cpp, so the paths are
# set here where every target below can see them.
set(RAYMAN2_SDL2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/RT64/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3")
if(WIN32)
    set(SDL2_INCLUDE_DIRS "${RAYMAN2_SDL2_DIR}/include")
    set(SDL2_LIBRARIES "SDL2" "SDL2main")
    link_directories("${RAYMAN2_SDL2_DIR}/lib/x64")
    # recompinput reads ${sdl2_SOURCE_DIR} on Windows -- the name FetchContent
    # would populate had the parent project fetched SDL2 itself. Pointing it at
    # the copy RT64 already bundles satisfies that without pulling a second SDL
    # into the build, which would risk two different SDLs in one executable.
    set(sdl2_SOURCE_DIR "${RAYMAN2_SDL2_DIR}")
else()
    find_package(SDL2 REQUIRED)
endif()

# RT64 defaults to a SHARED library. Its import library then exports only what
# is explicitly dllexported, and the Application/State/RDP classes that
# RecompFrontend calls into are not -- so a shared build links with dozens of
# undefined RT64 symbols. The frontend assumes a static RT64 (recompinput sets
# this too, but in its own scope, far too late to affect how RT64 was built).
set(RT64_STATIC ON CACHE BOOL "" FORCE)

add_subdirectory(lib/RT64)
add_subdirectory(lib/N64ModernRuntime)

if(RAYMAN2_ENABLE_FRONTEND)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/RecompFrontend/CMakeLists.txt")
        message(FATAL_ERROR "lib/RecompFrontend is empty. Run: git submodule update --init --recursive")
    endif()
    # RT64 defines build_vertex_shader/build_pixel_shader, which RecompFrontend
    # calls for its own UI shaders. The function body expands ${DXC}, but RT64
    # sets that as a DIRECTORY-scoped variable inside lib/RT64, so it is empty by
    # the time a sibling directory calls the function -- and the generated build
    # rule then tries to execute the .hlsl file itself as the compiler, produces
    # an empty output, and fails a step later in file_to_c with a message that
    # says nothing about the real cause. Setting it here, in the parent scope,
    # propagates it to every subdirectory added below.
    # The shader options are directory-scoped in RT64 too, and must be supplied
    # here for the same reason. Note that the target profile is passed as TWO
    # tokens ("-T" "vs_6_0"): RT64's own definitions join them ("-T vs_6_3"),
    # which survives inside its directory but arrives at dxc as a single
    # argument from here, and dxc then reports "Target profile argument is
    # missing" -- an error about the shader that is really about CMake quoting.
    set(DXC_COMMON_OPTS "-I${CMAKE_CURRENT_SOURCE_DIR}/lib/RecompFrontend/recompui/shaders")
    set(DXC_DXIL_OPTS   "-Wno-ignored-attributes")
    set(DXC_SPV_OPTS    "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
    set(DXC_PS_OPTS     "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T" "ps_6_0")
    set(DXC_VS_OPTS     "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T" "vs_6_0" "-fvk-invert-y")

    if(WIN32)
        set(DXC "${CMAKE_CURRENT_SOURCE_DIR}/lib/RT64/src/contrib/dxc/bin/x64/dxc.exe")
    else()
        set(DXC "LD_LIBRARY_PATH=${CMAKE_CURRENT_SOURCE_DIR}/lib/RT64/src/contrib/dxc/lib/x64"
                "${CMAKE_CURRENT_SOURCE_DIR}/lib/RT64/src/contrib/dxc/bin/x64/dxc-linux")
    endif()
    if(WIN32 AND NOT EXISTS "${DXC}")
        message(FATAL_ERROR "RT64's bundled DXC is missing: ${DXC}
"
                            "Run: git submodule update --init --recursive")
    endif()

    # RecompFrontend links against the runtime and the renderer, and locates both
    # by cache variable rather than by target, so these must be set before it is
    # added. They are the same checkouts the port itself uses, not copies.
    set(RECOMP_FRONTEND_N64MODERNRUNTIME_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lib/N64ModernRuntime"
        CACHE PATH "" FORCE)
    set(RECOMP_FRONTEND_RT64_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lib/RT64"
        CACHE PATH "" FORCE)
    add_subdirectory(lib/RecompFrontend)
endif()

# ---------------------------------------------------------------------------
# The host executable
# ---------------------------------------------------------------------------
set(PORT_SOURCES
    src/main.cpp
    src/rsp.cpp
    RecompiledRsp/rsp_audio.cpp
    src/port_runtime.cpp
    # Controller Pak: the 32 KiB backing store and the joybus transactions that
    # reach it. Both builds get these -- saving is not a frontend feature, and
    # the headless build is what the pak was brought up under.
    src/controller_pak.cpp
    src/si_pak.cpp
    # F9: capture a frame's worth of evidence for a graphics issue.
    src/capture.cpp
    # Draw distance, applied at the game's own guPerspective.
    src/draw_distance.cpp
    # The frame cap that gives the game back the rate the RDP used to impose
    # on it. Both builds, because a measurement taken against one and a fix
    # shipped in the other is how docs/issues/004 stayed hidden.
    src/frame_pacing.cpp
    # RAYMAN2_DEMOSCAN: find the game's attract-mode flag. See the file.
    src/demo_scan.cpp
    # The Cheats tab, and the memory search that finds the addresses it needs.
    src/cheats.cpp
    src/memory_search.cpp
    # Finds Rayman's health each session; its address is heap and moves.
    src/health_locator.cpp
    src/register_sections.cpp
    src/spin_yield.cpp
    src/crash_report.cpp
    # The per-session debug report: what a playtester sends when something goes
    # wrong. Mirrors stdout and stderr into it, so it carries RT64's and
    # librecomp's messages and not only the port's.
    src/debug_report.cpp
    src/thread_sampler.cpp
    src/debug_node.cpp)

# The renderer context. With the frontend, RecompFrontend owns the RT64
# application (its menus draw as an overlay and need the render hooks), so the
# port adapts to its factory. Without it, the port brings RT64 up itself --
# which is what makes the game bootable with no UI at all, and therefore what
# makes "does the game run" answerable separately from "do the menus work".
if(RAYMAN2_ENABLE_FRONTEND)
    list(APPEND PORT_SOURCES src/render_context.cpp)
else()
    list(APPEND PORT_SOURCES src/rt64_context.cpp)
endif()

# The recompiled microcode needs SSE4.1.
#
# librecomp's RSP vector unit implements the RSP's vector instructions with x86
# intrinsics, and several of them -- VCL, VMADN and others -- reach for
# _mm_blendv_epi8, which is SSE4.1. It is an always_inline, so without the
# feature enabled the build fails rather than falling back to something slower.
#
# The flag is applied to this one translation unit rather than to the whole
# port. Every other object here is either generated MIPS-to-C, which gains
# nothing from it, or host code where raising the instruction-set floor for the
# whole executable would be a portability decision made by accident.
set_source_files_properties(RecompiledRsp/rsp_audio.cpp PROPERTIES
    COMPILE_OPTIONS "-msse4.1")

add_executable(rayman2-recomp ${PORT_SOURCES})

target_include_directories(rayman2-recomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/RecompiledFuncs"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/N64Recomp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/RecompFrontend/recompui/include"
    # RT64 keeps its own include directories PRIVATE, so linking the target does
    # not bring its headers along; src/rt64_context.cpp includes them directly.
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/RT64/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/RT64/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/RT64/src/contrib"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/RT64/src/contrib/hlslpp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/RT64/src/contrib/dxc/inc"
    ${SDL2_INCLUDE_DIRS})

target_link_libraries(rayman2-recomp PRIVATE
    rayman2_recompiled
    librecomp
    ultramodern
    rt64)

# RT64's src/shared headers are dual-purpose: the same files are compiled as
# HLSL and as C++. HLSL_CPU is what makes `uint`, `float3` and `select` resolve
# to hlslpp's C++ equivalents instead of HLSL builtins. RT64 defines it with
# add_compile_definitions inside its own directory -- the same directory-scope
# habit as DXC and the SDL paths -- so any target that includes those headers
# has to define it too, or it gets a wall of "unknown type name 'uint'".
target_compile_definitions(rayman2-recomp PRIVATE HLSL_CPU)

if(RAYMAN2_ENABLE_FRONTEND)
    target_compile_definitions(rayman2-recomp PRIVATE RAYMAN2_ENABLE_FRONTEND)
    target_link_libraries(rayman2-recomp PRIVATE recompui recompinput)
    # The launcher, config menus and input binding. Only meaningful with the
    # frontend, and it will not compile without its headers.
    target_sources(rayman2-recomp PRIVATE src/frontend.cpp)
endif()

target_link_libraries(rayman2-recomp PRIVATE ${SDL2_LIBRARIES})

if(WIN32)
    target_link_libraries(rayman2-recomp PRIVATE winmm)

    # RT64's D3D12 backend loads the shader compiler at runtime, and SDL2 is a
    # DLL here, so all three have to sit next to the executable.
    foreach(dll "${RAYMAN2_SDL2_DIR}/lib/x64/SDL2.dll"
                "${CMAKE_CURRENT_SOURCE_DIR}/lib/RT64/src/contrib/dxc/bin/x64/dxcompiler.dll"
                "${CMAKE_CURRENT_SOURCE_DIR}/lib/RT64/src/contrib/dxc/bin/x64/dxil.dll")
        add_custom_command(TARGET rayman2-recomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:rayman2-recomp>")
    endforeach()
endif()

set_target_properties(rayman2-recomp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(RAYMAN2_ENABLE_FRONTEND)
    # recompui resolves assets as <exe dir>/assets/<name> and throws during
    # renderer bring-up if the registered primary font is not there. The throw
    # escapes through a noexcept boundary, so the failure mode is a silent
    # fail-fast exit with no message at all -- worth staging carefully.
    #
    # The fonts come from RmlUi's samples (SIL Open Font License 1.1); they are
    # copied at build time rather than committed, so no font binary lives in
    # this repository.
    # The port's own stylesheet. recompui loads assets/recomp.rcss by name and
    # throws during bring-up if it is absent -- on the renderer thread, so the
    # process fail-fasts with no message.
    add_custom_command(TARGET rayman2-recomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:rayman2-recomp>/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/recomp.rcss"
                "$<TARGET_FILE_DIR:rayman2-recomp>/assets/recomp.rcss")

    set(RMLUI_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/lib/RecompFrontend/recompui/lib/RmlUi/Samples/assets")
    foreach(font LatoLatin-Regular.ttf LatoLatin-Bold.ttf LatoLatin-Italic.ttf
                 LatoLatin-BoldItalic.ttf NotoEmoji-Regular.ttf)
        add_custom_command(TARGET rayman2-recomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:rayman2-recomp>/assets"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${RMLUI_ASSETS}/${font}" "$<TARGET_FILE_DIR:rayman2-recomp>/assets/${font}")
    endforeach()

    # promptfont: the glyph font recompui uses to draw controller and keyboard
    # symbols on binding buttons. Unlike the icons, a missing file here is not a
    # missing button -- ui_state.cpp loads assets/promptfont/promptfont.ttf by
    # name during renderer bring-up, before anything is drawn, and the frontend
    # reports the font as missing. It is not part of RmlUi's samples, so it is
    # committed here (SIL Open Font License 1.1, licence text alongside it).
    add_custom_command(TARGET rayman2-recomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:rayman2-recomp>/assets/promptfont"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/promptfont"
                "$<TARGET_FILE_DIR:rayman2-recomp>/assets/promptfont")

    # The menu icons. recompui builds its header buttons from assets/icons/*.svg
    # by name -- Quit and X in the config modal, Caret in dropdowns, Cont and
    # Keyboard in the Controls tab, and so on -- so a missing file is a missing
    # button rather than an error. These are the same set the sibling ports use,
    # so the three look like one family.
    add_custom_command(TARGET rayman2-recomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:rayman2-recomp>/assets/icons"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons"
                "$<TARGET_FILE_DIR:rayman2-recomp>/assets/icons")
endif()
