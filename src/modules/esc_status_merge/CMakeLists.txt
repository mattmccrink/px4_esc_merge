px4_add_module(
	MODULE modules__esc_status_merge
	MAIN esc_status_merge
	SRCS
		EscStatusMerge.cpp
		EscStatusMerge.hpp
	DEPENDS
		px4_work_queue
)
