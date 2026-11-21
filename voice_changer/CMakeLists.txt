cmake_minimum_required(VERSION 3.20)
project(VoiceChanger CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(ORT_DIR ${CMAKE_SOURCE_DIR}/third_party/onnxruntime)

if(MSVC)
  add_compile_options(/utf-8 /O2 /fp:fast /arch:AVX2 /W3 /MP)
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
endif()

# 核心引擎 (与界面无关)
add_library(vc_core STATIC src/vc_engine.cpp src/audio_io.cpp src/gpu_enum.cpp)
target_include_directories(vc_core PUBLIC src ${ORT_DIR}/include third_party/miniaudio)
target_link_libraries(vc_core PUBLIC ${ORT_DIR}/lib/onnxruntime.lib dxgi)

# 命令行工具
add_executable(vc_cli src/vc_cli.cpp)
target_link_libraries(vc_cli PRIVATE vc_core)

# 图形界面 (Win32 + WebView2, 前端 ui/index.html 编入资源)
set(WV2_DIR ${CMAKE_SOURCE_DIR}/third_party/webview2)
add_executable(VoiceChanger WIN32 src/gui_main.cpp src/app.rc)
target_include_directories(VoiceChanger PRIVATE ${WV2_DIR}/include)
target_link_libraries(VoiceChanger PRIVATE vc_core ${WV2_DIR}/lib/WebView2LoaderStatic.lib shlwapi version ole32 shell32 advapi32)
set_source_files_properties(src/app.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/ui/index.html;${CMAKE_SOURCE_DIR}/src/app.ico")

foreach(t vc_cli VoiceChanger)
  add_custom_command(TARGET ${t} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORT_DIR}/lib/onnxruntime.dll ${ORT_DIR}/lib/onnxruntime_providers_shared.dll ${ORT_DIR}/lib/DirectML.dll $<TARGET_FILE_DIR:${t}>)
endforeach()
