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
	src/game/combat_system.cpp
	src/game/defs.cpp
	src/game/propulsion_system.cpp
	src/game/render_system.cpp
	src/game/ship_factory.cpp
	src/game/terrain.cpp
	src/layers/game_layer.cpp
	src/main.cpp
)

find_package(moth_graphics REQUIRED)
find_package(box2d REQUIRED)
find_package(EnTT REQUIRED)

add_executable(${PROJECT_NAME} ${SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_link_libraries(${PROJECT_NAME} moth_graphics::moth_graphics box2d::box2d EnTT::EnTT)
set_target_properties(${PROJECT_NAME} PROPERTIES VERSION ${PROJECT_VERSION})
if (MSVC)
	set_target_properties(${PROJECT_NAME} PROPERTIES COMPILE_FLAGS "/W3 /WX")
	target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
	set_target_properties(${PROJECT_NAME} PROPERTIES COMPILE_FLAGS "-Wall -Werror")
endif()

install(TARGETS ${PROJECT_NAME} DESTINATION bin)
