cmake_minimum_required(VERSION 3.10)

project(mqttvars
	VERSION 0.1
    DESCRIPTION "MQTT Variables"
)

include(GNUInstallDirs)

find_library ( LIB_RT rt REQUIRED )
find_library ( LIB_VARSERVER varserver REQUIRED )
find_library ( LIB_PTHREAD pthread REQUIRED )
find_library ( LIB_TJSON tjson REQUIRED )
find_library ( LIB_MQTTASYNC paho-mqtt3a REQUIRED )

add_executable( ${PROJECT_NAME}
	src/mqtt_vars.c
)

target_link_libraries( ${PROJECT_NAME}
    ${LIB_RT}
    ${LIB_VARSERVER}
    ${LIB_PTHREAD}
    ${LIB_TJSON}
    ${LIB_MQTTASYNC}
)

set_target_properties( ${PROJECT_NAME}
    PROPERTIES OUTPUT_NAME mqttvars
)

target_include_directories( ${PROJECT_NAME} PRIVATE
	.
	inc
	${CMAKE_BINARY_DIR} )

install(TARGETS ${PROJECT_NAME}
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} )

