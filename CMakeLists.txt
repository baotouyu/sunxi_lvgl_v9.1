cmake_minimum_required(VERSION 3.10)
project(lvgl)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 99)#C99 # lvgl officially support C99 and above
set(CMAKE_CXX_STANDARD 17)#C17
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)

add_subdirectory(lvgl)
target_include_directories(lvgl PUBLIC ${PROJECT_SOURCE_DIR})

add_executable(main main.c mouse_cursor_icon.c)

set(F133_PORT_DIR "${PROJECT_SOURCE_DIR}/f133_port"
    CACHE PATH "Local F133 port directory that provides headers and libraries")

if(NOT EXISTS "${F133_PORT_DIR}/include/ion_mem_alloc.h")
    message(FATAL_ERROR "Missing ${F133_PORT_DIR}/include/ion_mem_alloc.h")
endif()

if(NOT EXISTS "${F133_PORT_DIR}/lib/libuapi.so")
    message(FATAL_ERROR "Missing ${F133_PORT_DIR}/lib/libuapi.so")
endif()

if(NOT EXISTS "${F133_PORT_DIR}/include/g2d_driver_enh.h")
    message(FATAL_ERROR "Missing ${F133_PORT_DIR}/include/g2d_driver_enh.h")
endif()

add_library(f133_port STATIC
    f133_port/src/f133_ion.c
    f133_port/src/f133_g2d.c
    f133_port/src/f133_display.c
    f133_port/src/f133_touch.c
)
target_include_directories(f133_port PUBLIC "${F133_PORT_DIR}/include")
target_link_libraries(f133_port PUBLIC lvgl "${F133_PORT_DIR}/lib/libuapi.so")

target_include_directories(main PRIVATE "${F133_PORT_DIR}/include")
target_link_libraries(main f133_port)
set_target_properties(main PROPERTIES SKIP_BUILD_RPATH TRUE)

target_link_libraries(main lvgl lvgl::examples lvgl::demos lvgl::thorvg ${SDL2_LIBRARIES} m pthread)
add_custom_target (run COMMAND ${EXECUTABLE_OUTPUT_PATH}/main DEPENDS main)
