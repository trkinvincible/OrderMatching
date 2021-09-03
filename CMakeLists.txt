cmake_minimum_required(VERSION 3.10)
project(DaVinciDerivatives CXX)

set(CMAKE_CXX_STANDARD 17)
#set(CMAKE_CXX_FLAGS "-pg") - use only with GCC 's own profiler

if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set (CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "default install path" FORCE )
endif()

#boost C++
#find_package(Boost COMPONENTS program_options REQUIRED)

#flash logger
set(TESTS OFF CACHE INTERNAL "")
set(MICROSERVICE OFF CACHE INTERNAL "")

include(FetchContent)
find_library(FLASHLOGGER_LIB
  NAMES FlashLogger
)
if(NOT FLASHLOGGER_LIB)
    set(FETCHCONTENT_QUIET OFF)
    FetchContent_Declare(
      FlashLogger
      GIT_REPOSITORY https://github.com/trkinvincible/FlashLoggerForCpp.git
      GIT_TAG        v1.3
    )
    FetchContent_MakeAvailable(FlashLogger)
endif()

# googletest
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        release-1.11.0
)
FetchContent_MakeAvailable(googletest)

# Include Definitions
add_definitions(-DQT)

# Include Directories
include_directories(
    ${Boost_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIR}
    ${FlashLogger_INCLUDE_DIR}
)

# Link Directories
link_directories(
   ${CMAKE_INSTALL_PREFIX}/lib
)

# Source
set (_HEADER_
  ./hdr/command.h
  ./hdr/DaVinciDerivatives.h
)

set (_SOURCES_
  ./src/main.cpp
)

add_executable(${PROJECT_NAME} ${_SOURCES_} ${_HEADER_})

#set (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
#set (CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")

#find_package(FlashLogger CONFIG REQUIRED)
if(MICROSERVICE)
    set(LINK_LIBRARIES ${Boost_LIBRARIES} grpc++)
else()
    set(LINK_LIBRARIES protobuf ${Boost_LIBRARIES})
endif(MICROSERVICE)
if(MICROSERVICE)
    target_link_libraries(${PROJECT_NAME}
        -lpthread
        -ltcmalloc
        -latomic
        -Wl,--no-as-needed -lprofiler
        -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free
        ${LINK_LIBRARIES}
        gtest
        fl_grpc_proto
        ${_REFLECTION}
        ${_GRPC_GRPCPP}
        ${_PROTOBUF_LIBPROTOBUF})
else()
    target_link_libraries(${PROJECT_NAME}
        FlashLogger
        -lpthread
        -ltcmalloc
        -latomic
        -Wl,--no-as-needed -lprofiler
        -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free
        ${LINK_LIBRARIES})
endif(MICROSERVICE)
