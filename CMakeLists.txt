cmake_minimum_required(VERSION 3.0)
project(purespice LANGUAGES C)
set(CMAKE_C_STANDARD 11)

find_package(PkgConfig)
pkg_check_modules(SPICE_PKGCONFIG REQUIRED
	spice-protocol
	nettle
	hogweed
)

add_definitions(-D USE_NETTLE)

add_compile_options(
  "-Wall"
  "-Wextra"
  "-Werror"
  "-Wfatal-errors"
  "-Wstrict-prototypes"
)
add_library(purespice STATIC
	src/ps.c
	src/log.c
	src/rsa.c
	src/queue.c
	src/channel.c
	src/channel_main.c
	src/channel_inputs.c
	src/channel_playback.c
	src/channel_record.c
	src/channel_display.c
	src/agent.c
)

target_link_libraries(purespice
	${SPICE_PKGCONFIG_LIBRARIES}
	gmp
)

target_include_directories(purespice
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
	PRIVATE
		src
		${SPICE_PKGCONFIG_INCLUDE_DIRS}
)
