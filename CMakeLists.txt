cmake_minimum_required(VERSION 3.20)
project(SecureDrive VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# 全局使用静态 CRT（/MT），使所有目标（包括 FetchContent 的 GLFW）都不依赖外部运行时
# 这样编译出的 exe 可以拷贝到任意 Windows 电脑直接运行
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "" FORCE)
endif()

# ============================================================
# 依赖：Dear ImGui + GLFW（自动下载，静态编译）
# ============================================================
include(FetchContent)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.6
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# ============================================================
# 源文件列表
# ============================================================
set(SOURCES
    src/main.cpp
    # Crypto
    src/crypto/sha256.cpp
    src/crypto/aes.cpp
    src/crypto/aes_xts.cpp
    src/crypto/blake2b.cpp
    src/crypto/argon2id.cpp
    src/crypto/random.cpp
    # Disk
    src/disk/disk.cpp
    # Volume
    src/volume/volume.cpp
    # VFS
    src/vfs/vfs.cpp
    # NTFS
    src/ntfs/ntfs_reader.cpp
    # Security
    src/security/lockout.cpp
    # UI
    src/ui/app.cpp
    src/ui/init_wizard.cpp
)

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

set(SDRV_CONSOLE OFF CACHE BOOL "Enable console window for debug logging" FORCE)

if(WIN32 AND NOT SDRV_CONSOLE)
    set(SUBSYSTEM_FLAG "WIN32")
else()
    set(SUBSYSTEM_FLAG "")
endif()

# 平台资源文件（只在 Windows 时添加）
if(WIN32)
    list(APPEND SOURCES platform/windows/manifest.rc)
endif()

add_executable(SecureDrive ${SUBSYSTEM_FLAG} ${SOURCES} ${IMGUI_SOURCES})

# ============================================================
# 头文件搜索路径
# ============================================================
target_include_directories(SecureDrive PRIVATE
    src/
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

if(WIN32)
    target_include_directories(SecureDrive PRIVATE platform/windows)
endif()

# ============================================================
# 平台相关链接与编译选项
# ============================================================
if(WIN32)
    target_link_libraries(SecureDrive PRIVATE glfw opengl32 comdlg32)
    # 请求管理员权限 (UAC)
    if(MSVC)
        set_target_properties(SecureDrive PROPERTIES
            LINK_FLAGS "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\" /ENTRY:mainCRTStartup"
        )
    elseif(MINGW)
        # RC resource (icon + UAC manifest) 已通过 WIN32 可执行属性全局包含
    endif()
    target_compile_options(SecureDrive PRIVATE /utf-8)
    target_compile_definitions(SecureDrive PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )

elseif(APPLE)
    find_library(OPENGL_LIB  OpenGL REQUIRED)
    find_library(COCOA_LIB   Cocoa  REQUIRED)
    find_library(IOKIT_LIB   IOKit  REQUIRED)
    find_library(COREVIDEO_LIB CoreVideo REQUIRED)
    target_link_libraries(SecureDrive PRIVATE
        glfw
        ${OPENGL_LIB} ${COCOA_LIB} ${IOKIT_LIB} ${COREVIDEO_LIB}
    )
    # 屏蔽 macOS OpenGL 弃用警告
    target_compile_options(SecureDrive PRIVATE
        -Wno-deprecated-declarations
    )
    # macOS 需要 root 权限读写裸设备，可通过 sudo 或 authorizationRef 实现
    set_target_properties(SecureDrive PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/platform/macos/Info.plist
    )

else()  # Linux
    find_package(OpenGL REQUIRED)
    target_link_libraries(SecureDrive PRIVATE
        glfw
        OpenGL::GL
        dl pthread
    )
endif()

# ============================================================
# 编译优化
# ============================================================
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        target_compile_options(SecureDrive PRIVATE /O2)
    else()
        target_compile_options(SecureDrive PRIVATE -O2)
    endif()
endif()

message(STATUS "SecureDrive build configured for: ${CMAKE_SYSTEM_NAME}")
