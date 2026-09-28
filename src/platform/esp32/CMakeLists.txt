# SVS Bridge for the ESP32-S3 (ESP-IDF 6.1; N16R8): the common code in src/core plus this target
# (main/). Same layout as Cruller (margaale/Cruller src/platform/esp32).
#   idf.py -C src/platform/esp32 -B build/esp32 build   (scripts/build.sh esp32 does it)
cmake_minimum_required(VERSION 3.22)

get_filename_component(SVS_BRIDGE_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)
include(${SVS_BRIDGE_ROOT}/src/version.cmake)
set(SVS_BRIDGE_VERSION "${SVS_BRIDGE_VERSION_DEFAULT}" CACHE STRING "Firmware version (X.Y.Z)")
set(PROJECT_VER ${SVS_BRIDGE_VERSION})
set(IDF_TARGET esp32s3)
# The generated sdkconfig goes to the build directory; sdkconfig.defaults here is the source.
set(SDKCONFIG ${CMAKE_BINARY_DIR}/sdkconfig)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# USB host, mDNS and ArduinoJson come in through main/idf_component.yml
project(svs_bridge)
