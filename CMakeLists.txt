cmake_minimum_required(VERSION 3.20)
# The single source of the version number is the VERSION file (MAJOR.MINOR.PATCH). Bump it for every delivery:
# the installer, the tools and Coyote Studio all derive their version from it.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" COYOTE_VERSION_STRING LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
project(Coyote VERSION ${COYOTE_VERSION_STRING} DESCRIPTION "Coyote programming language and toolchain" LANGUAGES CXX)

# Windows: link the C/C++ runtime statically so the tools run without the Visual C++ redistributable.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(MINGW)
  add_link_options(-static)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(COYOTE_BUILD_TESTS "Build and register the test suite" ON)
option(COYOTE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

function(coyote_target_defaults target)
  target_compile_definitions(${target} PRIVATE COYOTE_VERSION="${PROJECT_VERSION}")
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    if(COYOTE_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(COYOTE_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# Compiler: .coy -> ML. Shares nothing with the PVM except the textual ML format.
add_library(coyote_compiler STATIC
  src/compiler/lexer.cpp
  src/compiler/parser.cpp
  src/compiler/sema.cpp
  src/compiler/mlgen.cpp
  src/compiler/modules.cpp
  src/compiler/libpack.cpp)
target_include_directories(coyote_compiler PUBLIC src/compiler)
coyote_target_defaults(coyote_compiler)

add_executable(coyote src/compiler/main.cpp)
coyote_target_defaults(coyote)

# Pixel Virtual Machine: loads, verifies and runs ML. No dependency on the compiler (or LLVM).
add_library(pvm_core STATIC
  src/pvm/loader.cpp
  src/pvm/verifier.cpp
  src/pvm/interpreter.cpp
  src/pvm/heap.cpp
  src/pvm/ops.cpp
  src/pvm/host.cpp
  src/pvm/debugger.cpp)
target_include_directories(pvm_core PUBLIC src/pvm)
coyote_target_defaults(pvm_core)

add_executable(pvm src/pvm/main.cpp)
target_link_libraries(pvm PRIVATE pvm_core)
coyote_target_defaults(pvm)

# Native backend: emits LLVM IR text and drives clang; shares only the ML loader/verifier with the PVM.
add_executable(coyote-llvm src/llvm/main.cpp)
target_link_libraries(coyote-llvm PRIVATE pvm_core)
coyote_target_defaults(coyote-llvm)

# Project tools: packages.json handling and process running (no dependency on the compiler or PVM).
add_library(coyote_pkg STATIC
  src/pkg/json.cpp
  src/pkg/manifest.cpp
  src/pkg/sha256.cpp
  src/pkg/resolver.cpp
  src/pkg/lib.cpp
  src/pkg/process.cpp)
target_include_directories(coyote_pkg PUBLIC src/pkg)
coyote_target_defaults(coyote_pkg)

target_link_libraries(coyote_compiler PUBLIC coyote_pkg)
target_link_libraries(coyote PRIVATE coyote_compiler coyote_pkg)

add_executable(cpx src/cpx/main.cpp)
target_link_libraries(cpx PRIVATE coyote_pkg)
coyote_target_defaults(cpx)

add_executable(cpm src/cpm/main.cpp)
target_link_libraries(cpm PRIVATE coyote_pkg)
coyote_target_defaults(cpm)

add_executable(cdt src/cdt/main.cpp src/cdt/format.cpp)
target_include_directories(cdt PRIVATE src/cdt)
target_link_libraries(cdt PRIVATE coyote_compiler coyote_pkg)
coyote_target_defaults(cdt)

# The standard library: packed from stdlib/ with the compiler we just built, next to the tools (<tools>/libs).
file(GLOB COYOTE_STDLIB_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/stdlib/src/*.coy)
if(CMAKE_CROSSCOMPILING)
  message(STATUS "cross-compiling: standard.lib is not packed (the compiler cannot run on this machine); copy one into <tools>/libs")
else()
  add_custom_target(stdlib ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:coyote>/libs
    COMMAND coyote lib pack ${CMAKE_SOURCE_DIR}/stdlib -o $<TARGET_FILE_DIR:coyote>/libs/standard.lib
    DEPENDS coyote ${COYOTE_STDLIB_SOURCES} ${CMAKE_SOURCE_DIR}/stdlib/packages.json
    COMMENT "Packing the Standard library"
    VERBATIM)
endif()

install(TARGETS coyote pvm coyote-llvm cpx cpm cdt RUNTIME DESTINATION bin)
install(FILES $<TARGET_FILE_DIR:coyote>/libs/standard.lib DESTINATION libs OPTIONAL)
install(FILES README.md LICENSE DESTINATION share/coyote OPTIONAL)
install(DIRECTORY docs samples DESTINATION share/coyote)
install(DIRECTORY assets/brand DESTINATION share/coyote/assets)

if(COYOTE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
