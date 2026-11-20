cmake_minimum_required(VERSION 3.28...3.31)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(BUILD_TESTS "Build unit test suite" ON)

include(compilerconfig)
include(defaults)
include(helpers)

find_package(libobs REQUIRED)
if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Xml)

# Core ONVIF & device-provider static library (no libobs runtime dependency so tests link cleanly)
add_library(obs-network-device-core STATIC
  src/core/device-types.hpp
  src/core/device-provider.hpp
  src/core/device-registry.hpp
  src/core/device-registry.cpp
  src/onvif/onvif-auth.hpp
  src/onvif/onvif-auth.cpp
  src/onvif/ws-discovery.hpp
  src/onvif/ws-discovery.cpp
  src/onvif/onvif-client.hpp
  src/onvif/onvif-client.cpp
  src/onvif/onvif-provider.hpp
  src/onvif/onvif-provider.cpp
)

set_target_properties(obs-network-device-core PROPERTIES
  AUTOMOC ON
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)

target_include_directories(obs-network-device-core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(obs-network-device-core PUBLIC
  Qt6::Core
  Qt6::Gui
  Qt6::Network
  Qt6::Xml
)

if(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
  set_property(TARGET obs-network-device-core PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()

# OBS Studio Plugin Module
add_library(${CMAKE_PROJECT_NAME} MODULE
  src/plugin-main.cpp
  src/source/network-device-source.hpp
  src/source/network-device-source.cpp
  src/ui/device-explorer-dialog.hpp
  src/ui/device-explorer-dialog.cpp
  src/ui/ptz-dock.hpp
  src/ui/ptz-dock.cpp
)

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
  AUTOMOC ON
  AUTOUIC ON
  AUTORCC ON
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
  obs-network-device-core
  OBS::libobs
  $<$<BOOL:${ENABLE_FRONTEND_API}>:OBS::obs-frontend-api>
  Qt6::Widgets
)

target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
  $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(BUILD_TESTS)
  enable_testing()
  add_executable(obs-network-device-tests
    tests/test_onvif.cpp
  )
  set_target_properties(obs-network-device-tests PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
  )
  target_link_libraries(obs-network-device-tests PRIVATE
    obs-network-device-core
  )
  add_test(NAME OnvifCoreTests COMMAND obs-network-device-tests)

  if(OS_WINDOWS)
    # Locate Qt6 DLLs for test runner (from obs-deps-qt6)
    get_target_property(_qt6_core_loc Qt6::Core IMPORTED_LOCATION_RELEASE)
    if(NOT _qt6_core_loc)
      get_target_property(_qt6_core_loc Qt6::Core IMPORTED_LOCATION_RELWITHDEBINFO)
    endif()
    if(NOT _qt6_core_loc)
      get_target_property(_qt6_core_loc Qt6::Core LOCATION)
    endif()
    if(_qt6_core_loc)
      get_filename_component(_qt6_bin_dir "${_qt6_core_loc}" DIRECTORY)
      set_tests_properties(OnvifCoreTests PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qt6_bin_dir}"
      )
    endif()
  endif()
endif()
