cmake_minimum_required(VERSION 3.16)
project(Stand)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ─── Qt ───────────────────────────────────────────────────────────────────────
list(APPEND CMAKE_PREFIX_PATH "C:/Qt/6.10.0/mingw_64")
find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent PrintSupport Multimedia)

# ─── LimeSuite ────────────────────────────────────────────────────────────────
set(LIMESUITE_ROOT "C:/LimeSuite")
include_directories(${LIMESUITE_ROOT}/include)
link_directories(${LIMESUITE_ROOT}/lib)

# ─── FFTW (double) ────────────────────────────────────────────────────────────
set(FFTW_ROOT        "${CMAKE_SOURCE_DIR}/external/FFTW")
set(FFTW_INCLUDE_DIR "${FFTW_ROOT}/include")
set(FFTW_LIB_DIR     "${FFTW_ROOT}/lib")
set(FFTW_BIN_DIR     "${FFTW_ROOT}/bin")

# ─── QCustomPlot ──────────────────────────────────────────────────────────────
set(QCUSTOMPLOT_DIR "${CMAKE_SOURCE_DIR}/external/qcustomplot")
add_library(qcustomplot STATIC
        ${QCUSTOMPLOT_DIR}/qcustomplot.cpp
        ${QCUSTOMPLOT_DIR}/qcustomplot.h
)
target_include_directories(qcustomplot PUBLIC "${QCUSTOMPLOT_DIR}")
target_link_libraries(qcustomplot PUBLIC Qt6::Widgets Qt6::PrintSupport)

if (MINGW)
    target_compile_options(qcustomplot PRIVATE -Wa,-mbig-obj -g0)
endif ()

# ─── Main executable ──────────────────────────────────────────────────────────
# Directory layout:
#   Application/  — UI layer (Qt widgets only, no direct hardware access)
#   Hardware/     — LimeSDR: Device, LimeManager, StreamWorker, DeviceController
#   DSP/          — Signal processing: FftProcessor, BandpassExporter, FmDemodulator
#   Audio/        — Audio output: FmAudioOutput (QAudioSink wrapper + resampler)
#   Core/         — Shared utilities: Logger, LimeException
add_executable(Stand
        main.cpp
        resources.qrc

        Application/Application.cpp
        Application/Application.h

        Hardware/Device.cpp
        Hardware/Device.h
        Hardware/LimeManager.cpp
        Hardware/LimeManager.h
        Hardware/StreamWorker.cpp
        Hardware/StreamWorker.h
        Hardware/DeviceController.cpp
        Hardware/DeviceController.h

        DSP/FftProcessor.cpp
        DSP/FftProcessor.h
        DSP/BandpassExporter.cpp
        DSP/BandpassExporter.h
        DSP/FmDemodulator.cpp
        DSP/FmDemodulator.h

        Audio/FmAudioOutput.cpp
        Audio/FmAudioOutput.h

        Core/IDevice.h
        Core/IDeviceManager.h
        Core/IPipelineHandler.h
        Core/LimeException.h
        Core/Logger.cpp
        Core/Logger.h
)

target_include_directories(Stand PRIVATE
        "${FFTW_INCLUDE_DIR}"
        "${CMAKE_SOURCE_DIR}/Core"
        "${CMAKE_SOURCE_DIR}/Hardware"
        "${CMAKE_SOURCE_DIR}/DSP"
        "${CMAKE_SOURCE_DIR}/Audio"
)

target_link_libraries(Stand
        PRIVATE
        Qt6::Widgets
        Qt6::Concurrent
        Qt6::PrintSupport
        Qt6::Multimedia
        qcustomplot
        LimeSuite.lib
        "${FFTW_LIB_DIR}/libfftw3-3.dll.a"
        ${APP_RESOURCES}
)

# ─── Post-build ───────────────────────────────────────────────────────────────
add_custom_command(TARGET Stand POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LIMESUITE_ROOT}/bin/LimeSuite.dll"
        "$<TARGET_FILE_DIR:Stand>"
        COMMENT "Copying LimeSuite.dll"
)

add_custom_command(TARGET Stand POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${FFTW_BIN_DIR}/libfftw3-3.dll"
        "$<TARGET_FILE_DIR:Stand>/libfftw3-3.dll"
        COMMENT "Copying FFTW DLL"
)

add_custom_command(TARGET Stand POST_BUILD
        COMMAND "C:/Qt/6.10.0/mingw_64/bin/windeployqt.exe"
        --no-translations
        --no-opengl-sw
        "$<TARGET_FILE:Stand>"
        COMMENT "Running windeployqt"
)
