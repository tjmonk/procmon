cmake_minimum_required(VERSION 3.10)

project(procmon-bin
	VERSION 0.1
    DESCRIPTION "Process Monitor"
)

include(GNUInstallDirs)

find_library ( LIB_RT rt REQUIRED )
find_library ( LIB_PTHREAD pthread REQUIRED )

add_executable( ${PROJECT_NAME}
	src/procmon.c
)

target_link_libraries( ${PROJECT_NAME}
    ${LIB_RT}
    ${LIB_PTHREAD}
    tjson
)

set_target_properties( ${PROJECT_NAME}
		       PROPERTIES OUTPUT_NAME procmon
)

target_include_directories( ${PROJECT_NAME} PRIVATE
	.
	inc
	${CMAKE_BINARY_DIR} )

install(TARGETS ${PROJECT_NAME}
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} )

