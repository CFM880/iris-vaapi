#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# nabu-power-measure.sh - measure the power draw of the Xiaomi Pad 5 (nabu).
#
# Reads the PM8150B fuel gauge (qcom-battery).  Battery current cannot be
# sampled while the system is suspended, so --suspend derives the figure from
# the capacity drop across the sleep instead.
#
# Examples:
#   sudo ./nabu-power-measure.sh --screen-off --duration 120 -l "idle screen-off"
#   sudo ./nabu-power-measure.sh --suspend 600 -l "suspend 10 min"
#   ./nabu-power-measure.sh -d 60 -o /tmp/nabu-power.csv
#
set -u

BAT=/sys/class/power_supply/qcom-battery
CHG=/sys/class/power_supply/pm8150b-charger
RTC=rtc1

DURATION=60
INTERVAL=1
LABEL=""
CSV=""
SCREEN_OFF=0
SUSPEND=0

die() { echo "error: $*" >&2; exit 1; }

usage() {
	cat <<'EOF'
usage: nabu-power-measure.sh [options]

  -d, --duration SEC   sampling window in seconds          (default 60)
  -i, --interval SEC   seconds between samples             (default 1)
  -l, --label TEXT     label for the summary / CSV
  -o, --csv FILE       also write raw samples to FILE
  -s, --screen-off     blank the screen for the run (needs root via sudo)
  -S, --suspend SEC    suspend for SEC seconds, report from the capacity drop
  -h, --help           this help

Notes:
  * The device must be discharging - unplug the charger first, the script
    refuses to run while it is charging.
  * current_now uses the standard sign: negative = discharging.
  * Power = current_now (uA) * voltage_now (uV).  The gauge is coarse and the
    sample is noisy, so use a run of at least a minute; for standby figures
    prefer --suspend 600 or so.
  * --screen-off blanks through the GNOME screensaver; waking the screen when
    it finishes may need a touch (GNOME does not always honour SetActive
    false without input).
EOF
	exit "${1:-0}"
}

while [ $# -gt 0 ]; do
	case "$1" in
	-d|--duration) DURATION=${2:?-}; shift 2 ;;
	-i|--interval) INTERVAL=${2:?-}; shift 2 ;;
	-l|--label)    LABEL=${2:?-}; shift 2 ;;
	-o|--csv)      CSV=${2:?-}; shift 2 ;;
	-s|--screen-off) SCREEN_OFF=1; shift ;;
	-S|--suspend)  SUSPEND=${2:?-}; shift 2 ;;
	-h|--help)     usage 0 ;;
	*)             die "unknown option: $1 (try --help)" ;;
	esac
done

[ -r "$BAT/current_now" ] || die "cannot read $BAT/current_now - is this a nabu?"
[ "$(cat "$CHG/online" 2>/dev/null || echo 0)" = "1" ] &&
	die "charger is online - unplug it, the numbers would be meaningless"

cap_now() { cat "$BAT/capacity"; }

# Blank the screen through the invoking user's GNOME session (needs sudo).
blank_uid=""
if [ "$SCREEN_OFF" -eq 1 ]; then
	if [ "$(id -u)" -ne 0 ] || [ -z "${SUDO_USER:-}" ]; then
		echo "note: --screen-off needs 'sudo'; measuring with the screen as-is" >&2
		SCREEN_OFF=0
	else
		blank_uid=$(id -u "$SUDO_USER")
	fi
fi
screensaver() {   # $1 = true|false
	[ "$SCREEN_OFF" -eq 1 ] || return 0
	runuser -u "$SUDO_USER" -- env \
		XDG_RUNTIME_DIR="/run/user/$blank_uid" \
		DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$blank_uid/bus" \
		busctl --user call org.gnome.ScreenSaver /org/gnome/ScreenSaver \
		       org.gnome.ScreenSaver SetActive b "$1" >/dev/null 2>&1 || true
}
trap 'screensaver false' EXIT

echo "nabu power measurement${LABEL:+  [$LABEL]}"
echo "  gauge      : $BAT"
if [ "$SUSPEND" -gt 0 ]; then
	echo "  mode       : suspend ${SUSPEND}s, energy from capacity drop"
else
	echo "  mode       : sample every ${INTERVAL}s for ${DURATION}s"
fi
echo "  screen-off : $([ "$SCREEN_OFF" -eq 1 ] && echo yes || echo no)"

# ---------------------------------------------------------------------------
# Suspend mode: current is unreadable while asleep, use the capacity drop.
# ---------------------------------------------------------------------------
if [ "$SUSPEND" -gt 0 ]; then
	cap0=$(cap_now)
	v0=$(cat "$BAT/voltage_now")
	fc=$(cat "$BAT/charge_full_design" 2>/dev/null || echo 0)   # uAh

	awk -v c="$cap0" -v v="$v0" 'BEGIN{printf "  before     : %d%%  %.3f V\n", c, v/1e6}'
	echo "  suspending : ~${SUSPEND}s ..."
	screensaver true
	rtcwake -d "$RTC" -m no -s "$SUSPEND" >/dev/null 2>&1 || true
	systemctl suspend >/dev/null 2>&1 || true
	sleep 5
	cap1=$(cap_now)

	awk -v c0="$cap0" -v c1="$cap1" -v fc="$fc" -v v="$v0" -v t="$SUSPEND" 'BEGIN{
		wh   = (c0 - c1) / 100.0 * (fc / 1e6) * (v / 1e6);
		step = (fc / 1e6) * (v / 1e6) / 100.0;
		printf "  after      : %d%%\n", c1;
		printf "  drop       : %d%%  (~%.3f Wh over %ds)\n", c0 - c1, wh, t;
		printf "  power      : %.3f W\n", wh / (t / 3600.0);
		printf "  resolution : 1%% step = %.2f W at %ds - use >=1800s for standby\n",
		       step / (t / 3600.0), t;
	}'
	exit 0
fi

# ---------------------------------------------------------------------------
# Sampling mode.
# ---------------------------------------------------------------------------
screensaver true
[ "$SCREEN_OFF" -eq 1 ] && sleep 4        # let the display runtime-suspend

[ -n "$CSV" ] && echo "time,current_uA,voltage_uV,power_W,capacity" > "$CSV"

cap0=$(cap_now)
n=0
sum_c=0
sum_uw=0
min_uw=0
max_uw=0
i=0
while [ "$i" -lt "$DURATION" ]; do
	c=$(cat "$BAT/current_now")
	v=$(cat "$BAT/voltage_now")
	cap=$(cat "$BAT/capacity")
	uw=$(( c * v / 1000000 ))            # microwatts (64-bit is plenty)
	if [ "$n" -eq 0 ]; then min_uw=$uw; max_uw=$uw; fi
	[ "$uw" -lt "$min_uw" ] && min_uw=$uw
	[ "$uw" -gt "$max_uw" ] && max_uw=$uw
	sum_c=$(( sum_c + c ))
	sum_uw=$(( sum_uw + uw ))
	n=$(( n + 1 ))
	[ -n "$CSV" ] && echo "$(date +%H:%M:%S),$c,$v,$(awk -v u="$uw" 'BEGIN{printf "%.6f", u/1e6}'),$cap" >> "$CSV"
	sleep "$INTERVAL"
	i=$(( i + INTERVAL ))
done
cap1=$(cap_now)

awk -v n="$n" -v sc="$sum_c" -v sp="$sum_uw" -v lo="$min_uw" -v hi="$max_uw" 'BEGIN{
	printf "  samples    : %d\n", n;
	printf "  current    : avg %.1f mA\n", sc / n / 1000.0;
	printf "  power      : avg %.3f W   min %.3f W   max %.3f W\n",
	       sp / n / 1e6, lo / 1e6, hi / 1e6;
}'
echo "  capacity   : ${cap0}% -> ${cap1}%   (short runs may not move the gauge)"
if [ -n "$CSV" ]; then echo "  csv        : $CSV"; fi
exit 0
