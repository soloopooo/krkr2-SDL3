cmake_minimum_required(VERSION 3.6)
project(krkr2yuri)

set(CMAKE_CXX_STANDARD 11)
set(KRKR2CORE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
set(KRKR2PLUGIN_PATH ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins)
set(COCOS2DX_PATH ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/port/cocos2d-x)

option(KRKR2_BUILD_TESTS "Build host unit tests (no engine/cocos/android)" OFF)
option(KRKR2_USE_COCOS2D "Build with cocos2d-x rendering (vs SDL3)" ON)

if(CMAKE_SYSTEM_NAME MATCHES "Android")
	# --- Android: full engine + plugins + JNI shared lib ---
	add_subdirectory(${KRKR2CORE_PATH})
	add_subdirectory(${KRKR2PLUGIN_PATH})

	if(KRKR2_USE_COCOS2D)
		add_library(${PROJECT_NAME} SHARED
			project/android/app/cpp/krkr2_android.cpp
		)
		target_compile_options(${PROJECT_NAME} PUBLIC -fPIE)
		target_include_directories(${PROJECT_NAME} PUBLIC
			${KRKR2CORE_PATH}/environ/cocos2d
			${PORTBUILD_PATH}/include
			${PORTBUILD_PATH}/include/breakpad
			${COCOS2DX_PATH}/cocos
			${COCOS2DX_PATH}/cocos/audio/include
		)
		target_link_directories(${PROJECT_NAME} PUBLIC ${PORTBUILD_PATH}/lib)
		target_link_libraries(${PROJECT_NAME} PUBLIC
			-Wl,-Bstatic
			-Wl,--whole-archive
			cpp_android_spec krkr2plugin
			-Wl,--no-whole-archive
			krkr2core
		)
	else()
		add_library(${PROJECT_NAME} SHARED
			project/android/app/cpp/krkr2_android_sdl.cpp
		)
		target_compile_options(${PROJECT_NAME} PUBLIC -fPIE -DKRKR2_SDL_BUILD -DKRKR2_USE_VULKAN)
		target_include_directories(${PROJECT_NAME} PUBLIC
			${KRKR2CORE_PATH}/environ/sdl
			${PORTBUILD_PATH}/include
			${PORTBUILD_PATH}/include/breakpad
			${COCOS2DX_PATH}/external
			${COCOS2DX_PATH}/external/tinyxml2
			${COCOS2DX_PATH}/external/unzip
			${KRKR2CORE_PATH}
			${KRKR2CORE_PATH}/base
			${KRKR2CORE_PATH}/base/win32
			${KRKR2CORE_PATH}/environ
			${KRKR2CORE_PATH}/environ/win32
			${KRKR2CORE_PATH}/environ/android
			${KRKR2CORE_PATH}/msg
			${KRKR2CORE_PATH}/msg/win32
			${KRKR2CORE_PATH}/extension
			${KRKR2CORE_PATH}/sound
			${KRKR2CORE_PATH}/sound/win32
			${KRKR2CORE_PATH}/tjs2
			${KRKR2CORE_PATH}/utils
			${KRKR2CORE_PATH}/utils/win32
			${KRKR2CORE_PATH}/visual
			${KRKR2CORE_PATH}/visual/ARM
			${KRKR2CORE_PATH}/visual/win32
			${KRKR2CORE_PATH}/visual/RenderScript/rs
		)
		target_link_directories(${PROJECT_NAME} PUBLIC ${PORTBUILD_PATH}/lib)
		target_link_libraries(${PROJECT_NAME} PUBLIC
			-Wl,-Bstatic
			-Wl,--whole-archive
			krkr2plugin
			-Wl,--no-whole-archive
			krkr2core
			sdl3_static
			ext_tinyxml2 ext_xxhash ext_unzip
		)
	endif()

elseif(KRKR2_BUILD_TESTS)
	# --- Host: unit tests only (no engine, no cocos, no android) ---
	enable_testing()
	add_subdirectory(tests)
else()
	message(FATAL_ERROR "${CMAKE_SYSTEM_NAME} not supported yet. "
		"Use -DKRKR2_BUILD_TESTS=ON for the host unit-test suite.")
endif()
