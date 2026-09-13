cmake_minimum_required(VERSION 3.28)
project(obs-split-audio-mixer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

add_library(${PROJECT_NAME} MODULE src/plugin-main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs OBS::frontend-api Qt6::Core Qt6::Widgets)
target_compile_definitions(${PROJECT_NAME} PRIVATE QT_NO_KEYWORDS)
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "" OUTPUT_NAME "obs-split-audio-mixer")
