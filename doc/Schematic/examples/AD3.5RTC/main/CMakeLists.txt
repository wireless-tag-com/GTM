

# spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)


# SET(SOURCES screens/ui_Screen1.c
#     ui.c
#     ui_helpers.c)

# add_library(ui ${SOURCES})


#set(COMPONENT_SRCS "KeyHandle.c" "main_back.c" "main.c" "WifiConfig.c" "AudioPipeline.c" "VolcRTCDemo.c")
#set(COMPONENT_ADD_INCLUDEDIRS .)

file(GLOB_RECURSE SOURCES_C *.c)
file(GLOB_RECURSE SOURCES_CPP *.cpp)

idf_component_register(
    SRCS ${SOURCES_C} ${SOURCES_CPP} 
    INCLUDE_DIRS "." )

    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC -Wno-unused-variable -Wfatal-errors -Wno-incompatible-pointer-types -Wno-int-conversion -Wno-format -Wno-implicit-function-declaration -Wno-pointer-sign -Wno-double-promotion  -Wno-unused-value -Wno-switch -Wno-return-type -g -O3")  

    
# spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)

# 添加包含路径
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/volc_rtc_sdk/include)


