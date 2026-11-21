cmake_minimum_required(VERSION 3.20)
project(VoiceChanger CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(ORT_DIR ${CMAKE_SOURCE_DIR}/third_party/onnxruntime)
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

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

# 图形界面
add_executable(VoiceChanger WIN32 src/gui_main.cpp
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(VoiceChanger PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(VoiceChanger PRIVATE vc_core d3d11 d3dcompiler dxgi)

foreach(t vc_cli VoiceChanger)
  add_custom_command(TARGET ${t} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORT_DIR}/lib/onnxruntime.dll ${ORT_DIR}/lib/onnxruntime_providers_shared.dll ${ORT_DIR}/lib/DirectML.dll $<TARGET_FILE_DIR:${t}>)
endforeach()
