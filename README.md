# px4_esc_merge

Out-of-tree PX4 module that merges every `esc_status` instance (e.g. DShot + DroneCAN) into one
motor-ordered `esc_status_merged` topic, and repoints PX4's ESC consumers (QGC ESC panel, commander
ESC arming check, failure detector, ESC battery, ESC-RPM dynamic notch) at it.

**Why.** Each ESC driver publishes its own `esc_status` instance, but in stock PX4 v1.17.0 every
consumer subscribes to instance 0 only (`FailureDetector.hpp` even carries a `// TODO: multi-instance`).
With mixed ESC protocols, whichever driver advertises first is the only one QGC displays and the only
one health-checked. The rest are silently ignored.

Targets **PX4 v1.17.0**. Two ways to build it:

1. **In-tree, as a submodule of the Compressor PX4 fork (recommended).** The fork carries the
   consumer repoint as ordinary commits; this repo mounts at `src/modules/esc_status_merge` and is
   enabled by `CONFIG_MODULES_ESC_STATUS_MERGE` (`make px4_fmu-v5_compressor`). The root
   `CMakeLists.txt` and `Kconfig` exist for this mode. `tools/patch_px4.py` is not used.
2. **Out-of-tree** via `EXTERNAL_MODULES_LOCATION` (standalone, original pattern): everything below.

---

## Layout

```
px4_esc_merge/
├── PX4-Autopilot/                      # git submodule, pinned to the v1.17.0 tag (you add this)
├── src/
│   ├── CMakeLists.txt                  # config_module_list_external
│   └── modules/esc_status_merge/
│       ├── CMakeLists.txt
│       ├── EscStatusMerge.hpp
│       └── EscStatusMerge.cpp
├── tools/patch_px4.py                  # idempotent, content-matched PX4-tree edits
└── sd/etc/extras.txt                   # startup line for the SD card
```

---

## One-time setup

From WSL2 (or any Linux shell) with the PX4 toolchain already installed:

```bash
unzip px4_esc_merge.zip && cd px4_esc_merge
git init && git add . && git commit -m "esc_status_merge scaffold"

git submodule add https://github.com/PX4/PX4-Autopilot.git PX4-Autopilot
cd PX4-Autopilot
git checkout v1.17.0                       # the TAG -- not main/beta
git submodule update --init --recursive
cd ..
git add .gitmodules PX4-Autopilot && git commit -m "Pin PX4 v1.17.0"
```

If you already have a v1.17.0 tree checked out, you can point the scripts at it instead
(`--px4 /path/to/PX4-Autopilot`, and the same path in the build command below).

---

## Build

```bash
cd px4_esc_merge
tools/patch_px4.py                         # re-run after ANY checkout/update of PX4-Autopilot
cd PX4-Autopilot
make px4_fmu-v5_default EXTERNAL_MODULES_LOCATION=$(realpath ..)
```

- The configure output should include `-- External modules: .../px4_esc_merge`.
- The firmware is at `build/px4_fmu-v5_default/px4_fmu-v5_default.px4`. From WSL2, the easiest path is
  to flash it through QGC on Windows (Vehicle Setup > Firmware > Advanced > Custom firmware file).
  `make px4_fmu-v5_default upload` works only if the board's USB is passed into WSL (usbipd).
- `EXTERNAL_MODULES_LOCATION` is cached in the build directory. If this target was previously built
  without it (or pointing elsewhere), `make distclean` first. Otherwise the old value sticks.
- `tools/patch_px4.py --check` verifies the tree without writing anything (exit 1 if unpatched).
  Run it before any flight build.

### What `patch_px4.py` changes in the PX4 tree

| File | Edit |
|---|---|
| `CMakeLists.txt` | Move the external-modules block after the in-tree module loop, so `DEPENDS px4_work_queue` resolves. Same edit as `px4_flow_angle`; detection is position-based, so both scripts can run on one tree. |
| `msg/EscStatus.msg` | Append `# TOPICS esc_status esc_status_merged` (same type, second topic name). |
| `mavlink/streams/ESC_INFO.hpp`, `ESC_STATUS.hpp` | `ORB_ID(esc_status)` → `ORB_ID(esc_status_merged)` |
| `commander/.../escCheck.hpp` | 〃 (`COM_ARM_CHK_ESCS`) |
| `commander/failure_detector/FailureDetector.hpp` | 〃 (`FD_ESCS_EN`, motor-failure detection) |
| `esc_battery/EscBattery.hpp` | 〃 |
| `sensors/vehicle_angular_velocity/VehicleAngularVelocity.hpp` | 〃 (ESC-RPM dynamic notch) |
| `logger/logged_topics.cpp` | Raw `esc_status` logged as multi-instance (was instance 0 only), plus `esc_status_merged`. |

Every edit is anchored on exact source text, and each consumer must contain exactly one
`ORB_ID(esc_status)`. If an anchor is missing (e.g. after a PX4 bump), the script **fails** rather than
skipping it, because a consumer left on raw `esc_status` quietly health-checks only one driver's motors.

---

## Run

Copy `sd/etc/extras.txt` to the SD card as `/etc/extras.txt`, or append its line if one already exists.

```
nsh> esc_status_merge status
INFO  [esc_status_merge] esc_count: 4  seen: 0x0f  online: 0x0f  armed: 0x0f  conflicts: 0x00
INFO  [esc_status_merge]   Motor1  ONLINE   rpm      0   44.23 V    0.01 A    23.0 C
...
nsh> listener esc_status_merged
```

Start order relative to the ESC drivers doesn't matter.

---

## Behavior

- **Slot assignment.** Each `esc_report` goes to slot `actuator_function − 101` (Motor1 → slot 0). The
  full source array is scanned, so it works whether a driver packs reports compactly or by motor index.
- **Sticky motors.** Once a slot has reported, it stays in `esc_count`. If its source goes stale, it
  stays visible but **offline and unarmed**. Losing a driver shows up as motors going offline, not
  motors disappearing.
- **Staleness.**
  - Whole source: 700 ms on the top-level timestamp. This matches commander's ESC telemetry timeout,
    which is sized for DShot ESCs' ~550 ms init blackout.
  - Per ESC: 1200 ms on the report timestamp. This matches the DroneCAN ESC driver's own online window.
    It matters for DShot: its UART-telemetry path forces *all* online flags true (`DShot.cpp`, "FIXME"),
    so the per-report timestamp is the only real per-ESC liveness signal.
- **Fails closed when nothing has been seen.** Nothing is published until at least one ESC reports.
  So with no telemetry (or the module not started), commander raises *"ESC telemetry missing"* exactly
  as stock PX4 would, if `COM_ARM_CHK_ESCS=1`.
- **Conflicts.** If two sources claim the same motor, the fresher report wins and the slot is latched
  in `conflicts`.
- **Rate.** 50 Hz on a fixed schedule (`lp_default` work queue), so a silent driver is still detected.
- **Connection type.** There is no "mixed" enum; the first fresh source's type is reported.

### Behavior changes vs. stock you should know about

- **The failure detector now sees all motors.** `FD_ESCS_EN` (default 1) flags any ESC not armed within
  300 ms, and **disarms during `COM_SPOOLUP_TIME`**. Previously it saw only instance 0. A DShot ESC whose
  telemetry stays stale for more than 1.2 s during spool-up will now (correctly) disarm the vehicle.
  After spool-up it's a latched health failure, not a disarm.
- **The ESC-RPM dynamic notch** (`IMU_GYRO_DNF_EN`, default off) gets RPM for all motors, resampled at
  50 Hz. There's no effect if the notch is disabled.
- **ESC battery** (if used) averages across all online ESCs.

---

## Bench tests (props off)

1. QGC ESC panel shows Motors 1–4 in order, and `esc_status_merge status` agrees.
2. **Unplug CAN.** Motors 3–4 should go OFFLINE within ~0.7 s while `esc_count` stays 4. With
   `COM_ARM_CHK_ESCS=1`, arming should be refused.
3. **Kill DShot telemetry** (unplug the telemetry wire). Motors 1–2 should go OFFLINE within ~1.2 s.
4. **Stop the module** (`esc_status_merge stop`). After 5 s, `COM_ARM_CHK_ESCS=1` should report
   *ESC telemetry missing* and refuse arming. This confirms the repointed check fails closed.
5. **Conflict.** Temporarily assign the same motor function to a DShot and a CAN channel. Its bit should
   appear in `conflicts`.
6. **Log.** Open a ulog and confirm `esc_status` multi_id 0 and 1, plus `esc_status_merged`.

---

## Known limitations

- **Contiguous motor maps assumed.** `esc_count` = highest seen slot + 1, so a map like Motors 1, 2, 5, 6
  would show 3–4 as offline and block arming under `COM_ARM_CHK_ESCS`. That's fine for Motors 1–4.
- **The KDE (DroneCAN) temperature reads 0.1 °C.** That's a driver/ESC issue upstream of this module.
  Verify against a warm ESC before trusting it.
- **Bidirectional DShot** sets `esc_armed_flags = _outputs_on` (bit 0 only) in v1.17.0. If you enable
  BDShot, check the merged armed flags before relying on `FD_ESCS_EN`.

---

## Combining with px4_flow_angle

A build accepts only **one** `EXTERNAL_MODULES_LOCATION`, so the two out-of-tree repos don't compose.
That's why both now mount in-tree as submodules of the Compressor PX4 fork:

```
PX4-Autopilot  (fork, branch compressor/v1.17)
├── src/modules/esc_status_merge/   <- submodule: this repo
├── src/modules/flow_angle/         <- submodule: px4_flow_angle
└── boards/px4/fmu-v5/compressor.px4board
```

## Updating PX4 (out-of-tree mode)

In the fork, upgrading is a rebase of `compressor/v1.17` onto the new tag instead. Out-of-tree:
check out the new tag, then run `tools/patch_px4.py`. If it errors, re-grep the consumers:

```bash
grep -rn "ORB_ID(esc_status)" PX4-Autopilot/src/modules
```

Update `CONSUMERS` / `EXPECTED_PX4_TAG` in the script. Also check whether `EscStatus.msg` changed: later
PX4 versions grow it to 12 ESCs with `uint16` flags, which the module handles through
`CONNECTED_ESC_MAX` and `decltype` on the flag fields.
