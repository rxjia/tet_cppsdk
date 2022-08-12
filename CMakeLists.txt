##############################################################################
#
# C++ SDK - Cross-platform SDK for The Eye Tribe Tracker
#
##############################################################################

CMAKE_MINIMUM_REQUIRED(VERSION 2.8)

#----------------------------------------------------------------
#
# We don't want to mix relative and absolute paths in linker lib lists.
#
if(COMMAND cmake_policy)
  cmake_policy(SET CMP0003 NEW)
endif(COMMAND cmake_policy)

PROJECT(TET_CPPSDK)

#set(CMAKE_C_FLAGS "-fPIC")
#-----------------------------------------------------------------------------
#
# We only want debug and release configurations
#
SET(CMAKE_CONFIGURATION_TYPES  "Debug" "Release"  CACHE INTERNAL  "Allowed Configuration types" FORCE)

SET(TET_CPPSDK_INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/include)
SET(TET_CPPSDK_LIBS "")

SET( Boost_USE_MULTITHREAD ON )
#SET( Boost_USE_STATIC_LIBS ON )
SET( BOOST_MIN_VERSION "1.53.0")
FIND_PACKAGE( Boost REQUIRED COMPONENTS thread system chrono)

IF(Boost_FOUND)
#  SET(TET_CPPSDK_INCLUDE_DIRS ${TET_CPPSDK_INCLUDE_DIRS} "${Boost_INCLUDE_DIR}")
  SET(TET_CPPSDK_LIBS ${TET_CPPSDK_LIBS} "${Boost_LIBRARIES}")
ELSE()
  message (FATAL_ERROR "Could not find Boost libraries!")
ENDIF(Boost_FOUND)

#-----------------------------------------------------------------------------
#
# We only want debug and release configurations
#
FILE( GLOB TET_CPPSDK_FILES  ${CMAKE_CURRENT_LIST_DIR}/include/*.h
                             ${CMAKE_CURRENT_LIST_DIR}/src/*.hpp
                             ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp )

INCLUDE_DIRECTORIES( ${TET_CPPSDK_INCLUDE_DIRS} )

SET(TET_CPPSDK_LIBRARY_OUTPUT "${TET_CPPSDK_SOURCE_DIR}/lib")
IF( NOT EXISTS ${TET_CPPSDK_LIBRARY_OUTPUT} )
  MAKE_DIRECTORY( "${TET_CPPSDK_LIBRARY_OUTPUT}" )
ENDIF( NOT EXISTS ${TET_CPPSDK_LIBRARY_OUTPUT} )

SET( LIB_NAME "GazeApiLib" )
ADD_LIBRARY(${LIB_NAME} SHARED ${TET_CPPSDK_FILES} )
SET(TET_CPPSDK_LIBS ${TET_CPPSDK_LIBS} ${LIB_NAME})

IF( WIN32 )
  SET_TARGET_PROPERTIES( ${LIB_NAME} PROPERTIES DEBUG_POSTFIX "D" )
ENDIF( WIN32 )

#MESSAGE(boost---- ${Boost_LIBRARIES})
IF(Boost_FOUND)
  TARGET_LINK_DIRECTORIES(${LIB_NAME} PRIVATE ${Boost_INCLUDE_DIR})
ENDIF(Boost_FOUND)

TARGET_LINK_LIBRARIES(${LIB_NAME} PRIVATE ${Boost_LIBRARIES})
 
#-----------------------------------------------------------------------------
#
# Copy static lib into correct folder using a post-build event
#
ADD_CUSTOM_COMMAND(
  TARGET ${LIB_NAME}
  POST_BUILD
  COMMAND ${CMAKE_COMMAND}
  ARGS -E copy_if_different $<TARGET_FILE:${LIB_NAME}> ${TET_CPPSDK_LIBRARY_OUTPUT}
)

#add_executable(example_eye_reader src/example_eye_reader.cpp)
#target_link_libraries(example_eye_reader ${LIB_NAME})