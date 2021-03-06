#ifndef __MCB_INTERNAL
#define __MCB_INTERNAL

#include <linux/types.h>

#define CHAMELEON_FILENAME_LEN		12
#define CHAMELEONV2_MAGIC		0xabce

enum chameleon_descriptor_type {
	CHAMELEON_DTYPE_GENERAL = 0x0,
	CHAMELEON_DTYPE_BRIDGE = 0x1,
	CHAMELEON_DTYPE_CPU = 0x2,
	CHAMELEON_DTYPE_BAR = 0x3,
	CHAMELEON_DTYPE_END = 0xf,
};

enum chameleon_bus_type {
	CHAMELEON_BUS_WISHBONE,
	CHAMELEON_BUS_AVALON,
	CHAMELEON_BUS_LPC,
	CHAMELEON_BUS_ISA,
};

/**
 * struct chameleon_fpga_header
 *
 * @revision:	Revison of Chameleon table in FPGA
 * @model:	Chameleon table model ASCII char
 * @minor:	Revision minor
 * @bus_type:	Bus type (usually %CHAMELEON_BUS_WISHBONE)
 * @magic:	Chameleon header magic number (0xabce for version 2)
 * @reserved:	Reserved
 * @filename:	Filename of FPGA bitstream
 */
struct chameleon_fpga_header {
	u8 revision;
	char model;
	u8 minor;
	u8 bus_type;
	u16 magic;
	u16 reserved;
	/* This one has no '\0' at the end!!! */
	char filename[CHAMELEON_FILENAME_LEN];
} __packed;
#define HEADER_MAGIC_OFFSET 0x4

/**
 * struct chameleon_gdd - Chameleon General Device Descriptor
 *
 * @irq:	the position in the FPGA's IRQ controller vector
 * @rev:	the revision of the variant's implementation
 * @var:	the variant of the IP core
 * @dev:	the device  the IP core is
 * @dtype:	device descriptor type
 * @bar:	BAR offset that must be added to module offset
 * @inst:	the instance number of the device, 0 is first instance
 * @group:	the group the device belongs to (0 = no group)
 * @reserved:	reserved
 * @offset:	beginning of the address window of desired module
 * @size:	size of the module's address window
 */
struct chameleon_gdd {
	__le32 reg1;
	__le32 reg2;
	__le32 offset;
	__le32 size;

} __packed;

/* GDD Register 1 fields */
#define GDD_IRQ(x) ((x) & 0x1f)
#define GDD_REV(x) (((x) >> 5) & 0x3f)
#define GDD_VAR(x) (((x) >> 11) & 0x3f)
#define GDD_DEV(x) (((x) >> 18) & 0x3ff)
#define GDD_DTY(x) (((x) >> 28) & 0xf)

/* GDD Register 2 fields */
#define GDD_BAR(x) ((x) & 0x7)
#define GDD_INS(x) (((x) >> 3) & 0x3f)
#define GDD_GRP(x) (((x) >> 9) & 0x3f)

/**
 * struct chameleon_bdd - Chameleon Bridge Device Descriptor
 *
 * @irq:	the position in the FPGA's IRQ controller vector
 * @rev:	the revision of the variant's implementation
 * @var:	the variant of the IP core
 * @dev:	the device  the IP core is
 * @dtype:	device descriptor type
 * @bar:	BAR offset that must be added to module offset
 * @inst:	the instance number of the device, 0 is first instance
 * @dbar:	destination bar from the bus _behind_ the bridge
 * @chamoff:	offset within the BAR of the source bus
 * @offset:
 * @size:
 */
struct chameleon_bdd {
	unsigned int irq:6;
	unsigned int rev:6;
	unsigned int var:6;
	unsigned int dev:10;
	unsigned int dtype:4;
	unsigned int bar:3;
	unsigned int inst:6;
	unsigned int dbar:3;
	unsigned int group:6;
	unsigned int reserved:14;
	u32 chamoff;
	u32 offset;
	u32 size;
} __packed;

int chameleon_parse_cells(struct mcb_bus *bus, phys_addr_t mapbase,
			  void __iomem *base);

#endif
