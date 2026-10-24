#!/usr/bin/env bash
set -euo pipefail
module_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
service_binary="$(mktemp /tmp/ac-api-v1-service-tests.XXXXXX)"
http_binary="$(mktemp /tmp/ac-api-v1-http-tests.XXXXXX)"
trap 'rm -f "$service_binary" "$http_binary"' EXIT
common=(-std=c++20 -Wall -Wextra -Werror -pedantic -I"$module_root/src")
g++ "${common[@]}" "$module_root/src/ApiService.cpp" -x c++ \
    "$module_root/tests/ApiServiceTests.cpp.in" -o "$service_binary"
"$service_binary"
g++ "${common[@]}" -pthread "$module_root/src/ApiService.cpp" \
    "$module_root/src/HttpServer.cpp" -x c++ "$module_root/tests/HttpServerTests.cpp.in" \
    -o "$http_binary"
"$http_binary"
