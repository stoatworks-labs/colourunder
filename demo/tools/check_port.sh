#!/usr/bin/env bash
#
# The browser demo's CPU half (demo/model.js) against the plugin's own C++.
#
#   demo/tools/check_port.sh
#
# check_port.mjs compiles refport.cpp with the ParamID enum, the constructor
# and the whole of ProcessOpenGL cut out of source/Colourunder.{h,cpp} at run
# time, against the plugin's own Model.cpp, Controls.cpp and Clock.cpp (GL
# recorded, not drawn), then compares the declarations, the clock, every
# LineData float and every pass's target, textures and uniforms with
# demo/model.js's planFrame(), exactly. It says what it covers and what it
# cannot. Called from tools/verify.sh; exits 3 (skip) without node or a C++
# compiler.
#
set -uo pipefail
cd "$(dirname "$0")/../.."

command -v node >/dev/null 2>&1 || { echo "skipped: node not installed"; exit 3; }
command -v c++ >/dev/null 2>&1 || { echo "skipped: no C++ compiler"; exit 3; }
node demo/tools/check_port.mjs "$@"
