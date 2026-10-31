#pragma once

/**
 * Brings up the emulated OpenCores Ethernet adapter of QEMU, which has no Wi-Fi. QEMU's user networking puts the
 * host machine at 10.0.2.2. Only built with CONFIG_CM_NETWORK_QEMU.
 */
void qemu_network_start(void);
