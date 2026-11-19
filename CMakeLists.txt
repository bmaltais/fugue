cmake_minimum_required(VERSION 3.21)
project(fugue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 DBus Test ShaderTools)
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
file(GLOB fugue_sources CONFIGURE_DEPENDS src/*.cpp src/*.h src/canvas/*.cpp src/canvas/*.h
     src/commands/*.cpp src/commands/*.h src/menu/*.cpp src/menu/*.h src/search/*.cpp src/search/*.h
     src/workflow/*.cpp src/workflow/*.h src/run/*.cpp src/run/*.h src/settings/*.cpp src/settings/*.h)
list(REMOVE_ITEM fugue_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(fugue_core STATIC ${fugue_sources})
target_include_directories(fugue_core PUBLIC src)
# The window's runs (src/run) start and queue them as the CLI does, with its code.
target_link_libraries(fugue_core PUBLIC fugue_engine fugue_cli
  Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::DBus)
target_compile_options(fugue_core PUBLIC -Wall -Wextra)

# The canvas (spec 12): its QML files in src/canvas/, compiled in under qrc:/canvas, where
# Main.qml imports them. A new .qml file there needs no line here.
file(GLOB fugue_canvas_qml CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR}/src/canvas src/canvas/*.qml)
set(fugue_canvas_qml_files)
foreach(qml ${fugue_canvas_qml})
  set_source_files_properties(src/canvas/${qml} PROPERTIES QT_RESOURCE_ALIAS ${qml})
  list(APPEND fugue_canvas_qml_files src/canvas/${qml})
endforeach()
qt_add_resources(fugue_core fugue_canvas_qml PREFIX /canvas FILES ${fugue_canvas_qml_files})
# The widgets on a node's face (spec 6.2), in src/canvas/widgets/, under qrc:/canvas/widgets.
file(GLOB fugue_widget_qml CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR}/src/canvas/widgets src/canvas/widgets/*.qml)
set(fugue_widget_qml_files)
foreach(qml ${fugue_widget_qml})
  set_source_files_properties(src/canvas/widgets/${qml} PROPERTIES QT_RESOURCE_ALIAS ${qml})
  list(APPEND fugue_widget_qml_files src/canvas/widgets/${qml})
endforeach()
qt_add_resources(fugue_core fugue_widget_qml PREFIX /canvas/widgets FILES ${fugue_widget_qml_files})
# Its shaders (docs/canvas-look.md: the dot grid and rounded thumbnails), compiled to .qsb with
# Qt's shader tools, under qrc:/canvas/shaders/<name>.qsb.
file(GLOB fugue_canvas_shaders CONFIGURE_DEPENDS src/canvas/shaders/*.frag)
qt_add_shaders(fugue_core fugue_canvas_shaders PREFIX /canvas BASE src/canvas FILES ${fugue_canvas_shaders})

# Node search (spec 12.8): its QML in src/search/, compiled in under qrc:/search.
file(GLOB fugue_search_qml CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR}/src/search src/search/*.qml)
set(fugue_search_qml_files)
foreach(qml ${fugue_search_qml})
  set_source_files_properties(src/search/${qml} PROPERTIES QT_RESOURCE_ALIAS ${qml})
  list(APPEND fugue_search_qml_files src/search/${qml})
endforeach()
qt_add_resources(fugue_core fugue_search_qml PREFIX /search FILES ${fugue_search_qml_files})

# The menu bar and the context menus (#50): their QML in src/menu/, under qrc:/menu.
file(GLOB fugue_menu_qml CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR}/src/menu src/menu/*.qml src/menu/*.js)
set(fugue_menu_qml_files)
foreach(qml ${fugue_menu_qml})
  set_source_files_properties(src/menu/${qml} PROPERTIES QT_RESOURCE_ALIAS ${qml})
  list(APPEND fugue_menu_qml_files src/menu/${qml})
endforeach()
qt_add_resources(fugue_core fugue_menu_qml PREFIX /menu FILES ${fugue_menu_qml_files})

# The settings panel (#51): its QML in src/settings/, under qrc:/settings.
file(GLOB fugue_settings_qml CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR}/src/settings src/settings/*.qml)
set(fugue_settings_qml_files)
foreach(qml ${fugue_settings_qml})
  set_source_files_properties(src/settings/${qml} PROPERTIES QT_RESOURCE_ALIAS ${qml})
  list(APPEND fugue_settings_qml_files src/settings/${qml})
endforeach()
qt_add_resources(fugue_core fugue_settings_qml PREFIX /settings FILES ${fugue_settings_qml_files})

# The workflows (spec 11, 12.10): tabs, open, save, recovery and import, with their QML in
# src/workflow/, compiled in under qrc:/workflow. A new .qml file there needs no line here.
file(GLOB fugue_workflow_qml CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR}/src/workflow src/workflow/*.qml)
set(fugue_workflow_qml_files)
foreach(qml ${fugue_workflow_qml})
  set_source_files_properties(src/workflow/${qml} PROPERTIES QT_RESOURCE_ALIAS ${qml})
  list(APPEND fugue_workflow_qml_files src/workflow/${qml})
endforeach()
qt_add_resources(fugue_core fugue_workflow_qml PREFIX /workflow FILES ${fugue_workflow_qml_files})

# The command line (spec 13): the same `fugue` binary, but it needs nothing
# beyond the engine, so it is its own library with no Gui, Quick or QML. main.cpp
# hands over to it before it creates a QGuiApplication.
file(GLOB fugue_cli_sources CONFIGURE_DEPENDS src/cli/*.cpp src/cli/*.h)
add_library(fugue_cli STATIC ${fugue_cli_sources})
target_include_directories(fugue_cli PUBLIC src)
target_link_libraries(fugue_cli PUBLIC fugue_engine)

# The built-in nodes (spec 14.1): kernels in src/nodes/*.cpp, and one node.json per node
# in src/nodes/manifests/<name>/, compiled in as Qt resources under :/fugue/nodes. A new
# .cpp or manifest needs no line here. qt_add_resources on a static library makes an object
# library that is linked into every executable using it, so the resource initializer is
# never dropped by the linker (tests/cli checks the shipped binary knows fugue/text).
file(GLOB fugue_nodes_sources CONFIGURE_DEPENDS src/nodes/*.cpp src/nodes/*.h)
add_library(fugue_nodes STATIC ${fugue_nodes_sources})
target_include_directories(fugue_nodes PUBLIC src)
target_link_libraries(fugue_nodes PUBLIC fugue_engine)
file(GLOB fugue_node_manifests CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR}/src/nodes/manifests
     src/nodes/manifests/*/node.json)
set(fugue_node_manifest_files)
foreach(manifest ${fugue_node_manifests})
  set_source_files_properties(src/nodes/manifests/${manifest} PROPERTIES QT_RESOURCE_ALIAS ${manifest})
  list(APPEND fugue_node_manifest_files src/nodes/manifests/${manifest})
endforeach()
qt_add_resources(fugue_nodes fugue_node_manifests PREFIX /fugue/nodes FILES ${fugue_node_manifest_files})

add_executable(fugue src/main.cpp src/resources.qrc)
target_link_libraries(fugue PRIVATE fugue_core fugue_cli fugue_nodes)

# The tests load the real Main.qml, so they carry the resources too.
enable_testing()
add_executable(fugue_tests tests/fugue_tests.cpp src/resources.qrc)
target_link_libraries(fugue_tests PRIVATE fugue_core Qt6::Test)
add_test(NAME fugue_tests COMMAND fugue_tests)
set_tests_properties(fugue_tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

# Command tests: each tests/commands/<name>.cpp is its own QtTest executable. They load the
# real Main.qml, so they carry the resources too.
file(GLOB fugue_commands_tests CONFIGURE_DEPENDS tests/commands/*.cpp)
foreach(test_source ${fugue_commands_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} src/resources.qrc)
  target_link_libraries(${test_name} PRIVATE fugue_core Qt6::Test)
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
endforeach()

# Engine tests: each tests/engine/<name>.cpp is its own QtTest executable, so
# parts can be tested (and fail) independently. They link only the engine.
file(GLOB fugue_engine_tests CONFIGURE_DEPENDS tests/engine/*.cpp)
foreach(test_source ${fugue_engine_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source})
  target_link_libraries(${test_name} PRIVATE fugue_engine Qt6::Test)
  target_compile_definitions(${test_name} PRIVATE FUGUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Node tests: each tests/nodes/<name>.cpp is its own QtTest executable linking the built-in nodes.
file(GLOB fugue_nodes_tests CONFIGURE_DEPENDS tests/nodes/*.cpp)
foreach(test_source ${fugue_nodes_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source})
  target_link_libraries(${test_name} PRIVATE fugue_nodes Qt6::Test)
  target_compile_definitions(${test_name} PRIVATE FUGUE_BINARY="$<TARGET_FILE:fugue>")
  add_dependencies(${test_name} fugue)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Canvas tests: each tests/canvas/<name>.cpp is its own QtTest executable with the app's
# code, the real Main.qml and the built-in node types.
file(GLOB fugue_canvas_tests CONFIGURE_DEPENDS tests/canvas/*.cpp)
foreach(test_source ${fugue_canvas_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} src/resources.qrc)
  target_link_libraries(${test_name} PRIVATE fugue_core fugue_nodes Qt6::Test)
  target_compile_definitions(${test_name} PRIVATE FUGUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
endforeach()

# Search tests: each tests/search/<name>.cpp is its own QtTest executable, like the canvas ones.
file(GLOB fugue_search_tests CONFIGURE_DEPENDS tests/search/*.cpp)
foreach(test_source ${fugue_search_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} src/resources.qrc)
  target_link_libraries(${test_name} PRIVATE fugue_core fugue_nodes Qt6::Test)
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
endforeach()

# Workflow tests: each tests/workflow/<name>.cpp is its own QtTest executable with the app's
# code, the real Main.qml and the built-in node types. FUGUE_SOURCE_DIR finds examples/.
file(GLOB fugue_workflow_tests CONFIGURE_DEPENDS tests/workflow/*.cpp)
foreach(test_source ${fugue_workflow_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} src/resources.qrc)
  target_link_libraries(${test_name} PRIVATE fugue_core fugue_nodes Qt6::Test)
  target_compile_definitions(${test_name} PRIVATE FUGUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
endforeach()

# A test-only `fugue` with fake node kernels (tests/cli_support): the real CLI, queue and
# worker, but nodes that do fake work. Not installed.
add_executable(fugue_with_fakes tests/cli_support/fugue_with_fakes.cpp)
target_link_libraries(fugue_with_fakes PRIVATE fugue_cli)

# CLI tests drive the built `fugue` binary as a subprocess with no display.
file(GLOB fugue_cli_tests CONFIGURE_DEPENDS tests/cli/*.cpp)
foreach(test_source ${fugue_cli_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source})
  target_link_libraries(${test_name} PRIVATE fugue_cli Qt6::Test)
  target_compile_definitions(${test_name} PRIVATE FUGUE_BINARY="$<TARGET_FILE:fugue>"
                             FUGUE_FAKE_BINARY="$<TARGET_FILE:fugue_with_fakes>"
                             FUGUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_dependencies(${test_name} fugue fugue_with_fakes)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# No test reads or writes the owner's settings, providers or keymap: every test starts with
# XDG_CONFIG_HOME on an empty folder of its own (a test may still set its own).
get_property(fugue_all_tests DIRECTORY PROPERTY TESTS)
foreach(test_name ${fugue_all_tests})
  set_property(TEST ${test_name} APPEND PROPERTY ENVIRONMENT
               XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config/${test_name})
endforeach()

# `cmake --install build --prefix ~/.local` puts the app in the launcher
# before it is packaged; a package installs the same files under /usr.
include(GNUInstallDirs)
install(TARGETS fugue RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES share/fugue.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES share/fugue.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES examples/demo.fugue examples/text-to-image.fugue DESTINATION ${CMAKE_INSTALL_DATADIR}/fugue/examples)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/fugue)
