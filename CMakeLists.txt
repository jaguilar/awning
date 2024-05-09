cmake_minimum_required(VERSION 3.21)

if (EXISTS ${CMAKE_CURRENT_LIST_DIR}/secrets.cmake)
  include(secrets.cmake)
endif()

include(FetchContent)

FetchContent_Declare(
  picobase
  GIT_REPOSITORY https://github.com/jaguilar/picobase.git
  GIT_TAG        main
  EXCLUDE_FROM_ALL
)
FetchContent_Declare(
  somfy
  GIT_REPOSITORY https://github.com/Legion2/Somfy_Remote_Lib.git
  GIT_TAG        main
  EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(picobase)
include(${picobase_SOURCE_DIR}/basic_imports.cmake)

FetchContent_Declare(
  jagspico
  GIT_REPOSITORY https://github.com/jaguilar/jagspico.git
  GIT_TAG        main
  EXCLUDE_FROM_ALL
)

project(awning VERSION 0.1.0 LANGUAGES C CXX ASM)    

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(PICO_BOARD "pico_w")

pico_sdk_init()

add_compile_options(-ffunction-sections -fdata-sections -Wl,--gc-sections)

FetchContent_MakeAvailable(jagspico somfy)

file(GLOB somfy_srcs "${somfy_SOURCE_DIR}/src/*.cpp")
add_library(somfy ${somfy_srcs})
target_include_directories(somfy PUBLIC ${somfy_SOURCE_DIR}/src)
target_link_libraries(somfy PUBLIC jagsarduino stdc++)
target_compile_features(somfy PUBLIC cxx_std_23)
target_compile_options(somfy PUBLIC -DDEBUG=1 -DARDUINO_ARCH_AVR=1)

add_subdirectory(src)
