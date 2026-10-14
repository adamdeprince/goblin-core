cmake_minimum_required(VERSION 3.25)
project(goblin-core-cpp LANGUAGES CXX)
include(GNUInstallDirs)
add_library(goblin_core_client INTERFACE)
add_library(goblin::client ALIAS goblin_core_client)
target_compile_features(goblin_core_client INTERFACE cxx_std_20)
target_include_directories(goblin_core_client INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../sbe/generated>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(goblin_core_client PROPERTIES EXPORT_NAME client)
install(TARGETS goblin_core_client EXPORT goblinClientTargets)
# The client and its transitive public headers are self contained.
install(DIRECTORY ../include/goblin DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY ../sbe/generated/goblin_sbe DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT goblinClientTargets NAMESPACE goblin::
  FILE goblin-core-cppConfig.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/goblin-core-cpp)
install(FILES ../LICENSE ../NOTICE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/goblin-core-cpp)
