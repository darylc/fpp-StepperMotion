#!/bin/bash
#
# bash, not sh: this sources ${FPPDIR}/scripts/common, which is a bash script.
#
# Runs after FPP clones the plugin, and again on every "Reinstall All Plugins"
# and plugin update -- so everything here must be safe to repeat.
set -e

: "${FPPDIR:=/opt/fpp}"
: "${SRCDIR:=${FPPDIR}/src}"
. "${FPPDIR}/scripts/common"

PLUGIN_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "${PLUGIN_DIR}"

echo "Stepper Motion: building libStepperMotion.so"
make clean >/dev/null 2>&1 || true
make "SRCDIR=${SRCDIR}"

# Per-stepper reference state and config live here rather than in the plugin
# directory, so a plugin update cannot wipe a commissioned rig's home.
PLUGIN_DATA="${MEDIADIR}/plugindata/$(basename "${PLUGIN_DIR}")"
mkdir -p "${PLUGIN_DATA}"
# The installer runs as root; fppd and FPP's file tools run as fpp.
chown fpp:fpp "${PLUGIN_DATA}" 2>/dev/null || true

# FPP's channel output entry for the plugin (co-other.json) is written by
# the plugin itself, through FPP's API, once fppd loads it after the restart
# asked for below, and kept in step with its channel block from then on
# (TmcManager::syncOutputEntry).

# The channel output is only built during fppd startup and a plugin that
# provides one cannot be hot-swapped, so a restart is required
# (PLUGIN_GUIDELINES 2.8, 3.6). FPP sequences it; never restart fppd here.
setSetting restartFlag 1

echo "Stepper Motion: install complete"
