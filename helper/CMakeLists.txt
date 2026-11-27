# fugue-helper: Fugue's stable-diffusion.cpp (the `fugue` branch of github.com/bmaltais/stable-diffusion.cpp,
# upstream plus Fugue's changes as commits) at a pinned commit (spec 8.10).
# A project of its own, so the app's build and tests never compile the inference engine:
#
#   cmake -S helper -B build-helper -DFUGUE_HELPER_BACKEND=cuda   # or vulkan, cpu
#   cmake --build build-helper
#
# The pin moves only on purpose (spec 8.10): change the three commits and their hashes together,
# take the submodule commits from `git ls-tree <commit> ggml thirdparty/cutlass` of
# stable-diffusion.cpp (README.md). CI checks that packaging/PKGBUILD names the same commits.
# A packager that downloads the sources itself passes FETCHCONTENT_SOURCE_DIR_SDCPP,
# FETCHCONTENT_SOURCE_DIR_SDCPP_GGML and FETCHCONTENT_SOURCE_DIR_SDCPP_CUTLASS; so does
# bin/build-helper with FUGUE_SDCPP_SOURCE=<a local clone of the fork> (a developer's engine edits).
cmake_minimum_required(VERSION 3.25)
project(fugue_helper LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(FUGUE_HELPER_BACKEND "cpu" CACHE STRING "GPU backend of the helper: cuda, vulkan or cpu")
set_property(CACHE FUGUE_HELPER_BACKEND PROPERTY STRINGS cuda vulkan cpu)
if(NOT FUGUE_HELPER_BACKEND MATCHES "^(cuda|vulkan|cpu)$")
  message(FATAL_ERROR "FUGUE_HELPER_BACKEND must be cuda, vulkan or cpu, not '${FUGUE_HELPER_BACKEND}'")
endif()

# The fork's tag fugue-2026-11-24c (upstream 3b12781f plus Fugue's commits).
set(sdcpp_commit 03da399d318503d09bec3047123f1edb50941170)
set(sdcpp_sha256 e76d6a090d804bbb7d8c70f4d7d89f74edb8c825be9dcedad4fa820ca732e484)
set(ggml_commit eeda9ec48fc84a3df0bb9dcc19ae9053075a2539)
set(ggml_sha256 8bd7adc7eb1366d31670467951d21eb1a5f55e8f6d89b9b91909ffb0246ac5d2)
set(cutlass_commit ad7b2f5e84fcfa124cb02b91d5bd26d238c0459e)
set(cutlass_sha256 36429479df2ab1cd1c7981c2d34e786106326ccfefc8846f902f76151a65e112)

include(FetchContent)
# Fetched only: stable-diffusion.cpp adds ggml from this path, after setting the backend options
# (adding it here would build a CPU-only ggml first).
FetchContent_Declare(sdcpp_ggml
  URL https://github.com/leejet/ggml/archive/${ggml_commit}.tar.gz
  URL_HASH SHA256=${ggml_sha256}
  SOURCE_SUBDIR none)
FetchContent_MakeAvailable(sdcpp_ggml)
FetchContent_GetProperties(sdcpp_ggml SOURCE_DIR ggml_dir)
set(SD_GGML_SOURCE_DIR "${ggml_dir}" CACHE PATH "" FORCE)

if(FUGUE_HELPER_BACKEND STREQUAL "cuda")
  FetchContent_Declare(sdcpp_cutlass
    URL https://github.com/NVIDIA/cutlass/archive/${cutlass_commit}.tar.gz
    URL_HASH SHA256=${cutlass_sha256}
    SOURCE_SUBDIR none)  # headers only: nothing to add
  FetchContent_MakeAvailable(sdcpp_cutlass)
  FetchContent_GetProperties(sdcpp_cutlass SOURCE_DIR cutlass_dir)
  set(SD_CUDA_CUTLASS_PATH "${cutlass_dir}" CACHE PATH "" FORCE)
  set(SD_CUDA ON CACHE BOOL "" FORCE)
elseif(FUGUE_HELPER_BACKEND STREQUAL "vulkan")
  set(SD_VULKAN ON CACHE BOOL "" FORCE)
endif()

# A package runs on other machines than the one that built it.
set(GGML_NATIVE OFF CACHE BOOL "")
set(SD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SD_WEBP OFF CACHE BOOL "" FORCE)
set(SD_WEBM OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# Fixed source path: ccache then hits for every file an engine change did not touch, also across
# a pin move (an unpacked source in a new folder would recompile everything).
FetchContent_Declare(sdcpp
  URL https://github.com/bmaltais/stable-diffusion.cpp/archive/${sdcpp_commit}.tar.gz
  URL_HASH SHA256=${sdcpp_sha256}
  SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/sdcpp-src
  SYSTEM)  # its headers (and thirdparty/: json.hpp, stb) warn under our flags; they are not ours
FetchContent_MakeAvailable(sdcpp)

add_executable(fugue-helper src/main.cpp src/restore.h)
target_link_libraries(fugue-helper PRIVATE stable-diffusion)
# cpp-httplib (--serve-http) is in stable-diffusion.cpp's thirdparty/ too.
target_include_directories(fugue-helper SYSTEM PRIVATE ${sdcpp_SOURCE_DIR}/thirdparty)
target_compile_definitions(fugue-helper PRIVATE FUGUE_BACKEND="${FUGUE_HELPER_BACKEND}")
target_compile_options(fugue-helper PRIVATE -Wall -Wextra)
set_target_properties(fugue-helper PROPERTIES OUTPUT_NAME fugue-helper-${FUGUE_HELPER_BACKEND})
# The app looks for its helpers in lib/fugue (#78 picks one). Install with
# `--component fugue-helper`: stable-diffusion.cpp and ggml have install rules of their own.
install(TARGETS fugue-helper RUNTIME DESTINATION lib/fugue COMPONENT fugue-helper)

# The restore op's program against the engine's checker (no model, no GPU): not built by default.
enable_testing()
add_executable(fugue-helper-restore-test EXCLUDE_FROM_ALL test/restore_test.cpp)
target_link_libraries(fugue-helper-restore-test PRIVATE stable-diffusion)
target_include_directories(fugue-helper-restore-test PRIVATE src)
target_compile_options(fugue-helper-restore-test PRIVATE -Wall -Wextra)
add_test(NAME restore-program COMMAND fugue-helper-restore-test)
