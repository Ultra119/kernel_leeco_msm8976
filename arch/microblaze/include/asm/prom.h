/*
 * Definitions for talking to the Open Firmware PROM on
 * Power Macintosh computers.
 *
 * Copyright (C) 1996-2005 Paul Mackerras.
 *
 * Updates for PPC64 by Peter Bergner & David Engebretsen, IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#ifndef _ASM_MICROBLAZE_PROM_H
#define _ASM_MICROBLAZE_PROM_H

#include <linux/of.h>

#define HAVE_ARCH_DEVTREE_FIXUPS

/* Other Prototypes */
enum early_consoles {
	UARTLITE = 1,
	UART16550 = 2,
};

extern int of_early_console(void *version);

/*
 * OF address retreival & translation
 */

#ifdef CONFIG_PCI
extern unsigned long pci_address_to_pio(phys_addr_t address);
#define pci_address_to_pio pci_address_to_pio
#endif	/* CONFIG_PCI */

#endif /* _ASM_MICROBLAZE_PROM_H */
