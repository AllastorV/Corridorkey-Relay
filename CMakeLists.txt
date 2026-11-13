cmake_minimum_required(VERSION 3.24)
project(corridorkey2_ofx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)

include(FetchContent)
FetchContent_Declare(openfx
  URL https://github.com/AcademySoftwareFoundation/openfx/archive/refs/tags/OFX_Release_1.5.1.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR no-build)
FetchContent_MakeAvailable(openfx)

set(BUNDLE_DIR ${CMAKE_BINARY_DIR}/CorridorKey2.ofx.bundle)

add_library(corridorkey2_ofx SHARED plugin/ck2_ofx.cpp)
target_include_directories(corridorkey2_ofx PRIVATE ${openfx_SOURCE_DIR}/include)
target_compile_definitions(corridorkey2_ofx PRIVATE UNICODE _UNICODE)
set_target_properties(corridorkey2_ofx PROPERTIES
  OUTPUT_NAME CorridorKey2 SUFFIX .ofx
  RUNTIME_OUTPUT_DIRECTORY ${BUNDLE_DIR}/Contents/Win64
  LIBRARY_OUTPUT_DIRECTORY ${BUNDLE_DIR}/Contents/Win64)
configure_file(server/ck2_job.py ${BUNDLE_DIR}/Contents/Resources/ck2_job.py COPYONLY)
if(MSVC)
  target_link_options(corridorkey2_ofx PRIVATE /PDBALTPATH:CorridorKey2.pdb)
endif()

add_executable(test_plugin tests/test_plugin.cpp)
target_include_directories(test_plugin PRIVATE ${openfx_SOURCE_DIR}/include)
target_compile_definitions(test_plugin PRIVATE UNICODE _UNICODE)
