/* Copyright 2022 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <ztest.h>

#include "emul/tcpc/emul_tcpci.h"
#include "motion_sense_fifo.h"
#include "test/drivers/stubs.h"
#include "usb_pd_tcpm.h"

static void motion_sense_fifo_reset_before(const struct ztest_unit_test *test,
					   void *data)
{
	ARG_UNUSED(test);
	ARG_UNUSED(data);
	motion_sense_fifo_reset();
}
ZTEST_RULE(motion_sense_fifo_reset, motion_sense_fifo_reset_before, NULL);

static void tcpci_revision_reset_before(const struct ztest_unit_test *test,
					void *data)
{
	ARG_UNUSED(test);
	ARG_UNUSED(data);
	const struct emul *tcpci_emul =
		emul_get_binding(DT_LABEL(DT_NODELABEL(tcpci_emul)));
	const struct emul *ps8xxx_emul =
		emul_get_binding(DT_LABEL(DT_NODELABEL(ps8xxx_emul)));

	/* Set TCPCI to revision 2 for both emulators */
	tcpc_config[USBC_PORT_C0].flags |= TCPC_FLAGS_TCPCI_REV2_0;
	tcpci_emul_set_rev(tcpci_emul, TCPCI_EMUL_REV2_0_VER1_1);

	tcpc_config[USBC_PORT_C1].flags |= TCPC_FLAGS_TCPCI_REV2_0;
	tcpci_emul_set_rev(ps8xxx_emul, TCPCI_EMUL_REV2_0_VER1_1);
}
ZTEST_RULE(tcpci_revision_reset, tcpci_revision_reset_before, NULL);
