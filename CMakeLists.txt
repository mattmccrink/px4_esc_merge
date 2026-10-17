# In-tree entry point.
#
# This repo is mounted as a git submodule of the Compressor PX4 fork at
#     src/modules/esc_status_merge
# and enabled by CONFIG_MODULES_ESC_STATUS_MERGE (see Kconfig). PX4 calls
# add_subdirectory() on this directory; we forward to the module sources.
#
# (Out-of-tree builds via EXTERNAL_MODULES_LOCATION use src/CMakeLists.txt
# instead and never read this file.)
add_subdirectory(src/modules/esc_status_merge)
