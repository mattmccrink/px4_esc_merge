/****************************************************************************
 *
 * esc_status_merge — out-of-tree PX4 module
 *
 ****************************************************************************/

#include "EscStatusMerge.hpp"

#include <inttypes.h>

EscStatusMerge::EscStatusMerge() :
	ModuleBase(),
	ScheduledWorkItem(MODULE_NAME, px4::wq_configurations::lp_default)
{
}

EscStatusMerge::~EscStatusMerge()
{
	perf_free(_cycle_perf);
}

bool EscStatusMerge::init()
{
	ScheduleOnInterval(20_ms); // 50 Hz, fixed: a silent driver is still detected as stale
	return true;
}

void EscStatusMerge::Run()
{
	if (should_exit()) {
		ScheduleClear();
		exit_and_cleanup();
		return;
	}

	perf_begin(_cycle_perf);

	const hrt_abstime now = hrt_absolute_time();

	esc_status_s merged{};
	hrt_abstime slot_time[kMax] {};
	uint32_t claimed = 0;
	uint32_t online = 0;
	uint32_t armed = 0;
	bool type_set = false;

	for (int i = 0; i < _esc_status_subs.size(); i++) {
		esc_status_s s;

		if (!_esc_status_subs[i].copy(&s) || is_stale(s.timestamp, now, kSourceTimeout)) {
			continue; // instance not advertised, or the whole source has gone quiet
		}

		if (!type_set) {
			// No "mixed" enum exists; report the first fresh source's type.
			merged.esc_connectiontype = s.esc_connectiontype;
			type_set = true;
		}

		// Scan the full array: works whether a driver packs reports compactly
		// or places them at the motor index. Flag bit j always refers to esc[j].
		for (int j = 0; j < kMax; j++) {
			const esc_report_s &r = s.esc[j];
			const int slot = static_cast<int>(r.actuator_function) - kMotor1Function;

			if (r.timestamp == 0 || slot < 0 || slot >= kMax) {
				continue; // empty entry, or not a motor
			}

			const uint32_t bit = 1u << slot;

			if (claimed & bit) {
				_conflict_mask |= bit;

				if (r.timestamp <= slot_time[slot]) {
					continue; // keep the fresher report
				}
			}

			claimed |= bit;
			slot_time[slot] = r.timestamp;
			merged.esc[slot] = r;

			const bool src_online = (s.esc_online_flags & (1u << j)) != 0;
			const bool src_armed  = (s.esc_armed_flags  & (1u << j)) != 0;

			online = (src_online && !is_stale(r.timestamp, now, kReportTimeout)) ? (online | bit) : (online & ~bit);
			armed  = src_armed ? (armed | bit) : (armed & ~bit);
		}
	}

	_seen_mask |= claimed;

	// Fail closed on "never seen anything": don't publish until at least one
	// motor has reported. With no publication, commander's ESC check still
	// raises "ESC telemetry missing" exactly as it would with stock PX4.
	if (_seen_mask == 0) {
		perf_end(_cycle_perf);
		return;
	}

	merged.esc_count = static_cast<uint8_t>(32 - __builtin_clz(_seen_mask));

	// Every slot below esc_count carries its motor function, so consumers that
	// name the motor (commander's ESC check) report it correctly even when the
	// slot is empty (source stale, or a gap in a non-contiguous motor map).
	// Empty slots are offline and unarmed.
	for (int k = 0; k < merged.esc_count; k++) {
		if (!(claimed & (1u << k))) {
			merged.esc[k] = esc_report_s{};
		}

		merged.esc[k].actuator_function = static_cast<uint8_t>(kMotor1Function + k);
	}

	merged.esc_online_flags = static_cast<decltype(merged.esc_online_flags)>(online);
	merged.esc_armed_flags  = static_cast<decltype(merged.esc_armed_flags)>(armed);
	merged.counter = _counter++;
	merged.timestamp = hrt_absolute_time();

	_merged_pub.publish(merged);
	_merged = merged;

	perf_end(_cycle_perf);
}

int EscStatusMerge::print_status()
{
	PX4_INFO("esc_count: %d  seen: 0x%02" PRIx32 "  online: 0x%02x  armed: 0x%02x  conflicts: 0x%02" PRIx32,
		 _merged.esc_count, _seen_mask, (unsigned)_merged.esc_online_flags,
		 (unsigned)_merged.esc_armed_flags, _conflict_mask);

	if (_seen_mask == 0) {
		PX4_INFO("no ESC reports seen yet; not publishing");
	}

	for (int k = 0; k < _merged.esc_count; k++) {
		const esc_report_s &r = _merged.esc[k];
		PX4_INFO("  Motor%-2d %s  rpm %6" PRId32 "  %6.2f V  %6.2f A  %6.1f C",
			 k + 1, (_merged.esc_online_flags & (1u << k)) ? "ONLINE " : "OFFLINE",
			 r.esc_rpm, (double)r.esc_voltage, (double)r.esc_current, (double)r.esc_temperature);
	}

	perf_print_counter(_cycle_perf);
	return 0;
}

int EscStatusMerge::task_spawn(int argc, char *argv[])
{
	EscStatusMerge *instance = new EscStatusMerge();

	if (instance) {
		_object.store(instance);
		_task_id = task_id_is_work_queue;

		if (instance->init()) {
			return PX4_OK;
		}

	} else {
		PX4_ERR("alloc failed");
	}

	delete instance;
	_object.store(nullptr);
	_task_id = -1;

	return PX4_ERROR;
}

int EscStatusMerge::custom_command(int argc, char *argv[])
{
	return print_usage("unknown command");
}

int EscStatusMerge::print_usage(const char *reason)
{
	if (reason) {
		PX4_WARN("%s\n", reason);
	}

	PRINT_MODULE_DESCRIPTION(
		R"DESCR_STR(
### Description
Merges all esc_status instances (e.g. DShot + DroneCAN) into a single
motor-ordered esc_status_merged topic. Slots are assigned by actuator_function.
Motors seen once stay in esc_count and are flagged offline (and unarmed) if
their source goes stale. Nothing is published until at least one ESC reports.
)DESCR_STR");

	PRINT_MODULE_USAGE_NAME("esc_status_merge", "system");
	PRINT_MODULE_USAGE_COMMAND("start");
	PRINT_MODULE_USAGE_DEFAULT_COMMANDS();

	return 0;
}

extern "C" __EXPORT int esc_status_merge_main(int argc, char *argv[])
{
	return EscStatusMerge::main(argc, argv);
}
