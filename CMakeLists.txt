cmake_minimum_required(VERSION 3.21)
project(WinCaptureSync LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # Use static MSVC runtime (/MT, /MTd) to simplify single-file distribution.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # Require elevation (UAC prompt) for all generated executables.
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} /MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")
endif()

add_compile_definitions(
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)

set(SRC_FILES
    src/main/main.cpp
    src/main/app_controller.cpp
    src/main/config.cpp
    src/main/session_paths.cpp
    src/gui/main_window.cpp
    src/gui/window_selector.cpp
    src/time/time_utils.cpp
    src/input/input_writer.cpp
    src/input/input_recorder.cpp
    src/capture/capture_source.cpp
    src/capture/wgc_frame_grabber.cpp
    src/capture/screen_recorder.cpp
    src/encode/mf_h264_encoder.cpp
)

add_executable(WinCaptureSync WIN32 ${SRC_FILES})
target_include_directories(WinCaptureSync PRIVATE src)
target_link_libraries(WinCaptureSync PRIVATE
    user32
    gdi32
    dwmapi
    shcore
    ole32
    oleaut32
    mfplat
    mf
    mfreadwrite
    mfuuid
    xinput
    windowsapp
)

if(MSVC)
    target_compile_options(WinCaptureSync PRIVATE /W4 /EHsc)
else()
    target_compile_options(WinCaptureSync PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()

add_executable(time_tests
    tests/time_tests.cpp
    src/time/time_utils.cpp
)
target_include_directories(time_tests PRIVATE src)
target_link_libraries(time_tests PRIVATE ole32)
add_test(NAME time_tests COMMAND time_tests)
