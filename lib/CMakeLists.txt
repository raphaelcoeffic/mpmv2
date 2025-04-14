# Set devices directory for linker scripts
set(TI_DEVICE_DIR "${CMAKE_CURRENT_LIST_DIR}/ti/devices" PARENT_SCOPE)

# Import TI devices
add_subdirectory(ti/devices/cc13x2_cc26x2)

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
