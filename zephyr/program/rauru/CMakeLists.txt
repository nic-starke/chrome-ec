# Copyright 2023 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

cmake_minimum_required(VERSION 3.20.5)
find_package(Zephyr REQUIRED HINTS "${ZEPHYR_BASE}")

zephyr_include_directories("${CMAKE_CURRENT_SOURCE_DIR}/include")
target_sources(app PRIVATE ${PLATFORM_EC_PROGRAM_DIR}/rauru/src/usbc.c)
target_sources_ifdef(CONFIG_PLATFORM_EC_USB_PD_DP_MODE app PRIVATE
		${PLATFORM_EC_PROGRAM_DIR}/rauru/src/dp_alt_mode.c)

if(DEFINED CONFIG_BOARD_RAURU)
	project(rauru)
	add_subdirectory(rauru)
endif()
