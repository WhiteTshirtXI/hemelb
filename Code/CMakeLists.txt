cmake_minimum_required (VERSION 2.8)

project(HemeLB)

#---- OPTION switches accepted by the build -------

option(USE_HEMELB_DEBUGGER "Use built in hemelb debugger" OFF)
option(USE_MULTIMACHINE "Use multi-level parallelism support" OFF)
option(HEMELB_BUILD_UNITTESTS "Build the unit-tests" ON)
option(USE_ALL_WARNINGS_GNU "Show all compiler warnings on development builds (gnu-style-compilers)" ON)
set(HEMELB_LOG_LEVEL info
	CACHE STRING "Log level, choose 'debug', 'warning', or 'info'" )
set(HEMELB_STEERING_LIB basic
	CACHE STRING "Steering library, choose 'basic' or 'none'" )
set(DEPENDENCIES_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../dependencies"
	CACHE FILEPATH "Path to find dependency includes and libraries")
	
# Add warnings flags to development build types
if (USE_ALL_WARNINGS_GNU)
	list(APPEND CMAKE_CXX_FLAGS -Wall)
	list(APPEND CMAKE_CXX_FLAGS_DEBUG -Wall)
endif(USE_ALL_WARNINGS_GNU)
#------Dependencies --------------------------
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${DEPENDENCIES_PATH}/Modules/")
list(APPEND CMAKE_INCLUDE_PATH ${DEPENDENCIES_PATH}/include)
list(APPEND CMAKE_LIBRARY_PATH ${DEPENDENCIES_PATH}/lib)

# ------MPI------------------
# Require MPI for this project:
find_package(MPI REQUIRED)
set(CMAKE_CXX_COMPILE_FLAGS "${CMAKE_CXX_COMPILE_FLAGS} ${MPI_COMPILE_FLAGS}")
set(CMAKE_CXX_LINK_FLAGS "${CMAKE_CXX_LINK_FLAGS} ${CMAKE_CXX_LINK_FLAGS}")
include_directories(${MPI_INCLUDE_PATH})

#------Parmetis  ------------
find_package(Parmetis REQUIRED)
include_directories(${PARMETIS_INCLUDE_DIR})
MESSAGE(STATUS "Found Parmetis: ${PARMETIS_LIBRARY}")

#------TinyXML ----------------
find_package(TinyXML REQUIRED)
MESSAGE(STATUS "Found Tinyxml: ${TINYXML_LIBRARY}")
OPTION(TIXML_USE_STL "Use STL with TIXML" ON)
if(TIXML_USE_STL)
	add_definitions(-DTIXML_USE_STL)
endif(TIXML_USE_STL)
include_directories(${TINYXML_INCLUDE_DIR})

# ----------- HemeLB ------------------s

add_definitions(-DHEMELB_CFG_ON_BSD -DHEMELB_CFG_ON_OSX)
if (USE_HEMELB_DEBUGGER)
	# Force a debug build, because the debugger makes no sense without debug symbols
	set(CMAKE_BUILD_TYPE DEBUG)
endif(USE_HEMELB_DEBUGGER)

set(root_sources SimulationMaster.cc mpiInclude.cc D3Q15.cc)
add_executable(hemelb main.cc ${root_sources})
include_directories(${PROJECT_SOURCE_DIR})
set(package_subdirs
	configuration
	reporting
	steering
	vis
	lb
	net
	debug
	topology
	util
	geometry
	io
	log
	)
foreach(subdir ${package_subdirs})
	set(lib "hemelb_${subdir}")
	list(APPEND heme_libraries ${lib})
	add_subdirectory(${subdir})
endforeach(subdir)

target_link_libraries(hemelb 
	${heme_libraries}
	${MPI_LIBRARIES}
	${PARMETIS_LIBRARIES}
	${TINYXML_LIBRARIES}
	${CPPUNIT_LIBRARY}
	)

# ----------- HEMELB unittests ---------------
if(HEMELB_BUILD_UNITTESTS)
	#------CPPUnit ---------------
		find_package(CPPUnit REQUIRED)
		include_directories(${CPPUNIT_INCLUDE_DIR})
	add_executable(unittests_hemelb ${root_sources})
	add_subdirectory(unittests)
	target_link_libraries(unittests_hemelb 
		hemelb_unittests 
		${heme_libraries}
		${MPI_LIBRARIES}
		${PARMETIS_LIBRARIES}
		${TINYXML_LIBRARIES}
		${CPPUNIT_LIBRARY})
	INSTALL(TARGETS hemelb unittests_hemelb RUNTIME DESTINATION bin)

endif(HEMELB_BUILD_UNITTESTS)