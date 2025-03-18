file(GLOB SOURCES *.c)
set(priv_requires audio_sal audio_hal esp_peripherals  QM-Y1091-4832)

idf_component_register(
	SRCS ${SOURCES}
	INCLUDE_DIRS .
    REQUIRES ${requires} 
	PRIV_REQUIRES ${priv_requires}
)

IF (IDF_VERSION_MAJOR GREATER 3)
idf_component_get_property(audio_board_lib audio_board COMPONENT_LIB)
set_property(TARGET ${audio_board_lib} APPEND PROPERTY INTERFACE_LINK_LIBRARIES ${COMPONENT_LIB})

ELSEIF (IDF_VERSION_MAJOR EQUAL 3)
set_property(TARGET idf_component_audio_board APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES  $<TARGET_PROPERTY:${COMPONENT_TARGET},INTERFACE_INCLUDE_DIRECTORIES>)

ENDIF (IDF_VERSION_MAJOR GREATER 3)
