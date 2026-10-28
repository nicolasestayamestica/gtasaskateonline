cmake_minimum_required(VERSION 3.15)
project(gtskate_net LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# CRT estatico: el .asi no depende de ningun redistribuible de Visual C++
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(MH ${CMAKE_SOURCE_DIR}/third_party/minhook)
if(NOT EXISTS ${MH}/include/MinHook.h)
  message(FATAL_ERROR "Falta third_party/minhook (el workflow lo clona solo).")
endif()

add_library(gtskate_net SHARED
  src/main.cpp
  ${MH}/src/buffer.c
  ${MH}/src/hook.c
  ${MH}/src/trampoline.c
  ${MH}/src/hde/hde32.c
)
target_include_directories(gtskate_net PRIVATE ${MH}/include ${MH}/src)
target_compile_definitions(gtskate_net PRIVATE _CRT_SECURE_NO_WARNINGS)
set_target_properties(gtskate_net PROPERTIES PREFIX "" SUFFIX ".asi" OUTPUT_NAME "gtskate_net")
