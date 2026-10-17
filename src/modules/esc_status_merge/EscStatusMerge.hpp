/****************************************************************************
 *
 * esc_status_merge — out-of-tree PX4 module
 *
 * Merges every esc_status instance (e.g. DShot + DroneCAN) into a single,
 * motor-ordered esc_status_merged topic. Slots are assigned by
 * esc_report.actuator_function (Motor1 -> slot 0, ...).
 *
 * Targets PX4 v1.17.0 (EscStatus: CONNECTED_ESC_MAX = 8, uint8 flag fields).
 *
 ****************************************************************************/

#pragma once

#include <px4_platform_common/module.h>
#include <px4_platform_common/px4_work_queue/ScheduledWorkItem.hpp>
#include <lib/perf/perf_counter.h>
#include <uORB/Publication.hpp>
#include <uORB/SubscriptionMultiArray.hpp>
#include <uORB/topics/esc_status.h>

using namespace time_literals;

class EscStatusMerge : public ModuleBase<EscStatusMerge>, public px4::ScheduledWorkItem
{
public:
	EscStatusMerge();
	~EscStatusMerge() override;

	static int task_spawn(int argc, char *argv[]);
	static int custom_command(int argc, char *argv[]);
	static int print_usage(const char *reason = nullptr);

	bool init();
	int print_status() override;

private:
	void Run() override;

	static constexpr int kMotor1Function = 101;   // OutputFunction::Motor1
	static constexpr int kMax = esc_status_s::CONNECTED_ESC_MAX;

	// Whole-source timeout: matches commander's ESC telemetry timeout (escCheck.cpp, 700 ms),
	// which is sized to ride through DShot ESCs' ~550 ms init blackout.
	static constexpr hrt_abstime kSourceTimeout = 700_ms;

	// Per-ESC report timeout: matches the DroneCAN ESC driver's own online window
	// (uavcan/actuators/esc.cpp, 1200 ms). Needed because the DShot UART-telemetry
	// path forces all online flags true; the per-report timestamp is the only
	// real per-ESC liveness signal there.
	static constexpr hrt_abstime kReportTimeout = 1200_ms;

	static bool is_stale(hrt_abstime ts, hrt_abstime now, hrt_abstime timeout)
	{
		return (ts == 0) || (now > ts + timeout);   // overflow-safe vs. ts slightly newer than now
	}

	uORB::SubscriptionMultiArray<esc_status_s> _esc_status_subs{ORB_ID::esc_status};
	uORB::Publication<esc_status_s> _merged_pub{ORB_ID(esc_status_merged)};

	esc_status_s _merged{};
	uint16_t _counter{0};
	uint32_t _seen_mask{0};       // motor slots ever reported (sticky)
	uint32_t _conflict_mask{0};   // motor slots claimed by >1 source (sticky)

	perf_counter_t _cycle_perf{perf_alloc(PC_ELAPSED, MODULE_NAME": cycle")};
};
