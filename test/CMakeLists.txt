cmake_minimum_required(VERSION 3.0.0)
set(TARGET_NAME "spice-test")
project(${TARGET_NAME})

include_directories(include)
set(SOURCES main.c)

add_compile_options(
  "-Wall"
  "-Werror"
  "-Wfatal-errors"
  "-ffast-math"
  "-fdata-sections"
  "-ffunction-sections"
  "$<$<CONFIG:DEBUG>:-O0;-g3;-ggdb>"
)

get_filename_component(PROJECT_TOP "${PROJECT_SOURCE_DIR}/.." ABSOLUTE)
add_subdirectory("${PROJECT_TOP}" "${CMAKE_BINARY_DIR}/spice")
add_subdirectory("ADL" "${CMAKE_BINARY_DIR}/ADL")

add_executable(${TARGET_NAME} ${SOURCES})
target_link_libraries(${TARGET_NAME}
	adl
	purespice
)
