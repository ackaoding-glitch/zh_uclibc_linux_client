set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(DEFINED TOOLCHAIN_ROOT)
    set(TOOLCHAIN_ROOT "${TOOLCHAIN_ROOT}" CACHE PATH "RV1106 cross toolchain root" FORCE)
elseif(DEFINED ENV{TOOLCHAIN_ROOT})
    set(TOOLCHAIN_ROOT "$ENV{TOOLCHAIN_ROOT}" CACHE PATH "RV1106 cross toolchain root" FORCE)
endif()

set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES TOOLCHAIN_ROOT)

if(NOT TOOLCHAIN_ROOT)
    message(FATAL_ERROR "TOOLCHAIN_ROOT not set. Use -DTOOLCHAIN_ROOT=/path/to/rv1106/toolchain or export TOOLCHAIN_ROOT.")
endif()

set(_cross_gcc "${TOOLCHAIN_ROOT}/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc-8.3.0")
if(NOT EXISTS "${_cross_gcc}")
    message(FATAL_ERROR "RV1106 cross compiler not found at ${_cross_gcc}")
endif()

set(CMAKE_C_COMPILER "${_cross_gcc}" CACHE FILEPATH "RV1106 C compiler" FORCE)
