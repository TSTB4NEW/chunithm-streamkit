cmake_minimum_required(VERSION 3.16)
project(chunithm_streamkit LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)

# —— ASI 插件（Win32 优先；CHUNITHM Mate 2.50 的 chusanApp.exe 是 32 位）——
add_library(chunithm_streamkit SHARED
    src/main.cpp
    src/app.cpp
    src/httpd.cpp
)
target_include_directories(chunithm_streamkit PUBLIC src)
set_target_properties(chunithm_streamkit PROPERTIES
    PREFIX ""
    SUFFIX ".asi"
    OUTPUT_NAME "chunithm_streamkit"
)
target_link_libraries(chunithm_streamkit ws2_32)

# —— 测试宿主（可选构建，纯 C，模拟第三方进程加载 ASI）——
add_executable(test_host tests/host/main.c)
target_link_libraries(test_host ws2_32)

# 安装辅助：把 web/ 与示例配置拷到输出目录
add_custom_command(TARGET chunithm_streamkit POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/web $<TARGET_FILE_DIR:chunithm_streamkit>/web
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/config/chunithm_streamkit.ini.example
        $<TARGET_FILE_DIR:chunithm_streamkit>/chunithm_streamkit.ini.example
)
