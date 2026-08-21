cmake_minimum_required(VERSION 3.20)
project(cpp_app_launcher LANGUAGES CXX RC)

add_library(AppLauncher STATIC
    AppLauncher.cpp
    SimpleJson.cpp
)

target_include_directories(AppLauncher PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(AppLauncher PUBLIC cxx_std_17)

target_link_libraries(AppLauncher PUBLIC
    comctl32
    ole32
    shell32
)

add_executable(AppLauncherExample WIN32
    ExampleUsage.cpp
    MyApp.rc
)

target_link_libraries(AppLauncherExample PRIVATE AppLauncher)
