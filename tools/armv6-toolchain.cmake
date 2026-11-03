# Pi Zero 1 / Pi 1 cross build: ARMv6 (ARM1176JZF-S), hard-float VFP.
# release.sh passes -DLV_TUNE=arm1176jzf-s which adds -mcpu=arm1176jzf-s;
# the flags here pin the ABI floor so the codegen (and release.sh's floor
# scan) matches the oldest supported board. No NEON: minimp3 keeps its plain
# scalar decoder on this board (see CMakeLists LV_TUNE block).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_C_FLAGS_INIT "-march=armv6 -mfpu=vfp -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-march=armv6 -mfpu=vfp -mfloat-abi=hard")
