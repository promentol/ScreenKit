#!/bin/sh
# Writes what /proc and vcgencmd know into the app's system.json, once a second,
# for as long as the app runs. `tools/batocera/pi.sh push` copies this into the
# package and its launcher starts it in the background (and kills it afterwards).
#
#   pi-prelaunch.sh <path to system.json>
#
# Written to a temporary file and renamed, so the app never reads half a file.
set -u
OUT=${1:?usage: pi-prelaunch.sh <system.json>}
TMP="$OUT.tmp"

field() { awk -v k="$1" '$1 == k":" { print $2; exit }' /proc/meminfo; }

model=$(awk -F': ' '/^Model/ { print $2; exit }' /proc/cpuinfo)
[ -n "$model" ] || model=$(awk -F': ' '/^model name/ { print $2; exit }' /proc/cpuinfo)
# /proc/cpuinfo on a Pi has no "model name"; lscpu resolves the implementer and
# part numbers it does have into "Cortex-A53".
cpu=$(lscpu 2>/dev/null | awk -F': +' '/^Model name/ { print $2; exit }')
[ -n "$cpu" ] || cpu=$(awk -F': ' '/^model name/ { print $2; exit }' /proc/cpuinfo)
[ -n "$cpu" ] || cpu=$(awk -F': ' '/^Hardware/ { print $2; exit }' /proc/cpuinfo)
[ -n "$cpu" ] || cpu=$(uname -m)
cpu="$cpu ($(uname -m))"

while :; do
    mhz=$(( $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo 0) / 1000 ))
    governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)
    milli=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo 0)
    temp=$(awk -v m="$milli" 'BEGIN { printf "%.1f", m / 1000 }')
    load=$(cut -d' ' -f1-3 /proc/loadavg)
    up=$(awk '{ d = int($1 / 86400); h = int(($1 % 86400) / 3600); m = int(($1 % 3600) / 60);
                if (d) printf "%dd %dh %dm", d, h, m; else if (h) printf "%dh %dm", h, m; else printf "%dm", m }' /proc/uptime)
    gpumem=$(vcgencmd get_mem gpu 2>/dev/null | cut -d= -f2)
    engine=Hermes
    [ "${SCREENKIT_HERMES_JIT:-0}" = 0 ] || engine="Hermes (JIT)"

    cat > "$TMP" <<JSON
{
  "sampledAt": "$(date '+%H:%M:%S')",
  "host": {
    "model": "${model:-$(uname -m)}",
    "kernel": "$(uname -r)",
    "uptime": "$up",
    "engine": "$engine"
  },
  "cpu": {
    "model": "$cpu",
    "cores": $(nproc),
    "mhz": $mhz,
    "governor": "${governor:-unknown}",
    "tempC": $temp,
    "load": "$load"
  },
  "memory": {
    "totalKb": $(field MemTotal),
    "freeKb": $(field MemFree),
    "availableKb": $(field MemAvailable),
    "buffersKb": $(field Buffers),
    "cachedKb": $(field Cached),
    "swapTotalKb": $(field SwapTotal),
    "swapFreeKb": $(field SwapFree)
  },
  "gpu": { "memory": "${gpumem:-unknown}" }
}
JSON
    mv -f "$TMP" "$OUT"
    sleep 1
done
