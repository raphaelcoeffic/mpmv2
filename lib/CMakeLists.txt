# Set devices directory for linker scripts
set(TI_DEVICE_DIR "${CMAKE_CURRENT_LIST_DIR}/ti/devices" PARENT_SCOPE)

# Import TI devices: CC26x0 for the XJT-Lite (CC2650), CC13x2/CC26x2 for the
# others
if (BOARD STREQUAL "XJT_LITE")
  add_subdirectory(ti/devices/cc26x0)
else()
  add_subdirectory(ti/devices/cc13x2_cc26x2)
endif()

# Segger RTT
add_subdirectory(SeggerRTT)

# Littlefs
add_library(littlefs INTERFACE)

set(LFS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/littlefs)

target_include_directories(littlefs
  INTERFACE
  ${LFS_DIR}
)

target_sources(littlefs
  INTERFACE
  ${LFS_DIR}/lfs.c
  ${LFS_DIR}/lfs_util.c  
)

# Buffers given in the configurations (lfs_file_opencfg())
target_compile_definitions(littlefs
  INTERFACE
  LFS_NO_MALLOC
)
