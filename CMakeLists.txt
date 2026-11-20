cmake_minimum_required(VERSION 3.21)
# Satisfy older FetchContent deps (e.g. jsoncpp) under CMake 4.x; avoids fragile -D on command line.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Explicitly control the MSVC runtime before project() so generated VS projects
# keep the legacy PIME behavior: /MDd for Debug, /MD for RelWithDebInfo,
# and /MT for Release/MinSizeRel.
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>$<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:DLL>")

# Override default c/c++ flags to keep MSVC release builds self-contained.
set(CMAKE_USER_MAKE_RULES_OVERRIDE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/c_flag_overrides.cmake")
set(CMAKE_USER_MAKE_RULES_OVERRIDE_CXX "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cxx_flag_overrides.cmake")

project(MoqiIMWindows LANGUAGES C CXX)

# 空间优化：/Gy 函数级链接 + 链接期 /OPT:REF,ICF 死代码消除/相同 COMDAT 折叠。
# 上方 flag override 的 Release 只给 /O2 /Ob2、不含 /Gy，此处补上才能真正按
# 函数粒度删死代码。LTCG 不启用：TSF 是小项目，每键大头是等 daemon + GDI 系统
# 调用，LTCG 够得着的自有 CPU 很小，无正收益实测结论（daemon 大代码量才有同源
# A/B −8%）；不凭信心启用。PGO 已证慢 7-10%（56-交接），不启用。
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Gy")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /Gy")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /OPT:REF /OPT:ICF")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /OPT:REF /OPT:ICF")

# 独立单元测试（MoqiTextService/tests/protocol_json_test.cpp）。
enable_testing()

include(FetchContent)

# Load semantic version from version.txt.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" MOQI_VERSION)
string(REGEX MATCHALL "[0-9]+" MOQI_VERSION_PARTS "${MOQI_VERSION}")
list(GET MOQI_VERSION_PARTS 0 MOQI_VERSION_MAJOR)
list(GET MOQI_VERSION_PARTS 1 MOQI_VERSION_MINOR)
list(GET MOQI_VERSION_PARTS 2 MOQI_VERSION_PATCH)

add_definitions(
    /D_UNICODE=1
    /DUNICODE=1
    /D_CRT_SECURE_NO_WARNINGS
    /GR-
)

# Windows 7 targeting (2026-11-19). ON by default: one package must run on both
# Windows 7 SP1 and Windows 10/11, so the published binaries target the older
# floor. With it ON the SDK headers only expose Win7-era APIs, which turns any
# accidental Win8+ call into a compile error instead of a runtime "entry point
# not found"; the linker also stamps subsystem 6.01.
# Windows 7 SP1 is required (0x0601 == NT 6.1); Win7 RTM is not supported.
# Set -DMOQI_TARGET_WIN7=OFF to build against the 0x0602 floor again (the
# feature-specific Win8+ behaviour is runtime-gated anyway, see DaemonTray).
option(MOQI_TARGET_WIN7 "Build for Windows 7 SP1 (NT 6.1): hide Win8+ APIs at compile time" ON)
if(MOQI_TARGET_WIN7)
    set(MOQI_WINVER 0x0601)
    set(MOQI_NTDDI 0x06010000)
else()
    set(MOQI_WINVER 0x0602)
    set(MOQI_NTDDI 0x06020000)
endif()
add_definitions(
    -DWIN32_LEAN_AND_MEAN
    -D_WIN32_WINNT=${MOQI_WINVER}
    -DNTDDI_VERSION=${MOQI_NTDDI}
)
if(MOQI_TARGET_WIN7)
    # Subsystem version is what Windows' loader actually enforces. Without this
    # the linker stamps 6.00/6.02 from the default toolchain and the exe/dll can
    # still be refused on Win7 even though every imported API exists.
    # Only the DLLs get it globally: applying WINDOWS to every EXE would break
    # the console unit tests, which need CONSOLE. GUI executables (daemon,
    # SetupHelper) set their own subsystem in their own CMakeLists.
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /SUBSYSTEM:WINDOWS,6.01")
    message(STATUS "MOQI_TARGET_WIN7=ON: _WIN32_WINNT=0x0601, subsystem 6.01")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_definitions(
        _HAS_DEPRECATED_RESULT_OF=1
        _SILENCE_CXX17_RESULT_OF_DEPRECATION_WARNING
    )
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
        $<$<COMPILE_LANGUAGE:CXX>:/Zc:char8_t->
    )
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)

set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_PKGCONFIG_SUPPORT OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_STRICT_ISO OFF CACHE BOOL "" FORCE)

set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_WARNINGS OFF CACHE BOOL "" FORCE)

set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.17.0
)

FetchContent_MakeAvailable(spdlog)

add_subdirectory("${PROJECT_SOURCE_DIR}/jsoncpp")

add_subdirectory("${PROJECT_SOURCE_DIR}/libIME2")
add_subdirectory("${PROJECT_SOURCE_DIR}/libuv")
add_subdirectory("${PROJECT_SOURCE_DIR}/MoqiTextService")
add_subdirectory("${PROJECT_SOURCE_DIR}/Preview")

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_subdirectory("${PROJECT_SOURCE_DIR}/MoqLauncher")
    add_subdirectory("${PROJECT_SOURCE_DIR}/SetupHelper")
endif()
