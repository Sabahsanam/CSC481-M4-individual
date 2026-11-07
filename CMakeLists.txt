cmake_minimum_required(VERSION 3.10)
project(CSC481M4Individual)

set(CMAKE_CXX_STANDARD 17)

find_package(SDL3 REQUIRED)
find_package(SDL3_image REQUIRED)

# ZeroMQ (team engine networking)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED libzmq)
find_path(CPPZMQ_INCLUDE_DIR zmq.hpp)
if(NOT CPPZMQ_INCLUDE_DIR)
    message(FATAL_ERROR "cppzmq not found (macOS: brew install cppzmq)")
endif()

add_executable(
    game
    src/Game.cpp
    src/Entity.cpp
    src/Physics.cpp
    src/Input.cpp
    src/Collision.cpp
    src/Scaling.cpp
    src/Timeline.cpp
    src/Networking.cpp
)

target_include_directories(game PRIVATE ${CPPZMQ_INCLUDE_DIR} ${ZMQ_INCLUDE_DIRS})
target_link_directories(game PRIVATE ${ZMQ_LIBRARY_DIRS})
target_link_libraries(game PRIVATE SDL3::SDL3 SDL3_image::SDL3_image ${ZMQ_LIBRARIES})

# Headless tests of the game rules (no window or network needed):
#   ./logic_test   level rules from Milestone 3, now running through events
#   ./bot          an automatic player that must reach the portal
#   ./race_test    the event-driven two-player race (Milestone 4)
enable_testing()
foreach(t logic_test bot race_test)
    add_executable(${t} tests/${t}.cpp src/Physics.cpp src/Collision.cpp src/Entity.cpp src/Scaling.cpp)
    target_include_directories(${t} PRIVATE src)
    target_link_libraries(${t} PRIVATE SDL3::SDL3)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# "make check" builds and runs all three tests
add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
                  DEPENDS logic_test bot race_test)
