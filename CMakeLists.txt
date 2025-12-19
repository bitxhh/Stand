cmake_minimum_required(VERSION 3.16)
project(Stand)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

#-------------------QT----------------------
list(APPEND CMAKE_PREFIX_PATH "C:/Qt/6.10.0/mingw_64")
find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent PrintSupport)

#-------------------LimeSuite----------------
set(LIMESUITE_ROOT "C:/LimeSuite")
include_directories(${LIMESUITE_ROOT}/include)
link_directories(${LIMESUITE_ROOT}/lib)

#------------------FFTW(double)----------------------
set(FFTW_ROOT "${CMAKE_SOURCE_DIR}/external/FFTW")
set(FFTW_INCLUDE_DIR "${FFTW_ROOT}/include")
set(FFTW_LIB_DIR     "${FFTW_ROOT}/lib")
set(FFTW_BIN_DIR     "${FFTW_ROOT}/bin")

#--------------------QCustomPlot-----------------------
set(QCUSTOMPLOT_DIR "${CMAKE_SOURCE_DIR}/external/qcustomplot")
add_library(qcustomplot STATIC
        ${QCUSTOMPLOT_DIR}/qcustomplot.cpp
        ${QCUSTOMPLOT_DIR}/qcustomplot.h
)
target_include_directories(qcustomplot PUBLIC "${QCUSTOMPLOT_DIR}")
target_link_libraries(qcustomplot PUBLIC Qt6::Widgets Qt6::PrintSupport)

# Fix MinGW "too many sections / file too big" for qcustomplot.cpp
if (MINGW)
    target_compile_options(qcustomplot PRIVATE -Wa,-mbig-obj -g0)
endif()


add_executable(Stand
        Application/Application.cpp
        Application/Application.h
        main.cpp
        LimeCore/LimeManager.cpp
        LimeCore/LimeManager.h
        LimeCore/Device.cpp
        LimeCore/Device.h
        resources.qrc
)
target_include_directories(Stand PRIVATE "${FFTW_INCLUDE_DIR}")

target_link_libraries(Stand
        PRIVATE
        Qt6::Widgets
        Qt6::Concurrent
        Qt6::PrintSupport

        qcustomplot

        LimeSuite.lib
        "${FFTW_LIB_DIR}/libfftw3-3.dll.a"
        ${APP_RESOURCES}
)

add_custom_command(TARGET Stand POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LIMESUITE_ROOT}/bin/LimeSuite.dll"
        $<TARGET_FILE_DIR:Stand>
        COMMENT "Copying LimeSuite.dll to build directory"
)
add_custom_command(TARGET Stand POST_BUILD
        COMMAND "C:/Qt/6.10.0/mingw_64/bin/windeployqt.exe"
        --no-translations
        --no-opengl-sw
        "$<TARGET_FILE:Stand>"
        COMMENT "Запуск windeployqt для Stand"
)

add_custom_command(TARGET Stand POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${FFTW_BIN_DIR}/libfftw3-3.dll"
        "$<TARGET_FILE_DIR:Stand>/libfftw3-3.dll"
        COMMENT "Copying FFTW DLL"
)