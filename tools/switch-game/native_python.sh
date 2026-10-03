#!/bin/sh
# Python for the Vulkan build of switch-game (THEFT4_SWITCH_GAME_VULKAN), from
# the MSYS shell: the MSYS python of devkitPro cannot import pyexpat, which
# SPIRV-Tools' generators need, and a native Windows python does not get the
# /c/... paths that sit inside composite arguments (--x=a,/c/...) converted.
# Every /<drive>/ at the start of an argument or after = or , becomes <drive>:/.
# THEFT4_NATIVE_PYTHON: the native interpreter (a /<drive>/... path).
py="${THEFT4_NATIVE_PYTHON:?set THEFT4_NATIVE_PYTHON to a native python.exe}"
n=$#
i=0
while [ $i -lt $n ]; do
  a=$(printf '%s' "$1" | sed -E 's#(^|[=,])/([a-zA-Z])/#\1\2:/#g')
  shift
  set -- "$@" "$a"
  i=$((i + 1))
done
MSYS2_ARG_CONV_EXCL='*' exec "$py" "$@"
