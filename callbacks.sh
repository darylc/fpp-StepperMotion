#!/bin/sh
#
# fppd asks every plugin which callbacks it implements. Printing "c++:<lib>"
# makes fppd load that shared library from this plugin's directory by full
# path (Plugins.cpp loadUserPlugin); fppd's RUNPATH never covers
# media/plugins. The channel output is resolved through the same path -- see
# src/TmcPlugin.cpp.
echo "c++:libStepperMotion.so"
