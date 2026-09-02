cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(minecraft_javac C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Minecraft: JavaC Edition")
set(VITA_TITLEID  "MCJVC0001")
set(VITA_VERSION  "01.00")

add_executable(${PROJECT_NAME}
  src/main.c
  src/chunk.c
  src/atlas.c
  src/player.c
  src/raycast.c
  src/renderer.c
)

set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

target_include_directories(${PROJECT_NAME} PRIVATE src include)

target_link_libraries(${PROJECT_NAME}
  vitaGL vitashark SceShaccCg_stub SceShaccCgExt
  taihen_stub SceKernelDmacMgr_stub SceGxm_stub
  SceDisplay_stub SceCtrl_stub SceAppMgr_stub
  SceSysmodule_stub SceCommonDialog_stub SceLibKernel_stub
  mathneon m stdc++
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
)
