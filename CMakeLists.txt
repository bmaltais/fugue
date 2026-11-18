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

# Everything but main.cpp, shared by the app and the tests, so each source
# compiles once and a new file in src/ needs no line here.
file(GLOB fugue_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(REMOVE_ITEM fugue_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(fugue_core STATIC ${fugue_sources})
target_include_directories(fugue_core PUBLIC src)
target_link_libraries(fugue_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::DBus)
target_compile_options(fugue_core PUBLIC -Wall -Wextra)

add_executable(fugue src/main.cpp src/resources.qrc)
target_link_libraries(fugue PRIVATE fugue_core)

# The tests load the real Main.qml, so they carry the resources too.
enable_testing()
add_executable(fugue_tests tests/fugue_tests.cpp src/resources.qrc)
target_link_libraries(fugue_tests PRIVATE fugue_core Qt6::Test)
add_test(NAME fugue_tests COMMAND fugue_tests)
set_tests_properties(fugue_tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

# `cmake --install build --prefix ~/.local` puts the app in the launcher
# before it is packaged; a package installs the same files under /usr.
include(GNUInstallDirs)
install(TARGETS fugue RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES share/fugue.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES share/fugue.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/fugue)
