cmake_minimum_required(VERSION 3.24)
project(VicViewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

include(GNUInstallDirs)

find_package(vpx CONFIG QUIET)

# vcpkg provides unofficial-libvpx CONFIG target name; try that if the canonical one failed.
if(NOT vpx_FOUND)
	find_package(unofficial-libvpx CONFIG QUIET)
	if(TARGET unofficial::libvpx::libvpx)
		add_library(vpx::vpx INTERFACE IMPORTED)
		set_target_properties(vpx::vpx PROPERTIES
			INTERFACE_LINK_LIBRARIES unofficial::libvpx::libvpx)
		set(vpx_FOUND TRUE)
	endif()
endif()

# Manual fallback: directly include the config file from vcpkg if still not found (some environments miss prefix path early).
if(NOT vpx_FOUND AND WIN32)
	if(NOT VCPKG_TARGET_TRIPLET)
		set(VCPKG_TARGET_TRIPLET "x86-windows" CACHE STRING "Default vcpkg triplet guess")
	endif()
	if(DEFINED ENV{VCPKG_ROOT})
		set(_VCPKG_ROOT $ENV{VCPKG_ROOT})
		set(_VPX_CONFIG "${_VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}/share/unofficial-libvpx/unofficial-libvpx-config.cmake")
		if(EXISTS "${_VPX_CONFIG}")
			message(STATUS "Including libvpx config manually: ${_VPX_CONFIG}")
			include("${_VPX_CONFIG}")
			if(TARGET unofficial::libvpx::libvpx AND NOT TARGET vpx::vpx)
				add_library(vpx::vpx INTERFACE IMPORTED)
				set_target_properties(vpx::vpx PROPERTIES INTERFACE_LINK_LIBRARIES unofficial::libvpx::libvpx)
				set(vpx_FOUND TRUE)
			endif()
		endif()
	endif()
endif()

# Fallback to pkg-config only if neither official nor unofficial config packages were found.
if(NOT vpx_FOUND)
	# On Windows with vcpkg we expect unofficial-libvpx; require pkg-config only on non-Windows.
	if(NOT WIN32)
		find_package(PkgConfig REQUIRED)
		pkg_check_modules(VPX REQUIRED vpx)
		add_library(vpx::vpx INTERFACE IMPORTED)
		set_target_properties(vpx::vpx PROPERTIES
			INTERFACE_INCLUDE_DIRECTORIES "${VPX_INCLUDE_DIRS}"
			INTERFACE_LINK_LIBRARIES "${VPX_LIBRARIES}")
		if(VPX_CFLAGS_OTHER)
			set_property(TARGET vpx::vpx PROPERTY INTERFACE_COMPILE_OPTIONS "${VPX_CFLAGS_OTHER}")
		endif()
	else()
		message(FATAL_ERROR "libvpx not found: install via vcpkg (vcpkg install libvpx) or provide a CMake package.")
	endif()
endif()

add_subdirectory(modules)
add_subdirectory(app)
add_subdirectory(service)
enable_testing()
add_subdirectory(tests)
