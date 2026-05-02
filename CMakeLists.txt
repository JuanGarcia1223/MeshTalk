cmake_minimum_required(VERSION 3.24)
project(meshtalk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

add_executable(meshtalk_demo
  src/main.cpp
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
