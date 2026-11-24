cmake_minimum_required(VERSION 3.18)
project(esim VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ESIM_BUILD_TESTS "Build the test suite" ON)
option(ESIM_BUILD_EXAMPLES "Build the C++ examples" ON)
option(ESIM_BUILD_EDITOR "Build the editor (needs Qt 6.5 or later)" ON)

# --- Dependencies: Eigen (linear algebra) and nlohmann/json (input files) ---
# Installed packages are preferred; otherwise both are downloaded.
include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)  # extracted files get the extraction time stamp
endif()

find_package(Eigen3 QUIET NO_MODULE)
if(NOT TARGET Eigen3::Eigen)
  message(STATUS "Eigen3 not found, downloading it")
  # Header-only: SOURCE_SUBDIR points to a directory without CMakeLists.txt so
  # that Eigen's own (slow) CMake project is not configured.
  FetchContent_Declare(eigen
    URL https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz
    SOURCE_SUBDIR cmake-not-used)
  FetchContent_MakeAvailable(eigen)
  add_library(Eigen3::Eigen INTERFACE IMPORTED)
  target_include_directories(Eigen3::Eigen INTERFACE ${eigen_SOURCE_DIR})
endif()

find_package(nlohmann_json QUIET)
if(NOT TARGET nlohmann_json::nlohmann_json)
  message(STATUS "nlohmann_json not found, downloading it")
  FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
  FetchContent_MakeAvailable(json)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set(ESIM_WARNINGS -Wall -Wextra -Wpedantic)
endif()
if(MSVC)
  # sources are UTF-8 (texts like "Ω" and German quotes); large objects
  # (nlohmann/json, Eigen); standard conformance; no min/max macros
  add_compile_options(/utf-8 /bigobj /permissive- /Zc:__cplusplus)
  add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

# Windows: UTF-8 as the code page of every program (see cmake/utf8.manifest).
function(esim_windows_program target)
  if(WIN32)
    if(MSVC)
      target_sources(${target} PRIVATE ${PROJECT_SOURCE_DIR}/cmake/utf8.manifest)
    else()
      target_sources(${target} PRIVATE ${PROJECT_SOURCE_DIR}/cmake/utf8.rc)
    endif()
  endif()
endfunction()

# --- esimcore: generic DAE solvers and expressions, knows nothing about circuits
add_library(esimcore
  src/expr/Expression.cpp
  src/core/LinearSolver.cpp
  src/core/Newton.cpp
  src/core/DcSolver.cpp
  src/core/Transient.cpp
  src/core/StructuralIndex.cpp)
target_include_directories(esimcore PUBLIC src)
target_link_libraries(esimcore PUBLIC Eigen3::Eigen)
target_compile_options(esimcore PRIVATE ${ESIM_WARNINGS})

# --- esimcircuit: devices, analyses, JSON input, CSV output -----------------
add_library(esimcircuit
  src/circuit/Circuit.cpp
  src/circuit/Waveform.cpp
  src/circuit/Analysis.cpp
  src/circuit/IndexReduction.cpp
  src/circuit/devices/Passive.cpp
  src/circuit/devices/Sources.cpp
  src/circuit/devices/Diode.cpp
  src/circuit/devices/Bjt.cpp
  src/circuit/devices/Mosfet.cpp
  src/circuit/devices/Jfet.cpp
  src/circuit/devices/Bsim3.cpp
  src/circuit/devices/TransmissionLine.cpp
  src/circuit/devices/Equations.cpp
  src/circuit/devices/Behavioral.cpp
  src/circuit/devices/Switching.cpp
  src/circuit/devices/Plugin.cpp
  src/io/Units.cpp
  src/io/Netlist.cpp
  src/io/Spice.cpp
  src/io/Output.cpp
  src/app/Measure.cpp
  src/app/Runner.cpp)
find_package(Threads REQUIRED)
target_link_libraries(esimcircuit PUBLIC esimcore nlohmann_json::nlohmann_json Threads::Threads
                      ${CMAKE_DL_LIBS})
# plugin/esim_plugin.h: the interface of the plugin libraries (plain C)
target_include_directories(esimcircuit PUBLIC ${PROJECT_SOURCE_DIR})
target_compile_options(esimcircuit PRIVATE ${ESIM_WARNINGS})

add_executable(esim src/main.cpp)
target_link_libraries(esim PRIVATE esimcircuit)
target_compile_options(esim PRIVATE ${ESIM_WARNINGS})
esim_windows_program(esim)

if(ESIM_BUILD_EXAMPLES)
  # A DAE that is not a circuit, solved with esimcore alone.
  add_executable(dae_robertson examples/dae/robertson_main.cpp)
  target_link_libraries(dae_robertson PRIVATE esimcore)
endif()

if(ESIM_BUILD_TESTS)
  enable_testing()
  add_executable(esim_tests tests/tests.cpp)
  esim_windows_program(esim_tests)
  target_link_libraries(esim_tests PRIVATE esimcircuit)
  target_include_directories(esim_tests PRIVATE examples)

  # Plugin library for the tests, and a template for real plugins.
  add_library(esim_mock_plugin MODULE plugins/mock/mock_plugin.cpp)
  target_include_directories(esim_mock_plugin PRIVATE ${PROJECT_SOURCE_DIR})
  set_target_properties(esim_mock_plugin PROPERTIES PREFIX "")
  target_compile_options(esim_mock_plugin PRIVATE ${ESIM_WARNINGS})
  # ... and one with the function table of the first version, to check compatibility
  add_library(esim_legacy_plugin MODULE plugins/mock/legacy_plugin.cpp)
  target_include_directories(esim_legacy_plugin PRIVATE ${PROJECT_SOURCE_DIR})
  set_target_properties(esim_legacy_plugin PROPERTIES PREFIX "")
  add_dependencies(esim_tests esim_mock_plugin esim_legacy_plugin)
  target_compile_definitions(esim_tests PRIVATE ESIM_MOCK_PLUGIN="$<TARGET_FILE:esim_mock_plugin>"
                             ESIM_LEGACY_PLUGIN="$<TARGET_FILE:esim_legacy_plugin>")
  add_test(NAME esim_tests COMMAND esim_tests)
  # Charges of a single BSIM3 transistor, for tests/test_bsim3.py.
  add_executable(bsim3_charges tests/bsim3_charges.cpp)
  esim_windows_program(bsim3_charges)
  target_link_libraries(bsim3_charges PRIVATE esimcircuit)

  # The Python interface (python/esim.py) only needs an interpreter.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME python_interface
      COMMAND ${CMAKE_COMMAND} -E env ESIM_BIN=$<TARGET_FILE:esim>
              ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/python/test_esim.py)
    add_test(NAME bsim3_model
      COMMAND ${CMAKE_COMMAND} -E env ESIM_BIN=$<TARGET_FILE:esim>
              BSIM3_CHARGES_BIN=$<TARGET_FILE:bsim3_charges>
              ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_bsim3.py)
    # BSIM3 against ngspice, if it is installed.
    # (on Windows the console program is ngspice_con)
    find_program(NGSPICE_EXECUTABLE NAMES ngspice_con ngspice)
    if(NGSPICE_EXECUTABLE)
      add_test(NAME bsim3_ngspice
        COMMAND ${CMAKE_COMMAND} -E env ESIM_BIN=$<TARGET_FILE:esim>
                BSIM3_CHARGES_BIN=$<TARGET_FILE:bsim3_charges> NGSPICE=${NGSPICE_EXECUTABLE}
                ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/bsim3_ngspice.py)
    endif()
    # SPICE subcircuits and MOSFET level 3 against ngspice, if it is installed.
    if(NGSPICE_EXECUTABLE)
      add_test(NAME mos3_ngspice
        COMMAND ${CMAKE_COMMAND} -E env ESIM_BIN=$<TARGET_FILE:esim> NGSPICE=${NGSPICE_EXECUTABLE}
                ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/mos3_ngspice.py)
      add_test(NAME spice_subckt_ngspice
        COMMAND ${CMAKE_COMMAND} -E env ESIM_BIN=$<TARGET_FILE:esim> NGSPICE=${NGSPICE_EXECUTABLE}
                ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/spice_subckt_ngspice.py)
    endif()
    add_test(NAME spice_import
      COMMAND ${CMAKE_COMMAND} -E env ESIM_BIN=$<TARGET_FILE:esim>
              ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/test_spice2json.py)
  endif()
endif()

# --- the editor (after the tests, so that its test is registered) -------------
if(ESIM_BUILD_EDITOR)
  find_package(Qt6 6.5 QUIET COMPONENTS Widgets Concurrent)
  if(Qt6_FOUND)
    add_subdirectory(editor)
  else()
    message(STATUS "Qt 6 not found, the editor is not built")
  endif()
endif()
