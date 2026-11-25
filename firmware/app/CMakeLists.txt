cmake_minimum_required(VERSION 3.20.0)

# Glag for the SP-1: Wakes' board, power, watchdog, standby, battery, controls, LEDs and audio
# bring-up (src/sp1_*, from Worldwave/Wakes, MIT), with the Glag engine and controls (../glag)
# as the audio source. See ../README.md.
#
# No ccache, as in Wakes (it once mixed object layouts across header overrides).
set(USE_CCACHE 0 CACHE STRING "Never ccache" FORCE)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(glag-sp1)

target_sources(app PRIVATE
	src/main.c
	src/sp1_led.c
	src/sp1_power.c
	src/sp1_batt.c
	src/sp1_standby.c
	src/sp1_console.c
	src/sp1_logbuf.c
	src/sp1_logring.c
	src/sp1_threads.c
	src/sp1_usbd.c
	src/sp1_controls.c
	src/sp1_display.c
	src/sp1_calib.c
	src/sp1_audio.c
	src/sp1_meter.c
	src/sp1_playrow.c
)
target_include_directories(app PRIVATE src)

if(CONFIG_SP1_GLAG)
	# The engine runs in the audio thread: its own library, so it gets -O2 while the rest
	# of the firmware stays -Os. -ffp-contract=off: on the Cortex-M4 a fused multiply-add
	# takes 3 cycles and a separate multiply and add 2 (measured in ../host/cpu).
	set(GLAG ${CMAKE_CURRENT_SOURCE_DIR}/../glag)
	zephyr_library_named(glag)
	zephyr_library_sources(${GLAG}/glag_engine.c ${GLAG}/glag_ui.c)
	zephyr_library_include_directories(${GLAG})
	zephyr_library_compile_options(-O2 -ffp-contract=off -Wdouble-promotion)
	if(CONFIG_SP1_GLAG_NOIR_TEST)
		# the noir album as the CPU test: the audition pieces, Haze on the echo's tape, no script
		zephyr_library_compile_definitions(GL_PROF GL_DEMO_PIECES GL_NO_HAZE_RAM GL_NOIR_TEST GL_TEST_HOOKS)
		target_compile_definitions(app PRIVATE GL_PROF GL_DEMO_PIECES GL_NO_HAZE_RAM GL_NOIR_TEST GL_TEST_HOOKS)
	elseif(CONFIG_SP1_GLAG_CPU_TEST)
		# its parts timed and its output checked; the four heaviest pieces as 17-20; the
		# controls' test hooks for the scenario script (glag_test.h, run from main.c)
		zephyr_library_compile_definitions(GL_PROF GL_TEST_PIECES GL_TEST_HOOKS)
		target_compile_definitions(app PRIVATE GL_PROF GL_TEST_PIECES GL_TEST_HOOKS)   # (main and glag_ui agree)
	endif()
	if(DEFINED GLAG_DEFS)   # audition builds: -DGLAG_DEFS=PULSE_SOFT (engine options, glag_engine.c)
		zephyr_library_compile_definitions(${GLAG_DEFS})
	endif()
	target_link_libraries(app PRIVATE glag)
	target_include_directories(app PRIVATE ${GLAG})
endif()
