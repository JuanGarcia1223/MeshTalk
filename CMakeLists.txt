cmake_minimum_required(VERSION 3.24)
project(meshtalk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Protobuf REQUIRED)

include(FetchContent)

# Pull notcurses directly from upstream.
FetchContent_Declare(
  notcurses
  GIT_REPOSITORY https://github.com/dankamongmen/notcurses.git
  GIT_TAG master
  GIT_SHALLOW TRUE
)

# Keep the notcurses build lean for initial integration.
set(USE_CXX ON CACHE BOOL "Build C++ wrappers" FORCE)
set(USE_PANDOC OFF CACHE BOOL "Disable docs" FORCE)
set(USE_DOXYGEN OFF CACHE BOOL "Disable docs" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Disable tests" FORCE)

FetchContent_MakeAvailable(notcurses)

# Pull SQLiteCpp and build it as a subproject.
FetchContent_Declare(
  SQLiteCpp
  GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git
  GIT_TAG master
  GIT_SHALLOW TRUE
)
set(SQLITECPP_BUILD_TESTS OFF CACHE BOOL "Disable SQLiteCpp tests" FORCE)
set(SQLITECPP_BUILD_EXAMPLES OFF CACHE BOOL "Disable SQLiteCpp examples" FORCE)
FetchContent_MakeAvailable(SQLiteCpp)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS
  proto/discovery.proto
  proto/chat.proto
)

add_executable(meshtalk_demo
  src/main.cpp
  src/db/SqliteTest.cpp
  src/chat/ChatServer.cpp
  src/discovery/UdpHelloBroadcaster.cpp
  src/ui/TerminalUI.cpp
  ${PROTO_SRCS}
)

target_include_directories(meshtalk_demo PRIVATE
  src
  ${CMAKE_CURRENT_BINARY_DIR}
  ${Protobuf_INCLUDE_DIRS}
)

# Notcurses target names can vary; support common exported names.
if(TARGET notcurses++)
  target_link_libraries(meshtalk_demo PRIVATE notcurses++)
elseif(TARGET notcurses)
  target_link_libraries(meshtalk_demo PRIVATE notcurses)
elseif(TARGET notcurses-core)
  target_link_libraries(meshtalk_demo PRIVATE notcurses-core)
else()
  message(FATAL_ERROR "No expected notcurses target found (notcurses++, notcurses, notcurses-core)")
endif()

target_link_libraries(meshtalk_demo PRIVATE
  ${Protobuf_LIBRARIES}
  SQLiteCpp
)

add_custom_target(run
    COMMAND ${CMAKE_SOURCE_DIR}/run.sh
    DEPENDS meshtalk_demo
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
