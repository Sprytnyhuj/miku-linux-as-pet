cmake_minimum_required(VERSION 3.20)
project(desktop-pet VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets DBus Network Test)
find_package(KF6Runner REQUIRED)    # wyszukiwarka: ten sam silnik co KRunner w Plasmie
find_package(KF6Service REQUIRED)   # KSycoca: cache usług budowany w głównym wątku przed runnerami

# Cała logika w bibliotece statycznej - korzystają z niej i program, i testy.
add_library(petcore STATIC
    src/config.cpp
    src/animlog.cpp
    src/sprites.cpp
    src/vdf.cpp
    src/steam.cpp
    src/mpris.cpp
    src/volume.cpp
    src/bubbles.cpp
    src/gpualert.cpp
    src/search.cpp
    src/searchhelper.cpp
    src/searchbubble.cpp
    src/claudechat.cpp
    src/pet.cpp
)
target_include_directories(petcore PUBLIC src)
target_link_libraries(petcore PUBLIC Qt6::Widgets Qt6::DBus Qt6::Network KF6::Runner KF6::Service)
target_compile_options(petcore PUBLIC -Wall -Wextra)
target_compile_definitions(petcore PUBLIC QT_NO_CAST_TO_ASCII QT_NO_CAST_FROM_BYTEARRAY)

add_executable(desktop-pet src/main.cpp)
target_link_libraries(desktop-pet PRIVATE petcore)

enable_testing()
add_executable(tests tests/tests.cpp)
target_link_libraries(tests PRIVATE petcore Qt6::Test)
add_test(NAME tests COMMAND tests)
set_tests_properties(tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
