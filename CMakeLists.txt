cmake_minimum_required(VERSION 3.20)
project(AudioRouter VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(IMGUI_DEFINE_MATH_OPERATORS)

include(FetchContent)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.4  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(imnodes
    GIT_REPOSITORY https://github.com/Nelarius/imnodes.git
    GIT_TAG        v0.5  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imnodes)

# ── RNNoise v0.1 — bundled, self-contained, no generated files needed ────────
set(RNNOISE_SRC
    thirdparty/rnnoise/src/celt_lpc.c
    thirdparty/rnnoise/src/denoise.c
    thirdparty/rnnoise/src/kiss_fft.c
    thirdparty/rnnoise/src/pitch.c
    thirdparty/rnnoise/src/rnn.c
    thirdparty/rnnoise/src/rnn_data.c
)

add_library(rnnoise_bundled STATIC ${RNNOISE_SRC})
target_include_directories(rnnoise_bundled PUBLIC
    thirdparty/rnnoise/include
    thirdparty/rnnoise/src
)
target_compile_definitions(rnnoise_bundled PRIVATE _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
if(MSVC)
    target_compile_options(rnnoise_bundled PRIVATE /W0 /wd4244 /wd4305 /wd4996)
endif()

# ── Main sources ──────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/Logger.cpp
    src/CpuInfo.cpp
    src/AudioMath.cpp
    src/AudioMath_Scalar.cpp
    src/AudioMath_AVX2.cpp
    src/AudioEngine.cpp
    src/WasapiManager.cpp
    src/AsioManager.cpp
    src/RoutingGraph.cpp
    src/UI/NodeEditorUI.cpp
    src/UI/DevicePanel.cpp
    src/UI/LevelMeter.cpp
    src/UI/VolumeMixer.cpp
    resources/app.rc
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imnodes_SOURCE_DIR}/imnodes.cpp
)

add_executable(AudioRouter WIN32 ${SOURCES})

if(MSVC)
    set_source_files_properties(src/AudioMath_AVX2.cpp
        PROPERTIES COMPILE_FLAGS "/arch:AVX2")
    set_source_files_properties(${imnodes_SOURCE_DIR}/imnodes.cpp
        PROPERTIES COMPILE_FLAGS "/wd4005")
endif()

target_include_directories(AudioRouter PRIVATE
    src/
    src/UI/
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imnodes_SOURCE_DIR}
    thirdparty/asio
    thirdparty/rnnoise/include
)

target_link_libraries(AudioRouter PRIVATE
    rnnoise_bundled
    d3d11 dxgi d3dcompiler ole32 oleaut32 uuid winmm ksuser avrt winhttp
)

target_compile_definitions(AudioRouter PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    HAVE_RNNOISE=1
)

if(MSVC)
    target_compile_options(AudioRouter PRIVATE /W3 /MP)
    set_target_properties(AudioRouter PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup"
    )
endif()

set_target_properties(AudioRouter PROPERTIES
    OUTPUT_NAME "AudioRouter"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
