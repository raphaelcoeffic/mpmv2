# arm-none-eabi toolchain
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_CXX_STANDARD 17)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(MINGW OR WIN32)
  set(EXE_SUFFIX ".exe")
  set(CMAKE_OBJECT_PATH_MAX 200)
endif()

if(ARM_TOOLCHAIN_DIR)
  cmake_path(SET ARM_TOOLCHAIN_DIR NORMALIZE ${ARM_TOOLCHAIN_DIR})
  set(ARM_TOOLCHAIN_DIR "${ARM_TOOLCHAIN_DIR}/")
endif()

set(CMAKE_AR           ${ARM_TOOLCHAIN_DIR}arm-none-eabi-ar${EXE_SUFFIX})
set(CMAKE_ASM_COMPILER ${ARM_TOOLCHAIN_DIR}arm-none-eabi-gcc${EXE_SUFFIX})
set(CMAKE_C_COMPILER   ${ARM_TOOLCHAIN_DIR}arm-none-eabi-gcc${EXE_SUFFIX})
set(CMAKE_CXX_COMPILER ${ARM_TOOLCHAIN_DIR}arm-none-eabi-g++${EXE_SUFFIX})
set(CMAKE_LINKER       ${ARM_TOOLCHAIN_DIR}arm-none-eabi-ld${EXE_SUFFIX})
set(CMAKE_OBJCOPY      ${ARM_TOOLCHAIN_DIR}arm-none-eabi-objcopy${EXE_SUFFIX} CACHE INTERNAL "")
set(CMAKE_RANLIB       ${ARM_TOOLCHAIN_DIR}arm-none-eabi-ranlib${EXE_SUFFIX} CACHE INTERNAL "")
set(CMAKE_SIZE_UTIL    ${ARM_TOOLCHAIN_DIR}arm-none-eabi-size${EXE_SUFFIX} CACHE INTERNAL "")
set(CMAKE_STRIP        ${ARM_TOOLCHAIN_DIR}arm-none-eabi-strip${EXE_SUFFIX} CACHE INTERNAL "")
set(CMAKE_GCOV         ${ARM_TOOLCHAIN_DIR}arm-none-eabi-gcov${EXE_SUFFIX} CACHE INTERNAL "")

# Generate .elf files
set(CMAKE_EXECUTABLE_SUFFIX ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_CXX ".elf")

# Default C compiler flags
set(CMAKE_C_FLAGS_DEBUG_INIT "-g3 -Og -Wall -pedantic -DDEBUG")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG_INIT}" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELEASE_INIT "-O3 -Wall")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE_INIT}" CACHE STRING "" FORCE)

# Default C++ compiler flags
set(CMAKE_CXX_FLAGS_DEBUG_INIT "-g3 -Og -Wall -pedantic -DDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG_INIT}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O3 -Wall")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE_INIT}" CACHE STRING "" FORCE)

# customize linker command
set(CMAKE_EXE_LINKER_FLAGS "")
set(CMAKE_C_LINK_EXECUTABLE "<CMAKE_C_COMPILER> <FLAGS> <LINK_FLAGS> <OBJECTS>  -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_CXX_LINK_EXECUTABLE "<CMAKE_CXX_COMPILER> <FLAGS> <LINK_FLAGS> <OBJECTS>  -o <TARGET> <LINK_LIBRARIES>")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

if(NOT TARGET toolchain_gcc)
    add_library(toolchain_gcc INTERFACE)
    target_compile_options(
        toolchain_gcc
        INTERFACE
            $<$<COMPILE_LANGUAGE:C>:-std=c11>
            $<$<COMPILE_LANGUAGE:CXX>:-std=c++11>
            -mthumb -g
            -mabi=aapcs
            -Dgcc
            -ffunction-sections
            -fdata-sections
            -fmacro-prefix-map=${CMAKE_SOURCE_DIR}=.
    )
    target_link_options(
        toolchain_gcc
        INTERFACE
        # If linker-file property exists, add linker file
        $<$<NOT:$<STREQUAL:$<TARGET_PROPERTY:LINKER_FILE>,>>:-Wl,-T,$<TARGET_PROPERTY:LINKER_FILE>>
        # If map-file property exists, set map file
        $<$<NOT:$<STREQUAL:$<TARGET_PROPERTY:LINKER_MAP>,>>:-Wl,-Map,$<TARGET_PROPERTY:LINKER_MAP>>
        -specs=nano.specs
        # Disables 0x10000 sector allocation boundaries, which interfere
        # with the SPE layouts and prevent proper secure operation
        -Wl,--nmagic
        # Link only what is used
        -Wl,--gc-sections
    )

    add_library(toolchain_gcc_m4f INTERFACE)
    target_compile_options(toolchain_gcc_m4f INTERFACE -mcpu=cortex-m4 -mfloat-abi=hard -mfpu=fpv4-sp-d16)
    target_link_options(toolchain_gcc_m4f INTERFACE -mcpu=cortex-m4 -mfloat-abi=hard -mfpu=fpv4-sp-d16)
    target_link_libraries(toolchain_gcc_m4f INTERFACE toolchain_gcc)
endif()


