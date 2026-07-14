cmake_minimum_required(VERSION 3.16)
project(bk0010-emulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGLWidgets)
# Multimedia is optional (used only for sound, Stage 3). Build works without it.
find_package(Qt6 QUIET COMPONENTS Multimedia)

# --- Core emulation library (no Qt dependency) ---
add_library(bkcore STATIC
    src/core/Memory.cpp
    src/core/Cpu.cpp
    src/core/Disasm.cpp
    src/core/Screen.cpp
    src/core/Speaker.cpp
    src/core/Board.cpp
)
target_include_directories(bkcore PUBLIC src/core)

# --- GUI application ---
set(APP_SOURCES
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/GlScreen.cpp
    src/ui/DebuggerOverlay.cpp
    src/ui/MemVisWidget.cpp
    src/ui/HotPathWidget.cpp
    src/ui/CallGraphWidget.cpp
    src/ui/HotChartWidget.cpp
    src/ui/AudioOut.cpp
    src/ui/BkKeymap.cpp
    src/mcp/McpServer.cpp
)

qt_add_executable(bk0010-emulator ${APP_SOURCES})
target_link_libraries(bk0010-emulator PRIVATE
    bkcore
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGLWidgets
)

if(Qt6Multimedia_FOUND)
    target_compile_definitions(bk0010-emulator PRIVATE HAVE_QT_MULTIMEDIA=1)
    target_link_libraries(bk0010-emulator PRIVATE Qt6::Multimedia)
    message(STATUS "Qt6 Multimedia found: sound enabled")
else()
    message(STATUS "Qt6 Multimedia NOT found: sound disabled (install qt6-qtmultimedia-devel)")
endif()

# ROM path passed to the app so it can find roms/ next to the source tree by default
target_compile_definitions(bk0010-emulator PRIVATE
    BK_DEFAULT_ROM_DIR="${CMAKE_CURRENT_SOURCE_DIR}/roms"
)

# --- Unit tests for the CPU core ---
enable_testing()
add_executable(cpu_tests tests/cpu_tests.cpp)
target_link_libraries(cpu_tests PRIVATE bkcore)
add_test(NAME cpu_tests COMMAND cpu_tests)
