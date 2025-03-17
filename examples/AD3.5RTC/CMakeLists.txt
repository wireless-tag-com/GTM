# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

add_definitions(-DLV_MEM_SIZE=52000)
add_definitions(-DCONFIG_LV_MEM_POOL_ALLOC=malloc)

# if(EXISTS ${CMAKE_SOURCE_DIR}/QMSD-ESP32-BSP)
#     set(ENV{QMSD_8MS_PATH} ${CMAKE_SOURCE_DIR}/QMSD-ESP32-BSP)
# elseif(NOT DEFINED ENV{QMSD_8MS_PATH})
set(ENV{QMSD_8MS_PATH} ./qmsd-esp32-bsp)
# endif()

if(NOT DEFINED ENV{ADF_PATH})
    set(ENV{ADF_PATH} "~/esp/esp-adf")
endif()

include($ENV{ADF_PATH}/CMakeLists.txt)
include($ENV{QMSD_8MS_PATH}/component.cmake)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(get-start)


