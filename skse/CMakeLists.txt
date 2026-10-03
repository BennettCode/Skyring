cmake_minimum_required(VERSION 3.25)

project(SkyrimXER VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# CommonLibSSE-NG, AE runtime only: this mod targets SkyrimSE.exe 1.7.104 and nothing else.
set(ENABLE_SKYRIM_SE OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)
add_subdirectory(extern/CommonLibSSE-NG CommonLibSSE EXCLUDE_FROM_ALL)
include(extern/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)

file(GLOB_RECURSE SKYRIMXER_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h include/*.h)

add_commonlibsse_plugin(
	SkyrimXER
	NAME "SkyrimXER"
	AUTHOR "Skyrim X Elden Ring contributors"
	VERSION ${PROJECT_VERSION}
	USE_ADDRESS_LIBRARY
	SOURCES ${SKYRIMXER_SOURCES}
)

target_include_directories(SkyrimXER PRIVATE include src ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/generated)
target_precompile_headers(SkyrimXER PRIVATE include/PCH.h)
target_compile_definitions(SkyrimXER PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
if(MSVC)
	target_compile_options(SkyrimXER PRIVATE /W4 /permissive- /Zc:preprocessor /utf-8 /MP)
	target_link_options(SkyrimXER PRIVATE /DEBUG:FULL)
endif()
