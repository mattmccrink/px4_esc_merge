#!/usr/bin/env python3
"""
Idempotent, content-matched edits to the PX4 tree for esc_status_merge.

Re-run after every `git checkout` / submodule update of PX4-Autopilot.
Every edit is anchored on exact source text; if an anchor is missing (e.g. after
a PX4 version bump) the script exits non-zero instead of silently skipping it.
A consumer left on raw esc_status would quietly health-check only one driver's
motors, which is the failure this module exists to remove.

Usage:
    tools/patch_px4.py            # apply (default PX4 tree: ./PX4-Autopilot)
    tools/patch_px4.py --check    # verify only; exit 1 if anything is unpatched
    tools/patch_px4.py --px4 /path/to/PX4-Autopilot

Edits (verified against PX4 v1.17.0):
  1. CMakeLists.txt            move the external-modules block after the in-tree
                               module loop so DEPENDS on in-tree targets
                               (px4_work_queue) resolve. Same edit as
                               px4_flow_angle; detection is position-based, so
                               running both repos' scripts on one tree is safe.
  2. msg/EscStatus.msg         add '# TOPICS esc_status esc_status_merged'
  3. consumers                 ORB_ID(esc_status) -> ORB_ID(esc_status_merged)
  4. logger                    log raw esc_status as multi-instance + merged topic
"""

import argparse
import pathlib
import re
import subprocess
import sys

EXPECTED_PX4_TAG = "v1.17.0"

REPO = pathlib.Path(__file__).resolve().parents[1]

# Subscribers of esc_status in PX4 v1.17.0 (from: grep -rn "ORB_ID(esc_status)" src/modules).
# Publishers (dshot, uavcan, ...) and FailureInjector (not a subscriber) are deliberately absent.
CONSUMERS = [
	"src/modules/mavlink/streams/ESC_INFO.hpp",                                # QGC ESC panel
	"src/modules/mavlink/streams/ESC_STATUS.hpp",                              # QGC ESC panel
	"src/modules/commander/HealthAndArmingChecks/checks/escCheck.hpp",         # COM_ARM_CHK_ESCS
	"src/modules/commander/failure_detector/FailureDetector.hpp",              # FD_ESCS_EN, motor failure
	"src/modules/esc_battery/EscBattery.hpp",                                  # battery from ESC telemetry
	"src/modules/sensors/vehicle_angular_velocity/VehicleAngularVelocity.hpp", # ESC-RPM dynamic notch
]

OLD_SUB = "ORB_ID(esc_status)"
NEW_SUB = "ORB_ID(esc_status_merged)"


class PatchError(Exception):
	pass


# ---------------------------------------------------------------------------
# 1) CMake: external modules after in-tree modules
# ---------------------------------------------------------------------------
EXT_BLOCK_RE = re.compile(
	r"#=+\n# external modules\n#\n.*?\nendif\(\)\n", re.S)
INTREE_LOOP = "foreach(module ${config_module_list})\n\tadd_subdirectory(src/${module})\nendforeach()\n"


def cmake_order(s):
	m = EXT_BLOCK_RE.search(s)
	loop = s.find(INTREE_LOOP)

	if not m:
		raise PatchError("external-modules block not found")

	if loop < 0:
		raise PatchError("in-tree module foreach loop not found")

	if m.start() > loop:
		return s  # already after the loop

	block = m.group(0)
	s = s[:m.start()] + s[m.end():]
	loop = s.find(INTREE_LOOP) + len(INTREE_LOOP)
	return s[:loop] + "\n" + block + s[loop:]


# ---------------------------------------------------------------------------
# 2) msg: second topic name for the EscStatus type
# ---------------------------------------------------------------------------
def msg_topics(s):
	if re.search(r"^# TOPICS .*\besc_status_merged\b", s, re.M):
		return s

	m = re.search(r"^# TOPICS .*$", s, re.M)

	if m:
		return s[:m.end()] + " esc_status_merged" + s[m.end():]

	return s.rstrip("\n") + "\n\n# TOPICS esc_status esc_status_merged\n"


# ---------------------------------------------------------------------------
# 3) consumers
# ---------------------------------------------------------------------------
def repoint(s):
	n_old = s.count(OLD_SUB)
	n_new = s.count(NEW_SUB)

	if n_old == 0 and n_new == 1:
		return s  # already patched

	if n_old == 1 and n_new == 0:
		return s.replace(OLD_SUB, NEW_SUB)

	raise PatchError(f"expected exactly one {OLD_SUB} (found {n_old} raw, {n_new} merged)")


# ---------------------------------------------------------------------------
# 4) logger (default topic set)
# ---------------------------------------------------------------------------
LOG_OLD = '\tadd_topic("esc_status");\n'
LOG_NEW = ('\tadd_topic_multi("esc_status", 0, 4); // esc_status_merge: raw per-driver instances\n'
	   '\tadd_topic("esc_status_merged");\n')


def logger(s):
	if '"esc_status_merged"' in s:
		return s

	if s.count(LOG_OLD) != 1:
		raise PatchError('default-set add_topic("esc_status"); anchor not found exactly once')

	return s.replace(LOG_OLD, LOG_NEW)


EDITS = [("CMakeLists.txt", cmake_order),
	 ("msg/EscStatus.msg", msg_topics)] + \
	[(c, repoint) for c in CONSUMERS] + \
	[("src/modules/logger/logged_topics.cpp", logger)]


def px4_version_warning(px4):
	try:
		desc = subprocess.run(["git", "-C", str(px4), "describe", "--tags", "--always"],
				      capture_output=True, text=True, check=True).stdout.strip()
	except Exception:
		return

	if desc != EXPECTED_PX4_TAG:
		print(f"WARNING: PX4 tree is at '{desc}', patches were verified against {EXPECTED_PX4_TAG}.")


def main():
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--px4", type=pathlib.Path, default=REPO / "PX4-Autopilot", help="PX4-Autopilot tree")
	ap.add_argument("--check", action="store_true", help="verify only; exit 1 if any edit is not applied")
	args = ap.parse_args()

	px4 = args.px4.resolve()

	if not (px4 / "msg" / "EscStatus.msg").is_file():
		sys.exit(f"ERROR: {px4} does not look like a PX4-Autopilot tree")

	px4_version_warning(px4)

	errors, pending = 0, 0

	for rel, fn in EDITS:
		path = px4 / rel

		try:
			src = path.read_text()
			new = fn(src)

		except FileNotFoundError:
			print(f"ERROR    {rel}: file not found")
			errors += 1
			continue

		except PatchError as e:
			print(f"ERROR    {rel}: {e}")
			errors += 1
			continue

		if new == src:
			print(f"ok       {rel}")

		elif args.check:
			print(f"PENDING  {rel}")
			pending += 1

		else:
			path.write_text(new)
			print(f"patched  {rel}")

	if errors:
		sys.exit(f"\n{errors} edit(s) failed. PX4 tree changed? Review before building.")

	if pending:
		sys.exit(f"\n{pending} edit(s) not applied. Run tools/patch_px4.py.")


if __name__ == "__main__":
	main()
