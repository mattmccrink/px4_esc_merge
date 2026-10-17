# Out-of-tree module list, consumed by PX4's root CMakeLists.txt when built with
#   make <target> EXTERNAL_MODULES_LOCATION=<path to this repo>
# Must live in src/ (not the repo root).
set(config_module_list_external
	modules/esc_status_merge
	PARENT_SCOPE
)
