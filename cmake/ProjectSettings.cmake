# Compile settings: LTO (all targets, see below), sanitizers, and the warnings / definitions of the Zalie targets.

# Link-time optimization of Release builds, for every target (the dependencies are linked statically: LTO inlines and
# strips across them too). Set here, before the dependencies are added, through the variables that initialize the
# INTERPROCEDURAL_OPTIMIZATION property of the targets. The plain variable for single-config generators (only when
# building Release), the per-config one for multi-config generators (Visual Studio, Xcode, Ninja Multi-Config):
# these are the ones zlib-ng resets to opt out (its runtime CPU dispatch must not be optimized across files).
set(ZALIE_LTO_STATUS "off")
get_property(ZALIE_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(ZALIE_ENABLE_LTO AND (ZALIE_MULTI_CONFIG OR CMAKE_BUILD_TYPE STREQUAL "Release"))
  if(NOT CMAKE_C_COMPILER_ID STREQUAL CMAKE_CXX_COMPILER_ID)
    # e.g. GCC for C++ and Clang for C / Objective-C on macOS: their LTO objects (GIMPLE / LLVM bitcode) cannot be
    # linked together.
    set(ZALIE_LTO_STATUS "off (C: ${CMAKE_C_COMPILER_ID}, C++: ${CMAKE_CXX_COMPILER_ID})")
  else()
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ZALIE_IPO_SUPPORTED OUTPUT ZALIE_IPO_OUTPUT LANGUAGES C CXX)
    if(ZALIE_IPO_SUPPORTED)
      if(ZALIE_MULTI_CONFIG)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(ZALIE_LTO_STATUS "Release config")
      else()
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        set(ZALIE_LTO_STATUS "on")
      endif()
      if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # GCC 13 false positive, emitted at link time once LTO has inlined aeronet's HttpClient constructor
        # ('string_view::find' bound "exceeds maximum object size").
        add_link_options($<$<CONFIG:Release>:-Wno-stringop-overread>)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
          # zstd compiles its sources with '-Wa,--noexecstack'. GCC's LTO plugin wants the same assembler options in
          # every LTO object, or it warns at each link ("Extra option to '-Xassembler'") and drops them: give the option
          # to all the targets (zstd excepted, see Dependencies.cmake). Redundant, GCC already marks the stack of its
          # code as non-executable, but cheaper than linking zstd without LTO (+66 KB).
          set(ZALIE_LTO_ASSEMBLER_OPTION "$<$<CONFIG:Release>:-Wa,--noexecstack>")
          add_compile_options("${ZALIE_LTO_ASSEMBLER_OPTION}")
        endif()
      endif()
    else()
      set(ZALIE_LTO_STATUS "off (not supported by the toolchain)")
      message(STATUS "[zalie] LTO is not supported by this toolchain: ${ZALIE_IPO_OUTPUT}")
    endif()
  endif()
endif()

if(ZALIE_ENABLE_ASAN)
  if(MSVC)
    add_compile_options(/fsanitize=address)
  else()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

# Applies warnings, platform definitions and the version string to a Zalie target.
function(zalie_set_target_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /utf-8 /Zc:__cplusplus /Zc:preprocessor /permissive- /EHsc)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    if(ZALIE_ENABLE_WARNINGS)
      # C4324 only reports the padding that alignas asks for.
      target_compile_options(${target} PRIVATE /W4 /wd4324)
      if(ZALIE_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE /WX)
      endif()
    endif()
  else()
    if(ZALIE_ENABLE_WARNINGS)
      target_compile_options(
        ${target}
        PRIVATE -Wall
                -Wextra
                -Wpedantic
                -Wshadow
                -Wnon-virtual-dtor
                -Wold-style-cast
                -Wcast-align
                -Woverloaded-virtual
                -Wimplicit-fallthrough
                -Wformat=2
                -Wmissing-declarations
                # Designated initializers deliberately omit the fields keeping their default member initializer.
                -Wno-missing-field-initializers)
      if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # The narrowing MSVC reports at /W4 (C4267), caught on every platform: the amc containers have 32-bit sizes,
        # indexing them with a std::size_t narrows it.
        target_compile_options(${target} PRIVATE -Wshorten-64-to-32)
      endif()
      if(ZALIE_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
      endif()
    endif()
  endif()
  target_compile_definitions(${target} PRIVATE ZALIE_VERSION_STR="${ZALIE_VERSION_STRING}")
endfunction()
