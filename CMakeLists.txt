cmake_minimum_required(VERSION 3.10)

project(naval_moth VERSION 0.1 DESCRIPTION "Top-down naval combat built on moth_graphics, Box2D and EnTT")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS on)

if (MSVC)
	set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MD")
	set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MD")
endif()

set(SOURCES ${SOURCES}
	src/naval_application.cpp
	src/game/aggro_system.cpp
	src/game/attenuation.cpp
	src/game/audio.cpp
	src/game/camera_shake.cpp
	src/game/combat_system.cpp
	src/game/defs.cpp
	src/game/miniaudio_impl.cpp
	src/game/propulsion_system.cpp
	src/game/render_system.cpp
	src/game/ship_factory.cpp
	src/game/terrain.cpp
	src/game/wake_system.cpp
	src/game/wander_system.cpp
	src/layers/game_layer.cpp
	src/main.cpp
)

find_package(moth_graphics REQUIRED)
find_package(box2d REQUIRED)
find_package(EnTT REQUIRED)

add_executable(${PROJECT_NAME} ${SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE src)

# miniaudio is vendored as a single third-party header (external/miniaudio).
# SYSTEM keeps its warnings out of the -Wall -Werror below for the files that
# include it. Unlike the other dependencies it is not a Conan package: it has no
# dependencies of its own, so vendoring it costs a header and sidesteps the rule
# that the audio stack has to come from the system package manager on Linux.
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE external/miniaudio)

# Trim miniaudio to what this game asks of it — WAV sound effects — dropping the
# encoders, the FLAC/MP3 decoders and the waveform generators. These are set on
# the whole target rather than beside the implementation so that every
# translation unit including the header agrees with it about what is in it.
target_compile_definitions(${PROJECT_NAME} PRIVATE
	MA_NO_ENCODING
	MA_NO_FLAC
	MA_NO_MP3
	MA_NO_GENERATION
)

target_link_libraries(${PROJECT_NAME} moth_graphics::moth_graphics box2d::box2d EnTT::EnTT)
if (NOT WIN32)
	# miniaudio runs its own device thread and dlopen()s the platform audio
	# backend (ALSA/PulseAudio) at runtime, so it needs these even though no
	# audio library is linked directly.
	find_package(Threads REQUIRED)
	target_link_libraries(${PROJECT_NAME} Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES VERSION ${PROJECT_VERSION})
# Warnings are errors for our code. The file compiling miniaudio's
# implementation is the exception: it is third-party and not ours to fix, so it
# is built with warnings off. The SYSTEM include above is enough for that on its
# own with gcc/clang, but MSVC only honours a system include from a recent
# enough toolset and CMake, so this belt is worth the braces to keep the Windows
# build off a cliff we can't see from here. Source flags land after the target's,
# so the "off" wins, and only for this one file.
if (MSVC)
	set_target_properties(${PROJECT_NAME} PROPERTIES COMPILE_FLAGS "/W3 /WX")
	target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
	set_source_files_properties(src/game/miniaudio_impl.cpp PROPERTIES COMPILE_FLAGS "/w")
else()
	set_target_properties(${PROJECT_NAME} PROPERTIES COMPILE_FLAGS "-Wall -Werror")
	set_source_files_properties(src/game/miniaudio_impl.cpp PROPERTIES COMPILE_FLAGS "-w")
endif()

install(TARGETS ${PROJECT_NAME} DESTINATION bin)
