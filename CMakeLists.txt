cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum client macOS version")
endif()
if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Client SDK architectures")
endif()

project(OpenPCI VERSION 1.6.0 LANGUAGES C CXX)
if(NOT APPLE)
  message(FATAL_ERROR "The OpenPCI client SDK currently requires macOS and IOKit")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED YES)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
include(GNUInstallDirs)
include(CTest)
include(CMakePackageConfigHelpers)
find_library(EXTGPU_IOKIT IOKit REQUIRED)
find_library(EXTGPU_COREFOUNDATION CoreFoundation REQUIRED)

# Every source in sdk/ belongs to the client library; executable entry points
# live in tools/ or tests/. CONFIGURE_DEPENDS keeps this initial source tree
# usable while separate Darwin transport translation units are added.
file(GLOB EXTGPU_SDK_SOURCES CONFIGURE_DEPENDS "sdk/*.c" "sdk/*.cpp")
if(NOT EXTGPU_SDK_SOURCES)
  message(FATAL_ERROR "SDK implementation sources are missing; no empty library will be built")
endif()

add_library(extgpu_objects OBJECT ${EXTGPU_SDK_SOURCES})
target_include_directories(extgpu_objects PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_options(extgpu_objects PRIVATE -Wall -Wextra -Werror -fblocks)
set_target_properties(extgpu_objects PROPERTIES POSITION_INDEPENDENT_CODE YES
  C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)

add_library(extgpu STATIC $<TARGET_OBJECTS:extgpu_objects>)
add_library(extgpu_shared SHARED $<TARGET_OBJECTS:extgpu_objects>)
set_target_properties(extgpu_shared PROPERTIES OUTPUT_NAME extgpu
  VERSION ${PROJECT_VERSION} SOVERSION 1 MACOSX_RPATH YES)
foreach(target extgpu extgpu_shared)
  target_include_directories(${target} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(${target} PUBLIC "-framework IOKit" "-framework CoreFoundation")
endforeach()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/extgpu-info.c")
  add_executable(extgpu-info tools/extgpu-info.c)
  target_link_libraries(extgpu-info PRIVATE extgpu)
  target_compile_options(extgpu-info PRIVATE -Wall -Wextra -Werror)
  install(TARGETS extgpu-info RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
  add_subdirectory(tests)
endif()

install(TARGETS extgpu extgpu_shared EXPORT extGPUTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/extgpu/extgpu.h include/extgpu/module.modulemap
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/extgpu)
install(FILES LICENSES/SDK-MIT.txt sdk/README.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/ext-GPU-SDK)
install(EXPORT extGPUTargets NAMESPACE extGPU:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/extGPU)
configure_file(sdk/extgpu.pc.in "${CMAKE_CURRENT_BINARY_DIR}/extgpu.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/extgpu.pc"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
configure_package_config_file(cmake/extGPUConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/extGPUConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/extGPU)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/extGPUConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/extGPUConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/extGPUConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/extGPU)

# The DriverKit extension is intentionally a separate Xcode target. Building
# this client SDK never signs, installs, activates or opens a hardware device.
