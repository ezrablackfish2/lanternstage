cmake_minimum_required(VERSION 3.16)
project(LanternStage VERSION 1.0.0 LANGUAGES CXX)

option(LANTERNSTAGE_BUILD_TESTS "Build the LanternStage test suite" ON)
option(LANTERNSTAGE_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LANTERNSTAGE_SOURCES
  src/asset_store.cpp
  src/command.cpp
  src/compositor.cpp
  src/deployment_plan.cpp
  src/device_registry.cpp
  src/diagnostics.cpp
  src/engine.cpp
  src/expression_vm.cpp
  src/journal.cpp
  src/layout.cpp
  src/localization.cpp
  src/manifest.cpp
  src/playlist_graph.cpp
  src/query.cpp
  src/schedule_engine.cpp
  src/snapshot_codec.cpp
  src/text.cpp
  src/time_math.cpp
  src/wire.cpp
)

add_library(lanternstage STATIC ${LANTERNSTAGE_SOURCES})
target_include_directories(lanternstage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(lanternstage PRIVATE -Wall -Wextra -Wpedantic)

if(LANTERNSTAGE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(lanternstage PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lanternstage PUBLIC -fsanitize=address,undefined)
endif()

add_executable(lanternctl tools/lanternctl.cpp)
target_link_libraries(lanternctl PRIVATE lanternstage)
target_compile_options(lanternctl PRIVATE -Wall -Wextra -Wpedantic)

include(GNUInstallDirs)
install(TARGETS lanternstage lanternctl
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/lanternstage DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

if(LANTERNSTAGE_BUILD_TESTS)
  enable_testing()
  add_executable(lanternstage_tests tests/test_main.cpp)
  target_link_libraries(lanternstage_tests PRIVATE lanternstage)
  target_compile_options(lanternstage_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME lanternstage_tests COMMAND lanternstage_tests)
endif()
