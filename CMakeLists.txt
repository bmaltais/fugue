cmake_minimum_required(VERSION 3.21)
project(fugue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# lld links a 12 MB test executable several times faster than GNU ld; any linker without it keeps the default.
include(CheckLinkerFlag)
check_linker_flag(CXX -fuse-ld=lld fugue_linker_has_lld)
if(fugue_linker_has_lld)
  add_link_options(-fuse-ld=lld)
endif()

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 DBus Test ShaderTools)
find_package(PkgConfig REQUIRED)
pkg_check_modules(VIPS REQUIRED IMPORTED_TARGET vips-cpp)
# Shared packages are zips (engine/share_package.h); pacman itself needs libarchive.
pkg_check_modules(ARCHIVE REQUIRED IMPORTED_TARGET libarchive)
find_package(Vulkan REQUIRED) # free GPU memory by device type (spec 8.10); no device is created
find_package(ZLIB REQUIRED) # the PSD reader inflates channel data into a buffer of exactly the promised size (engine/psd_file.cpp)

# Development builds (bin/build) pass -DFUGUE_SHARED_LIBS=ON: the four libraries below become shared objects, so
# an edit to one of their sources relinks one library instead of every program that holds a copy of it (#373).
# The default (packaging, CI without bin/build) stays static: one self-contained `fugue` binary.
option(FUGUE_SHARED_LIBS "Build the engine, core, cli and nodes libraries as shared objects" OFF)
if(FUGUE_SHARED_LIBS)
  set(fugue_lib_type SHARED)
  set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
else()
  set(fugue_lib_type STATIC)
endif()

# The engine: documents, compiler, scheduler, cache, providers. It runs with
# no display (the CLI and tests use it headless), so it never links Gui or
# Quick. A new file in src/engine/ needs no line here.
file(GLOB fugue_engine_sources CONFIGURE_DEPENDS src/engine/*.cpp src/engine/*.h)
add_library(fugue_engine ${fugue_lib_type} ${fugue_engine_sources})
target_include_directories(fugue_engine PUBLIC src)
target_link_libraries(fugue_engine PUBLIC Qt6::Core Qt6::Network PkgConfig::VIPS PRIVATE Vulkan::Vulkan PkgConfig::ARCHIVE ZLIB::ZLIB)
target_compile_options(fugue_engine PUBLIC -Wall -Wextra)
# vips.h pulls in vips7compat.h, whose `#define Rect VipsRect` breaks fugue::crop::Rect in any file that includes a vips
# header first. Fugue uses only the vips8 API.
target_compile_definitions(fugue_engine PUBLIC VIPS_DISABLE_COMPAT)
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
     src/workflow/*.cpp src/workflow/*.h src/apps/*.cpp src/apps/*.h src/run/*.cpp src/run/*.h src/settings/*.cpp src/settings/*.h
     src/panels/*.cpp src/panels/*.h src/media/*.cpp src/media/*.h src/workspace/*.cpp src/workspace/*.h)
list(REMOVE_ITEM fugue_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(fugue_core ${fugue_lib_type} ${fugue_sources})
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

# The window's own resources (src/resources.qrc: Main.qml, the icons, the strings). The app links them in as
# objects, so the one binary carries them. The tests that load the real Main.qml link the same file as a shared
# library instead, made once: with the .qrc added to each of ~120 test executables, one edit of Main.qml
# recompiled a 1.7 MB qrc_resources.cpp and relinked all of them, and now it rebuilds one small library. The
# library is kept as a dependency even though no symbol of it is used (its resource registration runs when it
# loads), hence --no-as-needed.
add_library(fugue_app_resources OBJECT src/resources.qrc)
target_link_libraries(fugue_app_resources PRIVATE Qt6::Core)
add_library(fugue_test_resources SHARED src/resources.qrc)
target_link_libraries(fugue_test_resources PRIVATE Qt6::Core)
# A test does not relink when the library is rebuilt: it only holds the resources, and nothing in the test
# depends on their bytes at link time.
set(CMAKE_LINK_DEPENDS_NO_SHARED ON)
set(fugue_test_resources_link -Wl,--push-state,--no-as-needed fugue_test_resources -Wl,--pop-state)

# The bundled canvas actions (#231, docs/canvas-actions-design.md): share/actions/*.fugue compiled in under
# :/fugue/actions, where CanvasActions lists them. They move to fugue-workflows (category `canvas`) when the
# template library lists actions (slice 2 of #231).
file(GLOB fugue_action_files CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR}/share/actions share/actions/*.fugue)
set(fugue_action_sources)
foreach(action ${fugue_action_files})
  set_source_files_properties(share/actions/${action} PROPERTIES QT_RESOURCE_ALIAS ${action})
  list(APPEND fugue_action_sources share/actions/${action})
endforeach()
qt_add_resources(fugue_core fugue_actions PREFIX /fugue/actions FILES ${fugue_action_sources})

# The look presets (docs/canvas-look.md 20.13): share/looks/*.json compiled in under :/fugue/looks, read by the same reader as look.json.
file(GLOB fugue_look_files CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR}/share/looks share/looks/*.json)
set(fugue_look_sources)
foreach(look ${fugue_look_files})
  set_source_files_properties(share/looks/${look} PROPERTIES QT_RESOURCE_ALIAS ${look})
  list(APPEND fugue_look_sources share/looks/${look})
endforeach()
qt_add_resources(fugue_core fugue_looks PREFIX /fugue/looks FILES ${fugue_look_sources})

# The `fal` provider's model table (spec 8.7): share/providers/fal-models.json compiled in under :/fugue/providers.
set_source_files_properties(share/providers/fal-models.json PROPERTIES QT_RESOURCE_ALIAS fal-models.json)
set_source_files_properties(share/providers/gemini-models.json PROPERTIES QT_RESOURCE_ALIAS gemini-models.json)
qt_add_resources(fugue_engine fugue_provider_tables PREFIX /fugue/providers FILES share/providers/fal-models.json share/providers/gemini-models.json)
# The `replicate` provider's model table (spec 8.7): share/providers/replicate-models.json, under :/fugue/providers too.
set_source_files_properties(share/providers/replicate-models.json PROPERTIES QT_RESOURCE_ALIAS replicate-models.json)
qt_add_resources(fugue_engine fugue_replicate_table PREFIX /fugue/providers FILES share/providers/replicate-models.json)
# The `cloudflare` provider's model table (spec 8.7), the same way.
set_source_files_properties(share/providers/cloudflare-models.json PROPERTIES QT_RESOURCE_ALIAS cloudflare-models.json)
qt_add_resources(fugue_engine fugue_provider_tables_cloudflare PREFIX /fugue/providers FILES share/providers/cloudflare-models.json)

# A folder of the window's QML: its .qml files compiled in under qrc:<prefix>, where the
# others import them by folder. A new .qml file there needs no line here. The same files are
# written as build/qmllint/<name>.qrc too, so the qmllint test reads them where the app does.
set(fugue_qmllint_resources --resource ${CMAKE_SOURCE_DIR}/src/resources.qrc)
# Every .qml and .js of the window goes into the Fugue QML module (fugue_core), so Qt compiles them ahead of time (qmlcachegen):
# a plain qt_add_resources keeps the source, and every window the app (or a test) opens then compiles ~120 files again
# (no disk cache for qrc files either), seconds per window. PREFIX / with the alias keeps each file where the app
# imports it from (qrc:/canvas/X.qml).
set(fugue_qml_sources)
function(fugue_add_qml name folder prefix)
  file(GLOB names CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR}/${folder} ${folder}/*.qml ${folder}/*.js)
  file(GLOB scripts CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR}/${folder} ${folder}/*.js)
  set(qrc "<RCC>\n  <qresource prefix=\"${prefix}\">\n")
  set(files)
  foreach(qml ${names})
    string(SUBSTRING ${prefix} 1 -1 alias_folder)
    set_source_files_properties(${folder}/${qml} PROPERTIES QT_RESOURCE_ALIAS ${alias_folder}/${qml})
    if(NOT qml IN_LIST scripts)
      list(APPEND files ${folder}/${qml})
    endif()
    string(APPEND qrc "    <file alias=\"${qml}\">${CMAKE_SOURCE_DIR}/${folder}/${qml}</file>\n")
  endforeach()
  string(APPEND qrc "  </qresource>\n</RCC>\n")
  set(fugue_qml_sources ${fugue_qml_sources} ${files} PARENT_SCOPE)
  set(js_files)
  foreach(script ${scripts})
    set_source_files_properties(${folder}/${script} PROPERTIES QT_RESOURCE_ALIAS ${script})
    list(APPEND js_files ${folder}/${script})
  endforeach()
  if(js_files)
    qt_add_resources(fugue_core ${name}_js PREFIX ${prefix} FILES ${js_files})
  endif()
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
# The App view's page (Fugue Apps, #385).
fugue_add_qml(fugue_apps_qml src/apps /apps)
# The side panels (#56): the dock container, the node library and the workflows panel.
fugue_add_qml(fugue_panels_qml src/panels /panels)
# The media browser (#133): a standalone viewer of a list of images, videos and sounds.
fugue_add_qml(fugue_media_qml src/media /media)
# The Drift kit (docs/canvas-look.md): grouped rows, switches, chips, sliders and the hero header, on the canvas look's tokens.
fugue_add_qml(fugue_kit_qml src/kit /kit)
# The window's areas and their views (#217): the area tree drawn (AreaNode.qml).
fugue_add_qml(fugue_workspace_qml src/workspace /workspace)
# The window itself and the command UI, at the top of the resource tree.
set_source_files_properties(src/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)
set_source_files_properties(src/commands/CommandUi.qml PROPERTIES QT_RESOURCE_ALIAS CommandUi.qml)
list(APPEND fugue_qml_sources src/Main.qml src/commands/CommandUi.qml)
file(WRITE ${CMAKE_BINARY_DIR}/qmllint/fugue_top_qml.qrc "<RCC>\n  <qresource prefix=\"/\">\n    <file alias=\"Main.qml\">${CMAKE_SOURCE_DIR}/src/Main.qml</file>\n    <file alias=\"CommandUi.qml\">${CMAKE_SOURCE_DIR}/src/commands/CommandUi.qml</file>\n  </qresource>\n</RCC>\n")
list(APPEND fugue_qmllint_resources --resource ${CMAKE_BINARY_DIR}/qmllint/fugue_top_qml.qrc)
set_source_files_properties(${fugue_qml_sources} PROPERTIES QT_QML_SKIP_QMLDIR_ENTRY TRUE) # not types of the Fugue module: they are found by folder
# The folders import each other by path, with no qmldir of their own, so QTP0004 stays OLD; Qt's warning that OLD
# is deprecated is switched off around the call (CMake 4.3's CMP0218 is set OLD so the switch is still read).
qt_policy(SET QTP0004 OLD)
cmake_policy(PUSH)
if(POLICY CMP0218) # CMake 4.3 and later; the AppImage job's pinned CMake does not know it (and has no switch to read)
  cmake_policy(SET CMP0218 OLD)
endif()
set(CMAKE_WARN_DEPRECATED OFF)
qt_target_qml_sources(fugue_core PREFIX / QML_FILES ${fugue_qml_sources})
cmake_policy(POP)

# The command line (spec 13): the same `fugue` binary, but it needs nothing
# beyond the engine, so it is its own library with no Gui, Quick or QML. main.cpp
# hands over to it before it creates a QGuiApplication.
file(GLOB fugue_cli_sources CONFIGURE_DEPENDS src/cli/*.cpp src/cli/*.h)
add_library(fugue_cli ${fugue_lib_type} ${fugue_cli_sources})
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
add_library(fugue_nodes ${fugue_lib_type} ${fugue_nodes_sources})
target_include_directories(fugue_nodes PUBLIC src)
target_link_libraries(fugue_nodes PUBLIC fugue_engine)
# The region picker (src/canvas/region_pick.cpp) calls the Canvas Region node's own geometry, so the box and
# size it shows are the node's, and every program with the window code links the nodes (#213).
target_link_libraries(fugue_core PUBLIC fugue_nodes)
file(GLOB fugue_node_manifests CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR}/src/nodes/manifests
     src/nodes/manifests/*/node.json)
set(fugue_node_manifest_files src/nodes/manifests/pack.json) # the fugue pack's pack.json lists the nodes
set_source_files_properties(src/nodes/manifests/pack.json PROPERTIES QT_RESOURCE_ALIAS pack.json)
foreach(manifest ${fugue_node_manifests})
  set_source_files_properties(src/nodes/manifests/${manifest} PROPERTIES QT_RESOURCE_ALIAS ${manifest})
  list(APPEND fugue_node_manifest_files src/nodes/manifests/${manifest})
endforeach()
# A group the pack ships (runtime.kind "group") keeps its definition beside its node.json.
file(GLOB fugue_group_definitions CONFIGURE_DEPENDS RELATIVE ${CMAKE_SOURCE_DIR}/src/nodes/manifests
     src/nodes/manifests/*/group.fugue)
foreach(definition ${fugue_group_definitions})
  set_source_files_properties(src/nodes/manifests/${definition} PROPERTIES QT_RESOURCE_ALIAS ${definition})
  list(APPEND fugue_node_manifest_files src/nodes/manifests/${definition})
endforeach()
qt_add_resources(fugue_nodes fugue_node_manifests PREFIX /fugue/nodes FILES ${fugue_node_manifest_files})
# The font Draw Text uses when none is given (src/nodes/text_raster.h): Noto Sans, Latin subset, SIL OFL 1.1 (share/fonts/LICENSE.notosans).
set_source_files_properties(share/fonts/notosans-latin.ttf PROPERTIES QT_RESOURCE_ALIAS notosans-latin.ttf)
qt_add_resources(fugue_nodes fugue_node_fonts PREFIX /fugue/fonts FILES share/fonts/notosans-latin.ttf)

add_executable(fugue src/main.cpp)
target_link_libraries(fugue PRIVATE fugue_core fugue_cli fugue_nodes fugue_app_resources)

# The tests that take longest are split into shards, each its own ctest entry (`test_x.1of4` ... `test_x.4of4`)
# running every n-th test function of the same executable (tests/test_shard.h, FUGUE_TEST_MAIN), so a run's
# wall time is not one long test: `name=count`. A shard count over the number of functions only makes empty shards.
set(fugue_test_shards
    test_canvas_view_window=8 test_canvas_run=6 test_cli_canvas=6 test_cli_canvas_act=2 test_menus=4 test_settings_panel=4
    test_side_panels_window=4 test_basic_cli=3 test_canvas_window=3
    test_chrome_window=3 test_canvas_look=2 test_canvas_widgets=2 test_canvas_editing=2 test_canvas_mask_editor=2 test_packs_page=2 test_omarchy_provider=2 test_projects_window=2 test_area_split_window=2 test_canvas_layers_window=2 test_app_window=3 test_http_provider=2)
function(fugue_add_test name)
  cmake_parse_arguments(ARG "OFFSCREEN" "" "" ${ARGN})
  set(count 1)
  foreach(entry ${fugue_test_shards})
    if(entry MATCHES "^${name}=([0-9]+)$")
      set(count ${CMAKE_MATCH_1})
    endif()
  endforeach()
  set(names ${name})
  if(count GREATER 1)
    set(names)
    foreach(i RANGE 1 ${count})
      add_test(NAME ${name}.${i}of${count} COMMAND ${name})
      set_property(TEST ${name}.${i}of${count} APPEND PROPERTY ENVIRONMENT FUGUE_TEST_SHARD=${i}/${count})
      list(APPEND names ${name}.${i}of${count})
    endforeach()
  else()
    add_test(NAME ${name} COMMAND ${name})
  endif()
  if(ARG_OFFSCREEN)
    foreach(each ${names})
      set_property(TEST ${each} APPEND PROPERTY ENVIRONMENT QT_QPA_PLATFORM=offscreen)
    endforeach()
  endif()
endfunction()

# The tests load the real Main.qml, so they link the resources too (fugue_test_resources).
enable_testing()
add_executable(fugue_tests tests/fugue_tests.cpp tests/test_environment.cpp)
target_link_libraries(fugue_tests PRIVATE fugue_core ${fugue_test_resources_link} Qt6::Test)
fugue_add_test(fugue_tests OFFSCREEN)

# Command tests: each tests/commands/<name>.cpp is its own QtTest executable. They load the
# real Main.qml, so they carry the resources too.
file(GLOB fugue_commands_tests CONFIGURE_DEPENDS tests/commands/*.cpp)
foreach(test_source ${fugue_commands_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp)
  target_link_libraries(${test_name} PRIVATE fugue_core ${fugue_test_resources_link} Qt6::Test)
  fugue_add_test(${test_name} OFFSCREEN)
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
  if(test_name STREQUAL "test_paint_file")
    target_link_libraries(${test_name} PRIVATE PkgConfig::ARCHIVE) # it reads the zip itself; the engine's own link is private
  endif()
  fugue_add_test(${test_name})
endforeach()

# Node tests: each tests/nodes/<name>.cpp is its own QtTest executable linking the built-in nodes.
file(GLOB fugue_nodes_tests CONFIGURE_DEPENDS tests/nodes/*.cpp)
foreach(test_source ${fugue_nodes_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp)
  target_link_libraries(${test_name} PRIVATE fugue_nodes Qt6::Test)
  target_compile_definitions(${test_name} PRIVATE FUGUE_BINARY="$<TARGET_FILE:fugue>"
                             FUGUE_FAKE_HELPER="$<TARGET_FILE:fake_fugue_helper>"
                             FUGUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_dependencies(${test_name} fugue fake_fugue_helper)
  fugue_add_test(${test_name})
endforeach()

# Canvas tests: each tests/canvas/<name>.cpp is its own QtTest executable with the app's
# code, the real Main.qml and the built-in node types.
file(GLOB fugue_canvas_tests CONFIGURE_DEPENDS tests/canvas/*.cpp)
foreach(test_source ${fugue_canvas_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp)
  target_link_libraries(${test_name} PRIVATE fugue_core ${fugue_test_resources_link} fugue_nodes Qt6::Test)
  target_compile_definitions(${test_name} PRIVATE FUGUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  fugue_add_test(${test_name} OFFSCREEN)
endforeach()

# Search tests: each tests/search/<name>.cpp is its own QtTest executable, like the canvas ones.
file(GLOB fugue_search_tests CONFIGURE_DEPENDS tests/search/*.cpp)
foreach(test_source ${fugue_search_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp)
  target_link_libraries(${test_name} PRIVATE fugue_core ${fugue_test_resources_link} fugue_nodes Qt6::Test)
  fugue_add_test(${test_name} OFFSCREEN)
endforeach()

# Run tests: each tests/run/<name>.cpp is its own QtTest executable with the app's code (the run
# queue, the run toolbar's state), like the canvas ones.
file(GLOB fugue_run_tests CONFIGURE_DEPENDS tests/run/*.cpp)
foreach(test_source ${fugue_run_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp)
  target_link_libraries(${test_name} PRIVATE fugue_core ${fugue_test_resources_link} fugue_nodes Qt6::Test)
  fugue_add_test(${test_name} OFFSCREEN)
endforeach()

# App view tests: each tests/apps/<name>.cpp is its own QtTest executable (the App view's store, Fugue Apps #385).
file(GLOB fugue_apps_tests CONFIGURE_DEPENDS tests/apps/*.cpp)
foreach(test_source ${fugue_apps_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp)
  target_link_libraries(${test_name} PRIVATE fugue_core ${fugue_test_resources_link} fugue_nodes Qt6::Test)
  target_compile_definitions(${test_name} PRIVATE FUGUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  fugue_add_test(${test_name} OFFSCREEN)
endforeach()

# Panel tests: each tests/panels/<name>.cpp is its own QtTest executable (the side panels' state
# and the panels in the real window), like the run ones.
file(GLOB fugue_panels_tests CONFIGURE_DEPENDS tests/panels/*.cpp)
foreach(test_source ${fugue_panels_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp)
  target_link_libraries(${test_name} PRIVATE fugue_core ${fugue_test_resources_link} fugue_nodes Qt6::Test)
  fugue_add_test(${test_name} OFFSCREEN)
endforeach()

# Workspace tests: the area tree and view registry alone, the area tree drawn, and the window's one area (#217).
file(GLOB fugue_workspace_tests CONFIGURE_DEPENDS tests/workspace/*.cpp)
foreach(test_source ${fugue_workspace_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp)
  target_link_libraries(${test_name} PRIVATE fugue_core ${fugue_test_resources_link} fugue_nodes Qt6::Test)
  target_compile_definitions(${test_name} PRIVATE FUGUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  fugue_add_test(${test_name} OFFSCREEN)
endforeach()

# Media tests: the browser'"'"'s core (results of a workflow, thumbnails) and the browser in the window.
file(GLOB fugue_media_tests CONFIGURE_DEPENDS tests/media/*.cpp)
foreach(test_source ${fugue_media_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp)
  target_link_libraries(${test_name} PRIVATE fugue_core ${fugue_test_resources_link} fugue_nodes Qt6::Test)
  fugue_add_test(${test_name} OFFSCREEN)
endforeach()

# Kit tests: the Drift kit's controls alone, in a window of their own (tests/kit/KitGallery.qml is also its picture).
file(GLOB fugue_kit_tests CONFIGURE_DEPENDS tests/kit/*.cpp)
foreach(test_source ${fugue_kit_tests})
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source} tests/test_environment.cpp)
  target_link_libraries(${test_name} PRIVATE fugue_core ${fugue_test_resources_link} Qt6::Test)
  target_compile_definitions(${test_name} PRIVATE FUGUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  fugue_add_test(${test_name} OFFSCREEN)
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
  add_executable(${test_name} ${test_source} tests/test_environment.cpp)
  target_link_libraries(${test_name} PRIVATE fugue_core ${fugue_test_resources_link} fugue_nodes Qt6::Test PkgConfig::ARCHIVE) # test_share_package makes zips
  target_compile_definitions(${test_name} PRIVATE FUGUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  fugue_add_test(${test_name} OFFSCREEN)
endforeach()

# qmllint over every QML file in src/, with the Fugue module's types and the resource layout
# the app loads them in: any warning fails it (tests/qmllint.cmake), so `bin/test` keeps the
# QML lint-clean.
file(GLOB_RECURSE fugue_qml_files CONFIGURE_DEPENDS src/*.qml)
# In chunks (qmllint.1of4 ...), one process each, run at the same time: qmllint is one thread, and a long file list
# slows it down more than in proportion.
set(fugue_qmllint_chunks 4)
foreach(chunk RANGE 1 ${fugue_qmllint_chunks})
  set(chunk_files)
  set(index 0)
  foreach(qml ${fugue_qml_files})
    math(EXPR slot "${index} % ${fugue_qmllint_chunks} + 1")
    if(slot EQUAL chunk)
      list(APPEND chunk_files ${qml})
    endif()
    math(EXPR index "${index} + 1")
  endforeach()
  add_test(NAME qmllint.${chunk}of${fugue_qmllint_chunks}
           COMMAND ${CMAKE_COMMAND} "-DQMLLINT=$<TARGET_FILE:Qt6::qmllint>"
           "-DARGS=-I;${CMAKE_BINARY_DIR}/qml;${fugue_qmllint_resources}" "-DFILES=${chunk_files}"
           -P ${CMAKE_SOURCE_DIR}/tests/qmllint.cmake)
endforeach()

# The PKGBUILD's helper sources are the commits and hashes helper/CMakeLists.txt pins (CI's Helper job runs it too).
add_test(NAME packaging_pins COMMAND bash ${CMAKE_SOURCE_DIR}/tests/pins.sh ${CMAKE_SOURCE_DIR})

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
                             FUGUE_FAKE_MODULE_PROVIDER="$<TARGET_FILE:fake_module_provider>"
                             FUGUE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_dependencies(${test_name} fugue fugue_with_fakes fake_module_provider)
  fugue_add_test(${test_name})
endforeach()

# No test reads or writes the owner's files: every test has a home of its own (HOME, so
# Omarchy's state under ~/.local/state/omarchy is the test's, OmarchyPaths::standard()) and
# its own XDG folders (settings, providers and keymap; the queue; caches; data; the runtime folder with the GPU lock), a test
# may still set its own. tests/engine/test_isolation.cpp fails when that is not so. No test
# reaches the owner's desktop either: the generic platform theme, so a FileDialog is Qt's own
# and never the portal's picker on the owner's screen, and no session bus (the GUI tests also
# set these themselves, tests/test_environment.cpp, for runs outside ctest).
get_property(fugue_all_tests DIRECTORY PROPERTY TESTS)
# A hang fails a test in minutes instead of idling for hours (#205). The slowest passing test takes
# about 10 s with the suite running in parallel, so 300 s leaves a slow CI runner room; the waits
# inside a test (a fake server, a spawned `fugue`) have deadlines of their own, well under this.
set(fugue_test_timeout_seconds 300)
foreach(test_name ${fugue_all_tests})
  set_tests_properties(${test_name} PROPERTIES TIMEOUT ${fugue_test_timeout_seconds})
  set(test_home ${CMAKE_BINARY_DIR}/test-home/${test_name})
  file(MAKE_DIRECTORY ${test_home})
  # The runtime folder too: the cross-root GPU lock lives in it, and a test's run must not wait behind
  # a real run on the machine. 0700, as a runtime folder must be.
  file(MAKE_DIRECTORY ${test_home}/run)
  file(CHMOD ${test_home}/run DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
  set_property(TEST ${test_name} APPEND PROPERTY ENVIRONMENT
               HOME=${test_home}
               XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config/${test_name}
               XDG_STATE_HOME=${test_home}/.local/state
               XDG_DATA_HOME=${test_home}/.local/share
               XDG_CACHE_HOME=${test_home}/.cache
               XDG_RUNTIME_DIR=${test_home}/run
               QT_QPA_PLATFORMTHEME=generic
               QT_NO_XDG_DESKTOP_PORTAL=1
               # Qt's incremental GC, in slices, can run in the middle of a window's creation: under load a Question then read
               # its scope and the theme as null ("QML Question: Cannot find member data", "Cannot read property 'unit' of null"),
               # 1 run in ~50 here (#279). One slice that finishes the whole collection (a pause is nothing offscreen) never does.
               QV4_GC_TIMELIMIT=100000
               DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent/fugue-tests-have-no-session-bus)
endforeach()

# `cmake --install build --prefix ~/.local` puts the app in the launcher
# before it is packaged; a package installs the same files under /usr.
include(GNUInstallDirs)
install(TARGETS fugue RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES share/fugue.desktop share/fugue-app.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES share/fugue-mime.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages RENAME fugue.xml)
# The icon (one art, installed as `fugue` and `fugue-dark`), scalable and in the sizes a launcher asks for.
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
