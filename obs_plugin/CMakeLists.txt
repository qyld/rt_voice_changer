cmake_minimum_required(VERSION 3.20)
project(obs-ai-voice VERSION 2.0.0 LANGUAGES CXX)

# OBS 30.x 依赖：先运行 scripts\fetch_obs_deps.ps1 生成 deps\libobs\{include,lib}
set(LIBOBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/libobs" CACHE PATH "libobs 头文件与 obs.lib 所在目录")
set(OBS_PORTABLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/obs-portable" CACHE PATH "OBS 安装/便携版目录（用于 install_to_obs 目标）")
set(SHM_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../voice_changer/src" CACHE PATH "shm_output.h 所在目录")

if(NOT EXISTS "${LIBOBS_DIR}/include/obs-module.h" OR NOT EXISTS "${LIBOBS_DIR}/lib/obs.lib")
  message(FATAL_ERROR "未找到 libobs 依赖，请先运行: powershell -ExecutionPolicy Bypass -File scripts/fetch_obs_deps.ps1")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # 静态 CRT，插件无额外运行库依赖
add_compile_options(/utf-8 /W3 /permissive-)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

add_library(libobs_import INTERFACE)
target_include_directories(libobs_import INTERFACE "${LIBOBS_DIR}/include")
target_link_libraries(libobs_import INTERFACE "${LIBOBS_DIR}/lib/obs.lib")

# ---- 插件 ----
add_library(obs-ai-voice MODULE src/plugin.cpp src/obs-ai-voice.rc)
target_include_directories(obs-ai-voice PRIVATE "${SHM_INCLUDE_DIR}")
target_link_libraries(obs-ai-voice PRIVATE libobs_import winmm)
set_target_properties(obs-ai-voice PROPERTIES PREFIX "" OUTPUT_NAME "obs-ai-voice")

# ---- 测试写端（模拟变声器输出 440Hz 正弦）----
add_executable(test_writer test_writer.cpp)
target_include_directories(test_writer PRIVATE "${SHM_INCLUDE_DIR}")
target_link_libraries(test_writer PRIVATE winmm)

# ---- 单元测试 ----
enable_testing()
add_executable(test_shm tests/test_shm.cpp)
target_include_directories(test_shm PRIVATE "${SHM_INCLUDE_DIR}")
add_test(NAME shm_unit COMMAND test_shm)

# ---- 拷贝到 OBS 目录：cmake --build build --config Release --target install_to_obs ----
add_custom_target(install_to_obs
  COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_PORTABLE_DIR}/obs-plugins/64bit"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:obs-ai-voice>" "${OBS_PORTABLE_DIR}/obs-plugins/64bit/"
  DEPENDS obs-ai-voice
  COMMENT "安装 obs-ai-voice.dll 到 ${OBS_PORTABLE_DIR}/obs-plugins/64bit")
