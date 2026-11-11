# Native Wii U (wut) build of nullDC4Wii. The core keeps its Wii code paths (_WII, HOST_OS == OS_WII,
# same PowerPC JIT); libogc calls are supplied by wiiu/compat and the Wii-only frontend is replaced by
# wiiu/main_wiiu.cpp. Build: wiiu/build-wiiu.sh (see wiiu/Dockerfile), or powerpc-eabi-cmake -S wiiu -B build-wiiu -G Ninja
# && ninja -C build-wiiu
cmake_minimum_required(VERSION 3.13)
project(nulldc_wiiu C CXX)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(CORE_DIRS
    . cl config dc dc/aica dc/asic dc/gdrom dc/maple dc/mem dc/pvr dc/sh4 dc/sh4/rec_v2
    plugins plugs/nullExtDev plugs/drkMapleDevices plugs/nullAICA plugs/vbaARM plugs/ImgReader
    plugs/drkPvr wii/dc/sh4/rec_v2)
set(SOURCES)
foreach(dir ${CORE_DIRS})
    file(GLOB found ${ROOT}/${dir}/*.cpp ${ROOT}/${dir}/*.c)
    list(APPEND SOURCES ${found})
endforeach()
# superseded by aica_mem.cpp (the Wii Makefile filters it out too), and the GX renderer is replaced
list(FILTER SOURCES EXCLUDE REGEX "/plugs/nullAICA/mem\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX "/plugs/drkPvr/gxRend\\.cpp$")
file(GLOB WIIU_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
file(GLOB PICKER_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/picker/*.c)   # the picker's logic (picker_core.c)
# portable parts of the Wii frontend
list(APPEND WIIU_SOURCES ${ROOT}/wii/user_controls.cpp)

# precompiled GX2 shaders (wiiu/shaders/build.sh regenerates the .gsh files with CafeGLSL), embedded as arrays
set(SHADER_SOURCES)
foreach(name blit geo)
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${name}.gsh hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${name}_gsh.c
         "#include <stdint.h>\nconst uint8_t ${name}_gsh[] __attribute__((aligned(256))) = {${bytes}};\n")
    list(APPEND SHADER_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/${name}_gsh.c)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${name}.gsh)
endforeach()

add_executable(nulldc ${SOURCES} ${WIIU_SOURCES} ${PICKER_SOURCES} ${SHADER_SOURCES})
target_include_directories(nulldc BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat ${ROOT} ${ROOT}/wii ${ROOT}/plugs/drkPvr)
target_compile_definitions(nulldc PRIVATE _WII _PPC RELEASE)
set(COMMON_FLAGS -O2 -fno-strict-aliasing -fsingle-precision-constant -fomit-frame-pointer -ffast-math
    -Wno-unused-parameter -Wno-missing-field-initializers -Wno-write-strings -Wno-narrowing)
target_compile_options(nulldc PRIVATE ${COMMON_FLAGS} $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fpermissive>)
target_link_libraries(nulldc m)
wut_create_rpx(nulldc)
# the license files, bundled into the wuhb (wiiu/make-notices.sh fails the configure if a text is missing)
execute_process(COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/make-notices.sh ${CMAKE_CURRENT_BINARY_DIR}/content RESULT_VARIABLE notices_rv)
if(NOT notices_rv EQUAL 0)
    message(FATAL_ERROR "wiiu/make-notices.sh failed")
endif()
# icon.png: the Homebrew Channel banner (apps/apps/nulldc4wii/icon.png) remade at 128x128 by make_icon.py
wut_create_wuhb(nulldc NAME "nullDC" SHORTNAME "nullDC" AUTHOR "nullDC4Wii, Wii U build" CONTENT "${CMAKE_CURRENT_BINARY_DIR}/content"
                ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.png")
