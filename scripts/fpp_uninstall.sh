#!/bin/bash
#
# bash, not sh: this sources ${FPPDIR}/scripts/common, which is a bash script.
#
# Must be safe to run more than once, and must leave nothing behind outside
# the plugin directory.
set -e

: "${FPPDIR:=/opt/fpp}"
. "${FPPDIR}/scripts/common"

# The plugin's directory name (its repoName): the channel output's type in
# co-other.json and the name of its data folder.
PLUGIN_NAME="$(basename "$(cd "$(dirname "$0")/.." && pwd)")"
export PLUGIN_NAME

# Reinstall (one plugin, or Reinstall All Plugins after an FPP OS upgrade) is
# this script then the installer, run with FPP_PLUGIN_REINSTALL=1 (FPP 10.4,
# the plugin's minimum); then keep the config, every stepper's stored home
# reference and the channel output entry, so a commissioned rig needs no
# rehome. Unset or 0 is a real uninstall (PLUGIN_GUIDELINES 2.12).
if [ "${FPP_PLUGIN_REINSTALL:-0}" = "1" ]; then
	echo "Stepper Motion: reinstall, keeping configuration and stored positions"
else
	# Drop our entry from the "other" channel outputs through the API, which
	# performs the side effects (backup, fppd reload) a raw file write would skip.
	CUR="$(curl -s -m 5 http://localhost/api/channel/output/co-other 2>/dev/null || true)"
	if [ -n "${CUR}" ]; then
		NEW="$(printf '%s' "${CUR}" | python3 -c '
import json, os, sys
try:
    d = json.load(sys.stdin)
except ValueError:
    sys.exit(1)
outs = d.get("channelOutputs")
if not isinstance(outs, list):
    sys.exit(1)
kept = [o for o in outs if o.get("type") != os.environ["PLUGIN_NAME"]]
if len(kept) == len(outs):
    sys.exit(1)
json.dump({"channelOutputs": kept}, sys.stdout)
' 2>/dev/null || true)"
		if [ -n "${NEW}" ]; then
			curl -s -m 5 -X POST -H 'Content-Type: application/json' \
				-d "${NEW}" http://localhost/api/channel/output/co-other >/dev/null 2>&1 || true
			echo "Stepper Motion: removed the channel output entry"
		fi
	fi

	rm -rf "${MEDIADIR}/plugindata/${PLUGIN_NAME}"
	rm -f "${MEDIADIR}/config/plugin.${PLUGIN_NAME}" "${MEDIADIR}/config/plugin.${PLUGIN_NAME}.json"
fi

# A plugin that provides a channel output cannot be hot-unloaded, so a
# restart is required (PLUGIN_GUIDELINES 2.8/2.9, 3.6). FPP sequences it.
setSetting restartFlag 1

echo "Stepper Motion: uninstall complete"
exit 0
