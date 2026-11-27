cmake_minimum_required(VERSION 3.20)
project(MoqiPanel CXX)

# 墨奇输入板（2026-11-26）：绿色免安装的独立面板。自带 MoqiDaemon --portable
# 当输入引擎，不注册输入法、不写注册表，所有文件都在同一个文件夹里。
#
# 独立于顶层工程（不进 build.ps1 / CI / 安装包链），构建入口是
# package-panel.ps1；手工构建：
#   cmake -S MoqiPanel -B MoqiPanel/build-x86 -G "Visual Studio 17 2022" -A Win32
#   cmake --build MoqiPanel/build-x86 --config Release

if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
# CRT 全静态：绿色版不依赖目标机装没装 VC++ 运行库。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MOQI_WIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Windows 7 SP1 目标（与 daemon 同口径）：Win8+ API 在编译期变成错误，
# 子系统版本钉 6.01，一套二进制通吃 Win7 与 Win10/11。
option(MOQI_TARGET_WIN7 "Build for Windows 7 SP1 (NT 6.1)" ON)
if(MOQI_TARGET_WIN7)
  add_compile_definitions(_WIN32_WINNT=0x0601 NTDDI_VERSION=0x06010000)
  set(MOQI_PANEL_SUBSYSTEM_FLAG "/SUBSYSTEM:WINDOWS,6.01")
  message(STATUS "MOQI_TARGET_WIN7=ON (panel): _WIN32_WINNT=0x0601, subsystem 6.01")
else()
  add_compile_definitions(_WIN32_WINNT=0x0602 NTDDI_VERSION=0x06020000)
  set(MOQI_PANEL_SUBSYSTEM_FLAG "")
endif()

add_compile_definitions(
  _UNICODE=1 UNICODE=1
  WIN32_LEAN_AND_MEAN NOMINMAX
  _CRT_SECURE_NO_WARNINGS
  _SILENCE_CXX17_RESULT_OF_DEPRECATION_WARNING
)
add_compile_options(/utf-8 /Zc:char8_t- /W4 /permissive-)

# ---- 版本资源：与 daemon 同法读外层 version.txt，缺文件回落 0.0.0 ----
set(MOQI_VERSION_FILE "${MOQI_WIN_DIR}/version.txt")
set(MOQI_VERSION_MAJOR 0)
set(MOQI_VERSION_MINOR 0)
set(MOQI_VERSION_PATCH 0)
if(EXISTS "${MOQI_VERSION_FILE}")
  file(READ "${MOQI_VERSION_FILE}" MOQI_VERSION_STRING)
  string(STRIP "${MOQI_VERSION_STRING}" MOQI_VERSION_STRING)
  if(MOQI_VERSION_STRING MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(MOQI_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(MOQI_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(MOQI_VERSION_PATCH "${CMAKE_MATCH_3}")
  endif()
endif()
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/MoqiPanel.rc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/MoqiPanelVersion.rc"
  @ONLY
)

# jsoncpp 与 daemon/TSF 共用同一份源码（协议头 ProtoFraming.h 也依赖它）。
set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_PKGCONFIG_SUPPORT OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_STRICT_ISO OFF CACHE BOOL "" FORCE)
add_subdirectory("${MOQI_WIN_DIR}/jsoncpp" jsoncpp EXCLUDE_FROM_ALL)

enable_testing()

# 纯逻辑层：标点转换 / 翻译配置与应答解析。不碰窗口，单元测试直接链接。
add_library(moqi_panel_core STATIC
  src/PanelCore.cpp
  src/PanelCore.h
  src/TranslateCore.cpp
  src/TranslateCore.h
)
target_include_directories(moqi_panel_core PUBLIC src
  "${MOQI_WIN_DIR}/jsoncpp/include")
target_link_libraries(moqi_panel_core PUBLIC jsoncpp_static)

add_executable(MoqiPanel WIN32
  src/MoqiPanelMain.cpp
  src/EngineClient.cpp
  src/EngineClient.h
  src/PanelActions.cpp
  src/PanelMessages.h
  src/PanelStyle.h
  src/PanelWindow.cpp
  src/PanelWindow.h
  src/SettingsDialog.cpp
  src/SettingsDialog.h
  src/TranslateHttp.cpp
  src/TranslateHttp.h
  src/MoqiPanel.rc
  "${CMAKE_CURRENT_BINARY_DIR}/MoqiPanelVersion.rc"
)
target_include_directories(MoqiPanel PRIVATE
  src "${MOQI_WIN_DIR}/proto")
target_link_libraries(MoqiPanel PRIVATE
  moqi_panel_core
  winhttp comctl32 imm32 user32 gdi32 shell32 ole32)
# GUI 子系统；/SUBSYSTEM:WINDOWS,6.01 只加在本目标上（控制台单元测试要 CONSOLE）。
# /MANIFEST:NO：manifest 已经由 MoqiPanel.rc 以 RT_MANIFEST 资源嵌入，不让链接器
# 再自动生成一份（两份同名资源会让 cvtres 报 CVT1100 资源重复）。
set_target_properties(MoqiPanel PROPERTIES
  LINK_FLAGS "${MOQI_PANEL_SUBSYSTEM_FLAG} /MANIFEST:NO")

# 纯逻辑回归：标点映射逐条锁定、字数护栏、配置解析的类型安全、应答解析。
add_executable(panel_core_test tests/panel_core_test.cpp)
target_link_libraries(panel_core_test PRIVATE moqi_panel_core)
add_test(NAME panel_core_test COMMAND panel_core_test)

# 崩溃取证工具（可选目标，不进默认构建）：以调试器方式拉起子进程，第一次
# 访问冲突时打印带符号的调用栈。见 tests/daemon_crashtrace.py。
add_executable(crashtrace EXCLUDE_FROM_ALL tests/tools/crashtrace.cpp)
target_link_libraries(crashtrace PRIVATE dbghelp)
# 跳板：冒充 MoqiDaemon.exe 让真实的面板拉起链路跑到调试器下（取证用）。
add_executable(tramp EXCLUDE_FROM_ALL tests/tools/tramp.cpp)
