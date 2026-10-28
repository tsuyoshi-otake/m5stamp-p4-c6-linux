# SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
# Example LP core program for lp_core= in config.txt (#59).  The HP app is
# only a vehicle for ulp_embed_binary; the result is
# build/esp-idf/main/ulp_main/ulp_main.bin.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lp_example)
