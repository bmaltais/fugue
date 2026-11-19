cmake_minimum_required(VERSION 3.21)
project(fugue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 DBus Test)
find_package(PkgConfig REQUIRED)
pkg_check_modules(VIPS REQUIRED IMPORTED_TARGET vips-cpp)

# The engine: documents, compiler, scheduler, cache, providers. It runs with
# no display (the CLI and tests use it headless), so it never links Gui or
# Quick. A new file in src/engine/ needs no line here.
file(GLOB fugue_engine_sources CONFIGURE_DEPENDS src/engine/*.cpp src/engine/*.h)
add_library(fugue_engine STATIC ${fugue_engine_sources})
target_include_directories(fugue_engine PUBLIC src)
target_link_libraries(fugue_engine PUBLIC Qt6::Core Qt6::Network PkgConfig::VIPS)
target_compile_options(fugue_engine PUBLIC -Wall -Wextra)

# Everything else but main.cpp, shared by the app and the tests, so each source
# compiles once and a new file in src/ needs no line here.
file(GLOB fugue_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(REMOVE_ITEM fugue_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(fugue_core STATIC ${fugue_sources})
target_include_directories(fugue_core PUBLIC src)
target_link_libraries(fugue_core PUBLIC fugue_engine
  Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::DBus)
target_compile_options(fugue_core PUBLIC -Wall -Wextra)

# The command line (spec 13): the same `fugue` binary, but it needs nothing
# beyond the engine, so it is its own library with no Gui, Quick or QML. main.cpp
# hands over to it before it creates a QGuiApplication.
file(GLOB fugue_cli_sources CONFIGURE_DEPENDS src/cli/*.cpp src/cli/*.h)
add_library(fugue_cli STATIC ${fugue_cli_sources})
target_include_directories(fugue_cli PUBLIC src)
target_link_libraries(fugue_cli PUBLIC fugue_engine)

add_executable(fugue src/main.cpp src/resources.qrc)
target_link_libraries(fugue PRIVATE fugue_core fugue_cli)

# The tests load the real Main.qml, so they carry the resources too.
enable_testing()
add_executable(fugue_tests tests/fugue_tests.cpp src/resources.qrc)
target_link_libraries(fugue_tests PRIVATE fugue_core Qt6::Test)
add_test(NAME fugue_tests COMMAND fugue_tests)
set_tests_properties(fugue_tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

# Engine tests: each tests/engine/<name>.cpp is its own QtTest executable, so
# parts can be tested (and fail) independently. They link only the engine.
file(GLOB fugue_engine_tests CONFIGURE_DEPENDS tests/engine/*.cpp)
foreach(test_source ${fugue_engine_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source})
  target_link_libraries(${test_name} PRIVATE fugue_engine Qt6::Test)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# CLI tests drive the built `fugue` binary as a subprocess with no display.
file(GLOB fugue_cli_tests CONFIGURE_DEPENDS tests/cli/*.cpp)
foreach(test_source ${fugue_cli_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source})
  target_link_libraries(${test_name} PRIVATE fugue_cli Qt6::Test)
  target_compile_definitions(${test_name} PRIVATE FUGUE_BINARY="$<TARGET_FILE:fugue>")
  add_dependencies(${test_name} fugue)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# `cmake --install build --prefix ~/.local` puts the app in the launcher
# before it is packaged; a package installs the same files under /usr.
include(GNUInstallDirs)
install(TARGETS fugue RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES share/fugue.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES share/fugue.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/fugue)
