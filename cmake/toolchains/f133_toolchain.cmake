set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

set(F133_TOOLCHAIN_ROOT "/home/yuwei/samba/yuwei_work/chip_sdk/Allwinner/D1x/Tina-Linux/prebuilt/gcc/linux-x86/riscv/toolchain-thead-glibc/riscv64-glibc-gcc-thead_20200702" CACHE PATH "F133 RISC-V toolchain root")
set(F133_TOOLCHAIN_PREFIX "riscv64-unknown-linux-gnu" CACHE STRING "F133 toolchain executable prefix")
set(F133_TOOLCHAIN_BIN "${F133_TOOLCHAIN_ROOT}/bin")

set(CMAKE_C_COMPILER "${F133_TOOLCHAIN_BIN}/${F133_TOOLCHAIN_PREFIX}-gcc")
set(CMAKE_CXX_COMPILER "${F133_TOOLCHAIN_BIN}/${F133_TOOLCHAIN_PREFIX}-g++")
set(CMAKE_ASM_COMPILER "${F133_TOOLCHAIN_BIN}/${F133_TOOLCHAIN_PREFIX}-gcc")

execute_process(
    COMMAND "${CMAKE_C_COMPILER}" --print-sysroot
    OUTPUT_VARIABLE F133_SYSROOT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(F133_SYSROOT)
    set(CMAKE_SYSROOT "${F133_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH "${F133_SYSROOT}")
endif()

set(CMAKE_C_FLAGS_INIT "-march=rv64imafdcxthead -mabi=lp64d")
set(CMAKE_CXX_FLAGS_INIT "-march=rv64imafdcxthead -mabi=lp64d")
set(CMAKE_ASM_FLAGS_INIT "-march=rv64imafdcxthead -mabi=lp64d")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
