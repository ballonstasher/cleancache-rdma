cmake_minimum_required(VERSION 3.10)

project("DCC Daemon")
## APP OUTPUT
set(DCC_DAEMON dcc_daemon.out)

option(DEBUG "build debug mode." OFF)
option(STATS_ENABLE "enable record stats" OFF)

find_package(gflags)

if (DEBUG)
	message("[O] Debug mode")
	set(CMAKE_BUILD_TYPE Debug)
else()
	message("[X] Release mode")
	set(CMAKE_BUILD_TYPE Release)
endif(DEBUG)

if (STATS_ENABLE)
	string(APPEND CMAKE_CXX_FLAGS " -DDCC_STATS_ENABLE")
	message("[O] -DDCC_STATS_ENABLE")
else()
	message("[X] Stats is disabled")
endif(STATS_ENABLE)

if (TIME_CHECK)
	string(APPEND CMAKE_CXX_FLAGS " -DDCC_TIME_CHECK")
	message("[O] -DDCC_TIME_CHECK")
else()
	message("[X] timer is disabled")
endif(TIME_CHECK)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

LINK_LIBRARIES(pthread rdmacm ibverbs atomic jemalloc ${gflags_LIBRARIES})

add_executable(${DCC_DAEMON} 
	main.cpp 
	rdma.cpp rdma_mem.cpp 
	kv.cpp 
	percpu_pool.cpp util/lfstack.cpp
	hash-table/linear_probing_approx.cpp
	cpu-stats/CPUData.cpp cpu-stats/CPUSnapshot.cpp
	stats.cpp breakdown.cpp
	logger.cpp
	util/crc8.cpp)

target_compile_definitions(${DCC_DAEMON} PUBLIC)
