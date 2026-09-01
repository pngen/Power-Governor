cmake_minimum_required(VERSION 3.25)
project(PowerGovernor VERSION 1.0.0
        DESCRIPTION "Power Governor - power/energy/thermal governance runtime"
        LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(GNUInstallDirs)
enable_testing()
option(PG_BUILD_TESTS "Build the test suite" ON)
option(PG_BUILD_EXAMPLES "Build the runnable examples" ON)
option(PG_BUILD_BENCHMARKS "Build the benchmarks" ON)
option(PG_BUILD_CLI "Build the powergovernor CLI" ON)
option(PG_ENABLE_CUDA "Build the NVIDIA/CUDA (sm_120) backend" ON)

# ---------------------------------------------------------------------------
# Header-only core library
# ---------------------------------------------------------------------------
add_library(powergovernor INTERFACE)
add_library(PowerGovernor::powergovernor ALIAS powergovernor)
target_include_directories(powergovernor INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(powergovernor INTERFACE cxx_std_20)
function(pg_target_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# NVIDIA / CUDA backend (sm_120), optional
# ---------------------------------------------------------------------------
set(PG_HAS_CUDA OFF)
set(PG_CUDA_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/powergovernor_cuda/pg_cuda_backend.cu
  ${CMAKE_CURRENT_SOURCE_DIR}/src/powergovernor_cuda/nvidia_cuda_backend.cpp)
if(PG_ENABLE_CUDA
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/powergovernor_cuda/pg_cuda_backend.cu"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/powergovernor_cuda/nvidia_cuda_backend.cpp")
  if(EXISTS "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.1")
    set(CUDAToolkit_ROOT "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.1")
    set(CMAKE_CUDA_COMPILER "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.1/bin/nvcc.exe")
  endif()
  find_package(CUDAToolkit QUIET)
  if(CUDAToolkit_FOUND)
    enable_language(CUDA)
    set(PG_HAS_CUDA ON)
    add_library(powergovernor_cuda STATIC ${PG_CUDA_SOURCES})
    add_library(PowerGovernor::cuda_backend ALIAS powergovernor_cuda)
    target_include_directories(powergovernor_cuda PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
      ${CUDAToolkit_INCLUDE_DIRS})
    target_link_libraries(powergovernor_cuda PUBLIC
      CUDA::cudart
      PowerGovernor::powergovernor)
    target_compile_definitions(powergovernor_cuda PUBLIC PG_HAS_CUDA=1)
    set_target_properties(powergovernor_cuda PROPERTIES
      CUDA_ARCHITECTURES 120
      CUDA_STANDARD 20
      CUDA_STANDARD_REQUIRED ON)
    if(MSVC)
      target_compile_options(powergovernor_cuda PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4>)
      target_compile_options(powergovernor_cuda PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/WX>)
      target_compile_options(powergovernor_cuda PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
      # /W4 for warnings; /wd4211 suppresses an nvcc-generated host-stub warning we cannot fix.
      target_compile_options(powergovernor_cuda PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/W4,/wd4211>)
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PG_BUILD_TESTS)
  file(GLOB PG_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
  add_executable(powergovernor_tests ${PG_TEST_SOURCES})
  pg_target_warnings(powergovernor_tests)
  target_link_libraries(powergovernor_tests PRIVATE PowerGovernor::powergovernor)
  if(PG_HAS_CUDA)
    target_link_libraries(powergovernor_tests PRIVATE PowerGovernor::cuda_backend)
  endif()
  target_include_directories(powergovernor_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(NAME powergovernor_tests COMMAND powergovernor_tests)
endif()

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
if(PG_BUILD_CLI)
  file(GLOB PG_CLI_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/*.cpp)
  add_executable(powergovernor_cli ${PG_CLI_SOURCES})
  set_target_properties(powergovernor_cli PROPERTIES OUTPUT_NAME powergovernor)
  pg_target_warnings(powergovernor_cli)
  target_link_libraries(powergovernor_cli PRIVATE PowerGovernor::powergovernor)
  if(PG_HAS_CUDA)
    target_link_libraries(powergovernor_cli PRIVATE PowerGovernor::cuda_backend)
  endif()
  if(WIN32)
    target_link_libraries(powergovernor_cli PRIVATE ws2_32)
  endif()
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(PG_BUILD_EXAMPLES)
  file(GLOB PG_EXAMPLE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp)
  foreach(src ${PG_EXAMPLE_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(example_${name} ${src})
    pg_target_warnings(example_${name})
    target_link_libraries(example_${name} PRIVATE PowerGovernor::powergovernor)
    if(PG_HAS_CUDA)
      target_link_libraries(example_${name} PRIVATE PowerGovernor::cuda_backend)
    endif()
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(PG_BUILD_BENCHMARKS)
  file(GLOB PG_BENCH_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.cpp)
  add_executable(powergovernor_bench ${PG_BENCH_SOURCES})
  pg_target_warnings(powergovernor_bench)
  target_link_libraries(powergovernor_bench PRIVATE PowerGovernor::powergovernor)
endif()

# ---------------------------------------------------------------------------
# Install / package (CMake config)
# ---------------------------------------------------------------------------
install(TARGETS powergovernor EXPORT PowerGovernorTargets)
if(TARGET powergovernor_cuda)
  install(TARGETS powergovernor_cuda EXPORT PowerGovernorTargets)
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT PowerGovernorTargets
        FILE PowerGovernorTargets.cmake
        NAMESPACE PowerGovernor::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PowerGovernor)
include(CMakePackageConfigHelpers)
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PowerGovernorConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/PowerGovernorConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PowerGovernor)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/PowerGovernorConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/PowerGovernorConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/PowerGovernorConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PowerGovernor)
find_package(Threads REQUIRED)
if(PG_BUILD_TESTS)
  target_link_libraries(powergovernor_tests PRIVATE Threads::Threads)
endif()
if(PG_BUILD_CLI)
  target_link_libraries(powergovernor_cli PRIVATE Threads::Threads)
endif()