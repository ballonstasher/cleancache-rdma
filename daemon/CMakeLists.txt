cmake_minimum_required(VERSION 3.10)

project("DCC Daemon")
## APP OUTPUT
set(DCC_DAEMON dcc_daemon.out)

option(DEBUG "build debug mode." OFF)

find_package(gflags)

if (DEBUG)
	message("[O] Debug mode")
	set(CMAKE_BUILD_TYPE Debug)
	#message("[X] RelDebug mode")
	#set(CMAKE_BUILD_TYPE RelDebug)
else()
	message("[X] Release mode")
	set(CMAKE_BUILD_TYPE Release)
endif(DEBUG)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

LINK_LIBRARIES(pthread rdmacm ibverbs atomic jemalloc ${gflags_LIBRARIES})

add_executable(${DCC_DAEMON} 
	main.cpp 
	rdma.cpp rdma_mem.cpp
	)

target_compile_definitions(${DCC_DAEMON} PUBLIC)
