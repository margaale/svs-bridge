#!/usr/bin/env bash
# Host unit tests (no ESP-IDF, no board): scripts run from anywhere, tests run from the repo root.
#   tests/run.sh            (CXX=clang++ tests/run.sh to pick another compiler)
set -euo pipefail
cd "$(dirname "$0")/.."

CXX="${CXX:-g++}"
CXXFLAGS=(-std=c++17 -Wall -Wextra -Werror -O1 -g -I src/core)
mkdir -p build-tests

# The RFC 2217 parser is C: built as C, linked into its C++ test.
"${CC:-gcc}" -std=c11 -Wall -Wextra -Werror -O1 -g -I src/core -c src/core/rfc2217_proto.c -o build-tests/rfc2217_proto.o

tests=(test_svs_vectors test_svs_hex test_svs_protocol test_svs_config test_api_events)
for t in "${tests[@]}"; do
    "$CXX" "${CXXFLAGS[@]}" "tests/$t.cpp" -o "build-tests/$t"
done
"$CXX" "${CXXFLAGS[@]}" tests/test_rfc2217.cpp build-tests/rfc2217_proto.o -o build-tests/test_rfc2217

status=0
for t in "${tests[@]}" test_rfc2217; do
    echo "== $t"
    ./build-tests/$t || status=1
    echo
done
exit $status
