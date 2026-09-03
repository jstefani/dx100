cmake_minimum_required(VERSION 3.5)
project(DX100Voice)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT SC_PATH)
	if(EXISTS "/usr/local/include/SuperCollider/plugin_interface/SC_PlugIn.h")
		set(SC_PATH "/usr/local/include/SuperCollider")
	elseif(EXISTS "/usr/include/SuperCollider/plugin_interface/SC_PlugIn.h")
		set(SC_PATH "/usr/include/SuperCollider")
	else()
		message(FATAL_ERROR "SC_PATH must point at SuperCollider headers")
	endif()
endif()

if(EXISTS "${SC_PATH}/include/plugin_interface/SC_PlugIn.h")
	set(SC_INC "${SC_PATH}/include")
	set(SC_COMMON "${SC_PATH}/common")
elseif(EXISTS "${SC_PATH}/plugin_interface/SC_PlugIn.h")
	set(SC_INC "${SC_PATH}")
	set(SC_COMMON "${SC_PATH}/common")
else()
	message(FATAL_ERROR "No SC_PlugIn.h under ${SC_PATH}")
endif()

add_library(DX100Voice MODULE DX100Voice.cpp)

target_include_directories(DX100Voice PRIVATE
	"${SC_INC}/plugin_interface"
	"${SC_INC}/common"
	"${SC_COMMON}"
)

# Do not define SUPERNOVA — even SUPERNOVA=0 makes #ifdef SUPERNOVA true
# and scsynth will refuse the plugin (server_type 1 vs 0).

if(APPLE)
	set_target_properties(DX100Voice PROPERTIES PREFIX "" SUFFIX ".scx")
elseif(UNIX)
	set_target_properties(DX100Voice PROPERTIES PREFIX "" SUFFIX ".so")
endif()
