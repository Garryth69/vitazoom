cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(vitazoom C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -O2 -std=gnu99 -fno-builtin")

add_executable(vitazoom
  src/main.c
  src/zoom.c
  src/levels.c
  src/hooks.c
  src/gesture.c
  src/menu.c
  src/draw.c
  src/config.c
  src/font.c
  src/font_big.c
)

target_link_libraries(vitazoom
  taihenForKernel_stub
  SceDisplayForDriver_stub
  SceCtrlForDriver_stub
  SceTouchForDriver_stub
  SceAppMgrForDriver_stub
  SceIofilemgrForDriver_stub
  SceSysclibForDriver_stub
  SceSysmemForDriver_stub
  SceThreadmgrForDriver_stub
  SceSysrootForDriver_stub
  SceSysrootForKernel_stub_weak
  gcc
)

set_target_properties(vitazoom PROPERTIES LINK_FLAGS "-nostdlib")

vita_create_self(vitazoom.skprx vitazoom CONFIG exports.yml UNSAFE)
