#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -eu
cd "$(dirname "$0")"
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -I .. test_usb_serial.cpp -o test_usb_serial
./test_usb_serial
