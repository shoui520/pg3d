cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to a softfp VitaSDK.")
  endif()
endif()

project(pg3d_vita C CXX ASM)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Pixel Gun 3D")
set(VITA_TITLEID  "PGUN00001")
set(VITA_VERSION  "01.00")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

option(PG3D_DEBUG "Verbose loader logging" ON)

set(COMMON_FLAGS "-mfloat-abi=softfp -mfpu=neon -mcpu=cortex-a9 -Wall -Wno-unused-function -fno-optimize-sibling-calls -funwind-tables")
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} ${COMMON_FLAGS} -O2 -std=gnu11")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_FLAGS} -O2 -std=gnu++17 -fno-rtti")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-q -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=memalign,--wrap=free -Wl,--wrap=_malloc_r,--wrap=_calloc_r,--wrap=_realloc_r,--wrap=_memalign_r,--wrap=_free_r -Wl,--wrap=_open_r,--wrap=_read_r,--wrap=_lseek_r,--wrap=_fstat_r,--wrap=_close_r")

if(PG3D_DEBUG)
  add_definitions(-DPG3D_DEBUG=1)
endif()

include_directories(source)

file(GLOB_RECURSE PG3D_SOURCES
  source/*.c
  source/*.cpp
  source/*.S
)

add_executable(pg3d ${PG3D_SOURCES})

target_link_libraries(pg3d
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  vitaGL
  vitashark
  SceShaccCgExt
  mathneon
  OpenSLES
  z
  stdc++
  m
  taihen_stub
  kubridge_stub
  SceShaccCg_stub
  SceKernelDmacMgr_stub
  SceGxm_stub
  SceDisplay_stub
  SceCtrl_stub
  SceTouch_stub
  SceMotion_stub
  SceAudio_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceCommonDialog_stub
  SceSysmodule_stub
  SceNet_stub
  SceNetCtl_stub
  ScePower_stub
  SceRtc_stub
  SceLibKernel_stub
  SceKernelThreadMgr_stub
  SceIofilemgr_stub
)

vita_create_self(eboot.bin pg3d SAFE)
vita_create_vpk(pg3d.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
       sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
       sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
       sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
set_source_files_properties(source/bionic/imports.c PROPERTIES COMPILE_FLAGS -fno-builtin)
