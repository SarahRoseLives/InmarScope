cmake_minimum_required(VERSION 3.16)
project(InmarScope C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(OpenGL REQUIRED)
find_package(PkgConfig)

find_package(glfw3 CONFIG QUIET)
if(NOT glfw3_FOUND)
    if(NOT PkgConfig_FOUND)
        find_package(PkgConfig REQUIRED)
    endif()
    pkg_check_modules(GLFW REQUIRED glfw3)
endif()

# RTL-SDR
if(PkgConfig_FOUND)
    pkg_check_modules(RTLSDR QUIET librtlsdr)
endif()
if(NOT RTLSDR_FOUND)
    find_library(RTLSDR_LIBRARIES NAMES rtlsdr librtlsdr REQUIRED)
    find_path(RTLSDR_INCLUDE_DIRS NAMES rtl-sdr.h REQUIRED)
endif()

# HackRF (native source). The MSYS2 libhackrf.pc has a hardcoded build-machine
# libusb include path, so locate the header/library directly instead of via
# pkg-config. hackrf.h itself doesn't pull in libusb, so no extra include is
# needed for our wrapper.
find_path(HACKRF_INCLUDE_DIRS NAMES libhackrf/hackrf.h hackrf.h
          PATH_SUFFIXES libhackrf REQUIRED)
find_library(HACKRF_LIBRARIES NAMES hackrf libhackrf REQUIRED)

set(IMGUI_DIR   ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(IMPLOT_DIR  ${CMAKE_SOURCE_DIR}/third_party/implot)

# ---------------------------------------------------------------------------
# Dear ImGui + ImPlot (vendored) compiled as a static lib
# ---------------------------------------------------------------------------
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMPLOT_DIR}/implot.cpp
    ${IMPLOT_DIR}/implot_items.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMPLOT_DIR}
)
# implot_items.cpp is extremely template-heavy: ~5 min to compile at -O2.
# It is not performance-critical for us, so build it at -O1 (~1 min). The last
# -O flag wins in GCC, so this overrides the RelWithDebInfo -O2.
set_source_files_properties(${IMPLOT_DIR}/implot_items.cpp PROPERTIES COMPILE_OPTIONS "-O1")
target_link_libraries(imgui PUBLIC OpenGL::GL)
if(glfw3_FOUND)
    target_link_libraries(imgui PUBLIC glfw)
else()
    target_include_directories(imgui PUBLIC ${GLFW_INCLUDE_DIRS})
    target_link_libraries(imgui PUBLIC ${GLFW_LIBRARIES})
    target_link_directories(imgui PUBLIC ${GLFW_LIBRARY_DIRS})
endif()

# ---------------------------------------------------------------------------
# JAERO DSP (MSK/OQPSK demodulators + AeroL framing), Qt-stripped, plus the
# vendored libcorrect FEC. Built as a static lib with a plain C API.
# ---------------------------------------------------------------------------
set(JDSP ${CMAKE_SOURCE_DIR}/third_party/jaero_dsp)
file(GLOB LIBCORRECT_SOURCES
    ${JDSP}/libcorrect/src/convolutional/*.c
    ${JDSP}/libcorrect/src/reed-solomon/*.c
    ${JDSP}/libcorrect/src/fec_shim.c)

add_library(jaero_dsp STATIC
    ${JDSP}/DSP.cpp
    ${JDSP}/mskdemodulator.cpp
    ${JDSP}/oqpskdemodulator.cpp
    ${JDSP}/burstmskdemodulator.cpp
    ${JDSP}/burstoqpskdemodulator.cpp
    ${JDSP}/coarsefreqestimate.cpp
    ${JDSP}/fftwrapper.cpp
    ${JDSP}/fftrwrapper.cpp
    ${JDSP}/jfft.cpp
    ${JDSP}/jconvolutionalcodec.cpp
    ${JDSP}/aerol.cpp
    ${JDSP}/jaero_demod.cpp
    ${LIBCORRECT_SOURCES}
)
target_include_directories(jaero_dsp PUBLIC
    ${JDSP}
    ${JDSP}/libcorrect/include
)
target_compile_definitions(jaero_dsp PUBLIC _USE_MATH_DEFINES)

# ---------------------------------------------------------------------------
# mbelib (AMBE/IMBE voice codec, C) for Aero C-channel voice decoding.
# ---------------------------------------------------------------------------
file(GLOB MBELIB_SOURCES ${CMAKE_SOURCE_DIR}/third_party/mbelib/*.c)
add_library(mbelib STATIC ${MBELIB_SOURCES})
target_include_directories(mbelib PUBLIC ${CMAKE_SOURCE_DIR}/third_party/mbelib)

# ---------------------------------------------------------------------------
# libacars (vendored): decodes the ACARS application layer (CPDLC, ADS-C,
# MIAM, media-advisory). Built via its own CMake as the acars_static target.
# Examples are skipped (LA_BUILD_EXAMPLES off).
# ---------------------------------------------------------------------------
set(LA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libacars ${CMAKE_BINARY_DIR}/libacars EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# InmarScope application
# ---------------------------------------------------------------------------
if(LINUX)
    add_executable(InmarScope
        src/main.cpp
        src/signal/processing.cpp
        src/voice/voice_ops.cpp
        src/gui/gui_panels.cpp
        src/session/session.cpp
        src/state/config.cpp
        src/sdr/rtl_sdr_source.cpp
        src/sdr/hackrf_source.cpp
        src/sdr/wav_file_source.cpp
        src/sdr/sdrpp_server_source.cpp
        src/sdr/iq_recorder.cpp
        src/gui/waterfall.cpp
        src/decode/decoder.cpp
        src/decode/decoder_manager.cpp
        src/decode/acars_apps.cpp
        src/decode/icao_country.cpp
        src/decode/band_plan.cpp
        src/decode/json_mini.cpp
        src/util/log.cpp
        src/decode/egc/egc_decoder.cpp
        src/voice/ambe_decoder.cpp
        src/voice/wav_writer.cpp
        src/audio/audio_output.cpp
        src/output/message_feed.cpp
        src/update/version_check.cpp
    )
    target_compile_definitions(InmarScope PRIVATE _LINUX)
    target_include_directories(InmarScope PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/mbelib
        ${CMAKE_SOURCE_DIR}/third_party/miniaudio
        ${CMAKE_SOURCE_DIR}/third_party/libacars
        ${RTLSDR_INCLUDE_DIRS}
        ${HACKRF_INCLUDE_DIRS}
    )
endif()

if(WIN32)
    add_executable(InmarScope
        src/main.cpp
        src/signal/processing.cpp
        src/voice/voice_ops.cpp
        src/gui/gui_panels.cpp
        src/session/session.cpp
        src/state/config.cpp
        src/sdr/rtl_sdr_source.cpp
        src/sdr/hackrf_source.cpp
        src/sdr/wav_file_source.cpp
        src/sdr/sdrpp_server_source.cpp
        src/sdr/iq_recorder.cpp
        src/gui/waterfall.cpp
        src/decode/decoder.cpp
        src/decode/decoder_manager.cpp
        src/decode/acars_apps.cpp
        src/decode/icao_country.cpp
        src/decode/band_plan.cpp
        src/decode/json_mini.cpp
        src/util/log.cpp
        src/decode/egc/egc_decoder.cpp
        src/voice/ambe_decoder.cpp
        src/voice/wav_writer.cpp
        src/audio/audio_output.cpp
        src/output/message_feed.cpp
        src/update/version_check.cpp
        src/web/flight_map_webview.cpp
    )
    # Microsoft Edge WebView2 for the embedded Flight Map browser.
    set(WEBVIEW2_ROOT ${CMAKE_SOURCE_DIR}/third_party/webview2/build/native)
    find_library(WEBVIEW2_LIB WebView2Loader.dll PATHS ${WEBVIEW2_ROOT}/x64 NO_DEFAULT_PATH REQUIRED)
    target_include_directories(InmarScope PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/mbelib
        ${CMAKE_SOURCE_DIR}/third_party/miniaudio
        ${CMAKE_SOURCE_DIR}/third_party/libacars
        ${WEBVIEW2_ROOT}/include
        ${RTLSDR_INCLUDE_DIRS}
        ${HACKRF_INCLUDE_DIRS}
    )
endif()

target_compile_definitions(InmarScope PRIVATE _USE_MATH_DEFINES)
find_library(ZSTD_LIBRARIES NAMES zstd libzstd REQUIRED)
find_library(ZLIB_LIB NAMES z libz zlib REQUIRED)

# libogg + libvorbis for OGG Vorbis voice recording.
if(PkgConfig_FOUND)
    pkg_check_modules(OGG REQUIRED ogg)
    pkg_check_modules(VORBIS REQUIRED vorbis)
    pkg_check_modules(VORBISENC REQUIRED vorbisenc)
endif()
if(NOT OGG_FOUND OR NOT VORBIS_FOUND)
    find_library(OGG_LIBRARIES NAMES ogg libogg REQUIRED)
    find_library(VORBIS_LIBRARIES NAMES vorbis libvorbis REQUIRED)
    find_library(VORBISENC_LIBRARIES NAMES vorbisenc libvorbisenc REQUIRED)
endif()

# compile for platform
if(LINUX)
    message(STATUS "Building InmarScope for GNU/Linux")
    target_link_libraries(InmarScope PRIVATE imgui jaero_dsp mbelib acars_static ${ZLIB_LIB} ${RTLSDR_LIBRARIES} ${HACKRF_LIBRARIES} ${ZSTD_LIBRARIES} ${OGG_LIBRARIES} ${VORBIS_LIBRARIES} ${VORBISENC_LIBRARIES})
    target_link_directories(InmarScope PRIVATE ${RTLSDR_LIBRARY_DIRS})
    target_link_libraries(InmarScope PRIVATE jansson xml2)
endif()

if(WIN32)
    message(STATUS "Building InmarScope for Microsoft Windows")
    target_link_libraries(InmarScope PRIVATE imgui jaero_dsp mbelib acars_static ${ZLIB_LIB} ${RTLSDR_LIBRARIES} ${HACKRF_LIBRARIES} ${ZSTD_LIBRARIES} ${OGG_LIBRARIES} ${VORBIS_LIBRARIES} ${VORBISENC_LIBRARIES} ${WEBVIEW2_LIB})
    target_link_directories(InmarScope PRIVATE ${RTLSDR_LIBRARY_DIRS})
    target_link_libraries(InmarScope PRIVATE opengl32 gdi32 comdlg32 ole32 winmm ws2_32 winhttp shell32)

    # Build as a GUI-subsystem app so no console window opens alongside the GUI.
    # MinGW provides a WinMain shim, so the existing int main() still works.
    set_target_properties(InmarScope PROPERTIES WIN32_EXECUTABLE TRUE)

    # Embed the application icon (icon.ico in the project root). Windows and
    # GLFW both pick up the first icon resource for the exe and window.
    if(EXISTS "${CMAKE_SOURCE_DIR}/icon.ico")
        enable_language(RC)
        set(FRAMESYNC_ICON "${CMAKE_SOURCE_DIR}/icon.ico")
        configure_file("${CMAKE_SOURCE_DIR}/app.rc.in" "${CMAKE_BINARY_DIR}/app.rc" @ONLY)
        target_sources(InmarScope PRIVATE "${CMAKE_BINARY_DIR}/app.rc")
    endif()

    # Copy MinGW runtime DLLs next to the executable so it runs standalone
    # (PyCharm, double-click) without the MSYS2 bin dir on PATH.
    get_filename_component(MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(FRAMESYNC_RUNTIME_DLLS
        libgcc_s_seh-1.dll
        libwinpthread-1.dll
        libstdc++-6.dll
        glfw3.dll
        librtlsdr.dll
        libhackrf.dll
        libusb-1.0.dll
        libzstd.dll
        zlib1.dll
        libogg-0.dll
        libvorbis-0.dll
        libvorbisenc-2.dll
        WebView2Loader.dll
    )
    foreach(dll ${FRAMESYNC_RUNTIME_DLLS})
        if(EXISTS "${MINGW_BIN}/${dll}")
            add_custom_command(TARGET InmarScope POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${MINGW_BIN}/${dll}" "$<TARGET_FILE_DIR:InmarScope>"
                COMMENT "Copying ${dll}")
        else()
            message(WARNING "Runtime DLL not found: ${MINGW_BIN}/${dll}")
        endif()
    endforeach()
    # WebView2Loader.dll is not in the MINGW bin dir — copy from vendored SDK.
    add_custom_command(TARGET InmarScope POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${WEBVIEW2_ROOT}/x64/WebView2Loader.dll"
                "$<TARGET_FILE_DIR:InmarScope>"
        COMMENT "Copying WebView2Loader.dll")
endif()
