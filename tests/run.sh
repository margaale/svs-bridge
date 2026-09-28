#!/usr/bin/env bash
# Host unit tests (no ESP-IDF, no board): scripts run from anywhere, tests run from the repo root.
#   tests/run.sh            (CXX=clang++ tests/run.sh to pick another compiler)
set -euo pipefail
cd "$(dirname "$0")/.."

CXX="${CXX:-g++}"
CXXFLAGS=(-std=c++17 -Wall -Wextra -Werror -O1 -g -I src/core)
mkdir -p build-tests

tests=(test_svs_vectors test_svs_hex test_svs_protocol)
for t in "${tests[@]}"; do
    "$CXX" "${CXXFLAGS[@]}" "tests/$t.cpp" -o "build-tests/$t"
done

status=0
for t in "${tests[@]}"; do
    echo "== $t"
    ./build-tests/$t || status=1
    echo
done
exit $status
