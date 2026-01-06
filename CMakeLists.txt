cmake_minimum_required(VERSION 3.15)
project(OpenDSD_GUI VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Lokalizacje instalacji użytkownika
if(UNIX AND NOT APPLE)
    include_directories(/usr/local/include)
    link_directories(/usr/local/lib)
elseif(WIN32)
    # W MSYS2 biblioteki lądują w /mingw64
    include_directories(/mingw64/include)
    link_directories(/mingw64/lib)
else()
    include_directories(/usr/local/include)
    link_directories(/usr/local/lib)
endif()

# --- Zależności: GLFW & ImGui (FetchContent) ---
include(FetchContent)

# 1. GLFW
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw
    GIT_TAG        3.3.8
)
FetchContent_MakeAvailable(glfw)

# 2. Dear ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui
    GIT_TAG        docking
)
FetchContent_MakeAvailable(imgui)

# --- Źródła ImGui ---
set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

# --- Aplikacja ---
add_executable(${PROJECT_NAME} src/main.cpp ${IMGUI_SOURCES})

# --- Include ---
target_include_directories(${PROJECT_NAME} PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    src
)

# --- Linkowanie ---
if(APPLE)
    # macOS
    target_link_libraries(${PROJECT_NAME} PRIVATE
        glfw
        "-framework OpenGL"
        "-framework Cocoa"
        "-framework IOKit"
        "-framework CoreAudio"
        "-framework AudioToolbox"
        dsdcc
        mbe
    )
elseif(WIN32)
    # Windows (MinGW)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        glfw
        opengl32
        dsdcc
        mbe
        ws2_32  # Wymagane dla dsdcc na Windows
        winmm   # Wymagane dla miniaudio
    )
    # USUNIĘTO: add_custom_command (kopiowanie DLL robimy w workflow)
elseif(UNIX AND NOT APPLE)
    # Linux
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        glfw
        GL
        dsdcc
        mbe
        Threads::Threads
        dl
    )
endif()