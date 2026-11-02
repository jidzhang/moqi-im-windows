cmake_minimum_required(VERSION 3.0)

project(MoqiTextService)

# http://www.utf8everywhere.org/
add_definitions(/D_UNICODE=1 /DUNICODE=1)
set(CMAKE_CXX_STANDARD 20)

# generate the resource file containing version info
configure_file("MoqiTextService.rc.in" "MoqiTextService.rc" @ONLY)

include_directories(
    ${CMAKE_SOURCE_DIR}
    ${jsoncpp_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_library(MoqiTextService SHARED
    # core IME implementation
    MoqiImeModule.cpp
    MoqiImeModule.h
    MoqiTextService.cpp
    MoqiTextService.def
    MoqiTextService.h
    MoqiCandidateWindow.cpp
    MoqiCandidateWindow.h
    MoqiClient.cpp
    MoqiClient.h
    JsonCompat.h
    LocalEngine.cpp
    LocalEngine.h
    LocalRerank.cpp
    LocalRerank.h
    RimeCore.cpp
    RimeCore.h
    MoqiLangBarButton.cpp
    MoqiLangBarButton.h
    DllEntry.cpp
    # resources
    ${CMAKE_CURRENT_BINARY_DIR}/MoqiTextService.rc
)

target_link_libraries(MoqiTextService
    libIME2_static
    jsoncpp_static
)

# 纯 JSON 解释层单元测试（不依赖 TSF）。覆盖 2026-11-02 三次线上事故的
# 回归点：叶子菜单项 submenu:[]、changeButton 零值洗状态、分隔符形态。
# 运行：cmake --build <build> --config Release --target protocol_json_test
#       && <build>/MoqiTextService/Release/protocol_json_test.exe
add_executable(protocol_json_test tests/protocol_json_test.cpp)
target_link_libraries(protocol_json_test jsoncpp_static)
add_test(NAME protocol_json_test COMMAND protocol_json_test)
