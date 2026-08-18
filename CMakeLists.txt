cmake_minimum_required(VERSION 3.20)
project(pulse LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The project deliberately has no third-party dependency. Everything below is either
# the C++ standard library or a platform socket library that ships with the system.
add_library(pulse_core STATIC
  src/net.cpp
  src/http.cpp
  src/websocket.cpp
  src/metrics.cpp
  src/codec.cpp
  src/server.cpp
)
target_include_directories(pulse_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(WIN32)
  target_compile_definitions(pulse_core PUBLIC _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(pulse_core PUBLIC ws2_32 psapi)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(pulse_core PUBLIC Threads::Threads)
endif()

if(MSVC)
  set(PULSE_WARNINGS /W4)
else()
  set(PULSE_WARNINGS -Wall -Wextra)
endif()
target_compile_options(pulse_core PRIVATE ${PULSE_WARNINGS})

add_executable(pulse-server src/main.cpp)
target_link_libraries(pulse-server PRIVATE pulse_core)

add_executable(pulse-demo src/demo.cpp)
target_link_libraries(pulse-demo PRIVATE pulse_core)

enable_testing()
add_executable(pulse-tests tests/tests.cpp)
target_link_libraries(pulse-tests PRIVATE pulse_core)
add_test(NAME unit COMMAND pulse-tests)

foreach(target pulse-server pulse-demo pulse-tests)
  target_compile_options(${target} PRIVATE ${PULSE_WARNINGS})
endforeach()

# The web assets are read from disk at run time, so the binaries need to know where
# they are when started from an arbitrary working directory.
target_compile_definitions(pulse-server PRIVATE PULSE_WEB_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/web")
target_compile_definitions(pulse-demo   PRIVATE PULSE_WEB_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/web")

# One command that starts the system and prints the address to open.
add_custom_target(demo
  COMMAND pulse-demo
  DEPENDS pulse-demo
  USES_TERMINAL
  COMMENT "Running the Pulse demo"
)
