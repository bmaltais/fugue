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
# Shared packages are zips (engine/share_package.h); pacman itself needs libarchive.
pkg_check_modules(ARCHIVE REQUIRED IMPORTED_TARGET libarchive)
find_package(Vulkan REQUIRED) # free GPU memory by device type (spec 8.10); no device is created

# The engine: documents, compiler, scheduler, cache, providers. It runs with
# no display (the CLI and tests use it headless), so it never links Gui or
# Quick. A new file in src/engine/ needs no line here.
file(GLOB fugue_engine_sources CONFIGURE_DEPENDS src/engine/*.cpp src/engine/*.h)
add_library(fugue_engine STATIC ${fugue_engine_sources})
target_include_directories(fugue_engine PUBLIC src)
target_link_libraries(fugue_engine PUBLIC Qt6::Core Qt6::Network PkgConfig::VIPS PRIVATE Vulkan::Vulkan PkgConfig::ARCHIVE)
target_compile_options(fugue_engine PUBLIC -Wall -Wextra)
# The model catalogs (spec 8.9, #134) are compiled in: src/catalog/*.json under :/fugue/catalog.
file(GLOB fugue_catalogs CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR}/src/catalog src/catalog/*.json)
set(fugue_catalog_files)
foreach(catalog ${fugue_catalogs})
  set_source_files_properties(src/catalog/${catalog} PROPERTIES QT_RESOURCE_ALIAS ${catalog})
  list(APPEND fugue_catalog_files src/catalog/${catalog})
endforeach()
qt_add_resources(fugue_engine fugue_catalogs PREFIX /fugue/catalog FILES ${fugue_catalog_files})
# The prompt templates (spec 14.2, #140) are compiled in the same way: src/prompts/*.json under :/fugue/prompts.
file(GLOB fugue_prompt_files CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR}/src/prompts src/prompts/*.json)
set(fugue_prompt_sources)
foreach(prompts ${fugue_prompt_files})
  set_source_files_properties(src/prompts/${prompts} PROPERTIES QT_RESOURCE_ALIAS ${prompts})
  list(APPEND fugue_prompt_sources src/prompts/${prompts})
endforeach()
qt_add_resources(fugue_engine fugue_prompts PREFIX /fugue/prompts FILES ${fugue_prompt_sources})

# Everything else but main.cpp, shared by the app and the tests, so each source
# compiles once and a new file in src/ needs no line here.
file(GLOB fugue_sources CONFIGURE_DEPENDS src/*.cpp src/*.h src/canvas/*.cpp src/canvas/*.h
     src/commands/*.cpp src/commands/*.h src/menu/*.cpp src/menu/*.h src/search/*.cpp src/search/*.h
     src/workflow/*.cpp src/workflow/*.h src/run/*.cpp src/run/*.h src/settings/*.cpp src/settings/*.h
     src/panels/*.cpp src/panels/*.h src/media/*.cpp src/media/*.h)
list(REMOVE_ITEM fugue_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(fugue_core STATIC ${fugue_sources})
target_include_directories(fugue_core PUBLIC src)
# The window's runs (src/run) start and queue them as the CLI does, with its code.
target_link_libraries(fugue_core PUBLIC fugue_engine fugue_cli
  Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::DBus)
target_compile_options(fugue_core PUBLIC -Wall -Wextra)
# Its C++ types are the QML module Fugue (qml_app.h): QML reads the window's objects as the
# typed App singleton, so qmllint can check them. The QML files stay plain resources below.
# The module's qmldir and qmltypes land in build/qml/Fugue, where the qmllint test looks.
qt_add_qml_module(fugue_core URI Fugue VERSION 1.0 NO_PLUGIN RESOURCE_PREFIX /qt/qml
  OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml/Fugue)

# A folder of the window's QML: its .qml files compiled in under qrc:<prefix>, where the
# others import them by folder. A new .qml file there needs no line here. The same files are
# written as build/qmllint/<name>.qrc too, so the qmllint test reads them where the app does.
set(fugue_qmllint_resources --resource ${CMAKE_SOURCE_DIR}/src/resources.qrc)
function(fugue_add_qml name folder prefix)
  file(GLOB names CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR}/${folder} ${folder}/*.qml ${folder}/*.js)
  set(files)
  set(qrc "<RCC>\n  <qresource prefix=\"${prefix}\">\n")
  foreach(qml ${names})
    set_source_files_properties(${folder}/${qml} PROPERTIES QT_RESOURCE_ALIAS ${qml})
    list(APPEND files ${folder}/${qml})
    string(APPEND qrc "    <file alias=\"${qml}\">${CMAKE_SOURCE_DIR}/${folder}/${qml}</file>\n")
  endforeach()
  string(APPEND qrc "  </qresource>\n</RCC>\n")
  qt_add_resources(fugue_core ${name} PREFIX ${prefix} FILES ${files})
  file(WRITE ${CMAKE_BINARY_DIR}/qmllint/${name}.qrc "${qrc}")
  set(fugue_qmllint_resources ${fugue_qmllint_resources}
      --resource ${CMAKE_BINARY_DIR}/qmllint/${name}.qrc PARENT_SCOPE)
endfunction()

# The canvas (spec 12), in src/canvas/, where Main.qml imports it.
fugue_add_qml(fugue_canvas_qml src/canvas /canvas)
# The widgets on a node's face (spec 6.2), in src/canvas/widgets/.
fugue_add_qml(fugue_widget_qml src/canvas/widgets /canvas/widgets)
# Its shaders (docs/canvas-look.md: the dot grid and rounded thumbnails), compiled to .qsb with
# Qt's shader tools, under qrc:/canvas/shaders/<name>.qsb.
file(GLOB fugue_canvas_shaders CONFIGURE_DEPENDS src/canvas/shaders/*.frag)
qt_add_shaders(fugue_core fugue_canvas_shaders PREFIX /canvas BASE src/canvas FILES ${fugue_canvas_shaders})
# Node search (spec 12.8).
fugue_add_qml(fugue_search_qml src/search /search)
# The menu bar and the context menus (#50).
fugue_add_qml(fugue_menu_qml src/menu /menu)
# The settings panel (#51).
fugue_add_qml(fugue_settings_qml src/settings /settings)
# The workflows (spec 11, 12.10): tabs, open, save, recovery and import.
fugue_add_qml(fugue_workflow_qml src/workflow /workflow)
# The run toolbar and the queue panel (#60).
fugue_add_qml(fugue_run_qml src/run /run)
# The side panels (#56): the dock container, the node library and the workflows panel.
fugue_add_qml(fugue_panels_qml src/panels /panels)
# The media browser (#133): a standalone viewer of a list of images, videos and sounds.
fugue_add_qml(fugue_media_qml src/media /media)

# The command line (spec 13): the same `fugue` binary, but it needs nothing
# beyond the engine, so it is its own library with no Gui, Quick or QML. main.cpp
# hands over to it before it creates a QGuiApplication.
file(GLOB fugue_cli_sources CONFIGURE_DEPENDS src/cli/*.cpp src/cli/*.h)
add_library(fugue_cli STATIC ${fugue_cli_sources})
target_include_directories(fugue_cli PUBLIC src)
target_link_libraries(fugue_cli PUBLIC fugue_engine)
# The agent skill (spec 13.5) is compiled in, so the one binary installs it wherever it is.
# qt_add_resources on a static library links its initializer into every executable using it.
set_source_files_properties(share/skills/fugue/SKILL.md PROPERTIES QT_RESOURCE_ALIAS fugue/SKILL.md)
qt_add_resources(fugue_cli fugue_skill PREFIX /fugue/skills FILES share/skills/fugue/SKILL.md)

# The built-in nodes (spec 14.1): kernels in src/nodes/*.cpp, and one node.json per node
# in src/nodes/manifests/<name>/, compiled in as Qt resources under :/fugue/nodes, which is the
# `fugue` pack (pack.json lists its nodes; a node.json it does not list is reported). A new
# .cpp needs no line here; a new node needs its folder added to pack.json. qt_add_resources on a static library makes an object
# library that is linked into every executable using it, so the resource initializer is
# never dropped by the linker (tests/cli checks the shipped binary knows fugue/text).
file(GLOB fugue_nodes_sources CONFIGURE_DEPENDS src/nodes/*.cpp src/nodes/*.h)
add_library(fugue_nodes STATIC ${fugue_nodes_sources})
target_include_directories(fugue_nodes PUBLIC src)
target_link_libraries(fugue_nodes PUBLIC fugue_engine)
file(GLOB fugue_node_manifests CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR}/src/nodes/manifests
     src/nodes/manifests/*/node.json)
set(fugue_node_manifest_files src/nodes/manifests/pack.json) # the fugue pack's pack.json lists the nodes
set_source_files_properties(src/nodes/manifests/pack.json PROPERTIES QT_RESOURCE_ALIAS pack.json)
foreach(manifest ${fugue_node_manifests})
  set_source_files_properties(src/nodes/manifests/${manifest} PROPERTIES QT_RESOURCE_ALIAS ${manifest})
  list(APPEND fugue_node_manifest_files src/nodes/manifests/${manifest})
endforeach()
qt_add_resources(fugue_nodes fugue_node_manifests PREFIX /fugue/nodes FILES ${fugue_node_manifest_files})

add_executable(fugue src/main.cpp src/resources.qrc)
target_link_libraries(fugue PRIVATE fugue_core fugue_cli fugue_nodes)

# The tests load the real Main.qml, so they carry the resources too.
enable_testing()
add_executable(fugue_tests tests/fugue_tests.cpp tests/test_environment.cpp src/resources.qrc)
target_link_libraries(fugue_tests PRIVATE fugue_core Qt6::Test)
add_test(NAME fugue_tests COMMAND fugue_tests)
set_tests_properties(fugue_tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

# Command tests: each tests/commands/<name>.cpp is its own QtTest executable. They load the
# real Main.qml, so they carry the resources too.
file(GLOB fugue_commands_tests CONFIGURE_DEPENDS tests/commands/*.cpp)
foreach(test_source ${fugue_commands_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp src/resources.qrc)
  target_link_libraries(${test_name} PRIVATE fugue_core Qt6::Test)
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
endforeach()

# A stand-in for fugue-helper that speaks its protocol without inference (test_helper_provider).
add_executable(fake_fugue_helper tests/helper_support/fake_fugue_helper.cpp)
target_link_libraries(fake_fugue_helper PRIVATE Qt6::Core)
target_compile_options(fake_fugue_helper PRIVATE -Wall -Wextra)

# A tiny third-party provider speaking the provider protocol over stdio or http, for the tests of
# provider modules (test_provider_module).
add_executable(fake_module_provider tests/provider_module_support/fake_module_provider.cpp)
target_link_libraries(fake_module_provider PRIVATE Qt6::Core Qt6::Network)
target_compile_options(fake_module_provider PRIVATE -Wall -Wextra)

# Engine tests: each tests/engine/<name>.cpp is its own QtTest executable, so
# parts can be tested (and fail) independently. They link only the engine.
file(GLOB fugue_engine_tests CONFIGURE_DEPENDS tests/engine/*.cpp)
foreach(test_source ${fugue_engine_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp)
  target_link_libraries(${test_name} PRIVATE fugue_engine Qt6::Test)
  target_compile_definitions(${test_name} PRIVATE FUGUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
                             FUGUE_FAKE_HELPER="$<TARGET_FILE:fake_fugue_helper>"
                             FUGUE_FAKE_MODULE_PROVIDER="$<TARGET_FILE:fake_module_provider>")
  add_dependencies(${test_name} fake_fugue_helper fake_module_provider)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Node tests: each tests/nodes/<name>.cpp is its own QtTest executable linking the built-in nodes.
file(GLOB fugue_nodes_tests CONFIGURE_DEPENDS tests/nodes/*.cpp)
foreach(test_source ${fugue_nodes_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp)
  target_link_libraries(${test_name} PRIVATE fugue_nodes Qt6::Test)
  target_compile_definitions(${test_name} PRIVATE FUGUE_BINARY="$<TARGET_FILE:fugue>"
                             FUGUE_FAKE_HELPER="$<TARGET_FILE:fake_fugue_helper>")
  add_dependencies(${test_name} fugue fake_fugue_helper)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Canvas tests: each tests/canvas/<name>.cpp is its own QtTest executable with the app's
# code, the real Main.qml and the built-in node types.
file(GLOB fugue_canvas_tests CONFIGURE_DEPENDS tests/canvas/*.cpp)
foreach(test_source ${fugue_canvas_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp src/resources.qrc)
  target_link_libraries(${test_name} PRIVATE fugue_core fugue_nodes Qt6::Test)
  target_compile_definitions(${test_name} PRIVATE FUGUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
endforeach()

# Search tests: each tests/search/<name>.cpp is its own QtTest executable, like the canvas ones.
file(GLOB fugue_search_tests CONFIGURE_DEPENDS tests/search/*.cpp)
foreach(test_source ${fugue_search_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp src/resources.qrc)
  target_link_libraries(${test_name} PRIVATE fugue_core fugue_nodes Qt6::Test)
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
endforeach()

# Run tests: each tests/run/<name>.cpp is its own QtTest executable with the app's code (the run
# queue, the run toolbar's state), like the canvas ones.
file(GLOB fugue_run_tests CONFIGURE_DEPENDS tests/run/*.cpp)
foreach(test_source ${fugue_run_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp src/resources.qrc)
  target_link_libraries(${test_name} PRIVATE fugue_core fugue_nodes Qt6::Test)
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
endforeach()

# Panel tests: each tests/panels/<name>.cpp is its own QtTest executable (the side panels' state
# and the panels in the real window), like the run ones.
file(GLOB fugue_panels_tests CONFIGURE_DEPENDS tests/panels/*.cpp)
foreach(test_source ${fugue_panels_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp src/resources.qrc)
  target_link_libraries(${test_name} PRIVATE fugue_core fugue_nodes Qt6::Test)
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
endforeach()

# Media tests: the browser'"'"'s core (results of a workflow, thumbnails) and the browser in the window.
file(GLOB fugue_media_tests CONFIGURE_DEPENDS tests/media/*.cpp)
foreach(test_source ${fugue_media_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp src/resources.qrc)
  target_link_libraries(${test_name} PRIVATE fugue_core fugue_nodes Qt6::Test)
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
endforeach()

# A `fugue` run from the build folder finds the bundled templates beside it, as an installed one
# finds share/fugue/templates (engine/templates.h).
file(CREATE_LINK ${CMAKE_SOURCE_DIR}/templates ${CMAKE_BINARY_DIR}/templates SYMBOLIC)

# Workflow tests: each tests/workflow/<name>.cpp is its own QtTest executable with the app's
# code, the real Main.qml and the built-in node types. FUGUE_SOURCE_DIR finds examples/ and the
# bundled templates/.
file(GLOB fugue_workflow_tests CONFIGURE_DEPENDS tests/workflow/*.cpp)
foreach(test_source ${fugue_workflow_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp src/resources.qrc)
  target_link_libraries(${test_name} PRIVATE fugue_core fugue_nodes Qt6::Test PkgConfig::ARCHIVE) # test_share_package makes zips
  target_compile_definitions(${test_name} PRIVATE FUGUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
endforeach()

# qmllint over every QML file in src/, with the Fugue module's types and the resource layout
# the app loads them in: any warning fails it (tests/qmllint.cmake), so `bin/test` keeps the
# QML lint-clean.
file(GLOB_RECURSE fugue_qml_files CONFIGURE_DEPENDS src/*.qml)
add_test(NAME qmllint COMMAND ${CMAKE_COMMAND} "-DQMLLINT=$<TARGET_FILE:Qt6::qmllint>"
         "-DARGS=-I;${CMAKE_BINARY_DIR}/qml;${fugue_qmllint_resources}" "-DFILES=${fugue_qml_files}"
         -P ${CMAKE_SOURCE_DIR}/tests/qmllint.cmake)

# A test-only `fugue` with fake node kernels (tests/cli_support): the real CLI, queue and
# worker, but nodes that do fake work. Not installed.
add_executable(fugue_with_fakes tests/cli_support/fugue_with_fakes.cpp)
target_link_libraries(fugue_with_fakes PRIVATE fugue_cli)

# CLI tests drive the built `fugue` binary as a subprocess with no display.
file(GLOB fugue_cli_tests CONFIGURE_DEPENDS tests/cli/*.cpp)
foreach(test_source ${fugue_cli_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp)
  target_link_libraries(${test_name} PRIVATE fugue_cli Qt6::Test)
  target_compile_definitions(${test_name} PRIVATE FUGUE_BINARY="$<TARGET_FILE:fugue>"
                             FUGUE_FAKE_BINARY="$<TARGET_FILE:fugue_with_fakes>"
                             FUGUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_dependencies(${test_name} fugue fugue_with_fakes)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# No test reads or writes the owner's files: every test has a home of its own (HOME, so
# Omarchy's state under ~/.local/state/omarchy is the test's, OmarchyPaths::standard()) and
# its own XDG folders (settings, providers and keymap; the queue; caches; data), a test
# may still set its own. tests/engine/test_isolation.cpp fails when that is not so. No test
# reaches the owner's desktop either: the generic platform theme, so a FileDialog is Qt's own
# and never the portal's picker on the owner's screen, and no session bus (the GUI tests also
# set these themselves, tests/test_environment.cpp, for runs outside ctest).
get_property(fugue_all_tests DIRECTORY PROPERTY TESTS)
foreach(test_name ${fugue_all_tests})
  set(test_home ${CMAKE_BINARY_DIR}/test-home/${test_name})
  file(MAKE_DIRECTORY ${test_home})
  set_property(TEST ${test_name} APPEND PROPERTY ENVIRONMENT
               HOME=${test_home}
               XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config/${test_name}
               XDG_STATE_HOME=${test_home}/.local/state
               XDG_DATA_HOME=${test_home}/.local/share
               XDG_CACHE_HOME=${test_home}/.cache
               QT_QPA_PLATFORMTHEME=generic
               QT_NO_XDG_DESKTOP_PORTAL=1
               DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent/fugue-tests-have-no-session-bus)
endforeach()

# `cmake --install build --prefix ~/.local` puts the app in the launcher
# before it is packaged; a package installs the same files under /usr.
include(GNUInstallDirs)
install(TARGETS fugue RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES share/fugue.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES share/fugue-mime.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages RENAME fugue.xml)
# The icon, light-art as `fugue` and dark-art as `fugue-dark`, in the sizes a launcher asks for.
install(DIRECTORY share/icons/hicolor DESTINATION ${CMAKE_INSTALL_DATADIR}/icons)
install(FILES examples/demo.fugue DESTINATION ${CMAKE_INSTALL_DATADIR}/fugue/examples)
# The bundled templates (spec 11.10): the `templates` submodule, bmaltais/fugue-workflows at the
# commit this release pins. Its folders and categories.json; not its README, check script or CI.
install(DIRECTORY templates/ DESTINATION ${CMAKE_INSTALL_DATADIR}/fugue/templates
        PATTERN ".*" EXCLUDE PATTERN "bin" EXCLUDE PATTERN "README.md" EXCLUDE)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/fugue)

# Tests that count timer, socket or file-watch events with margins of tens of milliseconds. `bin/test` runs the
# suite in parallel, one test per core, and on a busy 4-core runner those margins vanish (a download's requests,
# Hyprland calls, a reload burst). They run alone; the rest stay parallel.
foreach(serial_test fugue_tests test_model_download test_registry_reloader)
  if(TEST ${serial_test})
    set_tests_properties(${serial_test} PROPERTIES RUN_SERIAL TRUE)
  endif()
endforeach()
