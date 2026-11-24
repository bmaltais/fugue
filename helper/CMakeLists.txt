# fugue-helper: stable-diffusion.cpp at a pinned commit with Fugue's stage patch (spec 8.10).
# A project of its own, so the app's build and tests never compile the inference engine:
#
#   cmake -S helper -B build-helper -DFUGUE_HELPER_BACKEND=cuda   # or vulkan, cpu
#   cmake --build build-helper
#
# The pin moves only on purpose (spec 8.10): change the three commits and their hashes together,
# take the submodule commits from `git ls-tree <commit> ggml thirdparty/cutlass` of
# stable-diffusion.cpp, and refresh sdcpp.patch against the new commit (README.md). CI checks
# that packaging/PKGBUILD names the same commits.
# A packager that downloads the sources itself passes FETCHCONTENT_SOURCE_DIR_SDCPP (patched
# already), FETCHCONTENT_SOURCE_DIR_SDCPP_GGML and FETCHCONTENT_SOURCE_DIR_SDCPP_CUTLASS.
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

set(sdcpp_commit 3b12781fac1a22eda27ad0f08d0884ef816f24d7)
set(sdcpp_sha256 1122fa32b5e2d5dc376da0220bcd10c741f76eb5a6dad8170d5a6582979dc2a9)
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

# patch, not git apply: inside another git checkout (this one) git apply skips every file.
find_program(PATCH_EXECUTABLE patch REQUIRED)
# FetchContent runs PATCH_COMMAND only when it extracts the source, so an existing build folder
# would keep building the old patch (#374). The patch's hash is kept beside the source: when the
# patch changed, the source and its populate step are removed and extracted and patched again at
# the same path. The same path matters: ccache then hits for every file the patch did not change,
# so a patch edit rebuilds in seconds, not the 11 minutes of a new folder (#382).
set(sdcpp_patch ${CMAKE_CURRENT_SOURCE_DIR}/sdcpp.patch)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${sdcpp_patch})
file(SHA256 ${sdcpp_patch} sdcpp_patch_sha256)
set(sdcpp_source_dir ${FETCHCONTENT_BASE_DIR}/sdcpp-src)
set(sdcpp_patch_stamp ${FETCHCONTENT_BASE_DIR}/sdcpp-patch.sha256)
set(sdcpp_patched_with "")
if(EXISTS ${sdcpp_patch_stamp})
  file(READ ${sdcpp_patch_stamp} sdcpp_patched_with)
endif()
if(NOT sdcpp_patched_with STREQUAL sdcpp_patch_sha256)
  file(REMOVE_RECURSE ${sdcpp_source_dir} ${FETCHCONTENT_BASE_DIR}/sdcpp-subbuild)
endif()
# The hash-named folders a build made between #374 and this change are only disk space now.
file(GLOB sdcpp_old_sources LIST_DIRECTORIES true ${FETCHCONTENT_BASE_DIR}/sdcpp-src-*)
if(sdcpp_old_sources)
  file(REMOVE_RECURSE ${sdcpp_old_sources})
endif()
FetchContent_Declare(sdcpp
  URL https://github.com/leejet/stable-diffusion.cpp/archive/${sdcpp_commit}.tar.gz
  URL_HASH SHA256=${sdcpp_sha256}
  SOURCE_DIR ${sdcpp_source_dir}
  SYSTEM  # its headers (and thirdparty/: json.hpp, stb) warn under our flags; they are not ours
  PATCH_COMMAND ${PATCH_EXECUTABLE} -p1 --forward --input=${sdcpp_patch})
FetchContent_MakeAvailable(sdcpp)
file(WRITE ${sdcpp_patch_stamp} ${sdcpp_patch_sha256})

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
