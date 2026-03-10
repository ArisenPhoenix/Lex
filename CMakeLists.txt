cmake_minimum_required(VERSION 3.20)

project(Lex LANGUAGES CXX VERSION 0.1.0)

# Helps clangd/lint tooling resolve include paths and flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LEX_BUILD_SHARED "Build Lex as a shared library" OFF)
option(LEX_BUILD_EXAMPLES "Build Lex examples" OFF)

set(LEX_HEADERS
  ${CMAKE_CURRENT_SOURCE_DIR}/include/Scanner.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/include/Structurizer.hpp
)

set(LEX_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/Scanner.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/Structurizer.cpp
)

# Source currently includes headers as "lex/Scanner.hpp" and "lex/Structurizer.hpp".
# Keep repo layout as-is by generating a matching include prefix in the build dir.
# file(MAKE_DIRECTORY ${LEX_GENERATED_INCLUDE_DIR}/lex)
# configure_file(${CMAKE_CURRENT_SOURCE_DIR}/include/Scanner.hpp
#                ${LEX_GENERATED_INCLUDE_DIR}/lex/Scanner.hpp COPYONLY)
# configure_file(${CMAKE_CURRENT_SOURCE_DIR}/include/Structurizer.hpp
#                ${LEX_GENERATED_INCLUDE_DIR}/lex/Structurizer.hpp COPYONLY)

if(LEX_BUILD_SHARED)
  add_library(lex SHARED ${LEX_SOURCES} ${LEX_HEADERS})
else()
  add_library(lex STATIC ${LEX_SOURCES} ${LEX_HEADERS})
endif()

add_library(Lex::lex ALIAS lex)

set_target_properties(lex PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS NO
  POSITION_INDEPENDENT_CODE ON
)

target_include_directories(lex
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(MSVC)
  target_compile_options(lex PRIVATE /W4)
else()
  target_compile_options(lex PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(LEX_BUILD_EXAMPLES)
  message(STATUS "LEX_BUILD_EXAMPLES is ON, but no examples are defined yet")
endif()

include(GNUInstallDirs)
install(TARGETS lex
  EXPORT LexTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES ${LEX_HEADERS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lex)

install(EXPORT LexTargets
  NAMESPACE Lex::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Lex
)
