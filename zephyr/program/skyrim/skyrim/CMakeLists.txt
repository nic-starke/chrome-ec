# Copyright 2022 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

cros_ec_library_include_directories_ifdef(CONFIG_BOARD_SKYRIM include)
zephyr_library_sources(
		"src/usb_mux_config.c"
		"src/ppc_config.c"
		"src/form_factor.c"
		"src/alt_charger.c"
)
zephyr_library_sources_ifdef(CONFIG_PLATFORM_EC_FAN "src/fan.c")
