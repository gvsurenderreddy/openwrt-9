/*
 *  Carsten Langgaard, carstenl@mips.com
 *  Copyright (C) 1999, 2000 MIPS Technologies, Inc.  All rights reserved.
 *  Copyright (C) 2007 Felix Fietkau <nbd@openwrt.org>
 *
 *  This program is free software; you can distribute it and/or modify it
 *  under the terms of the GNU General Public License (Version 2) as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place - Suite 330, Boston MA 02111-1307, USA.
 */

/* FIXME: convert nasty volatile register derefs to readl/writel calls */

#include <linux/types.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <asm/io.h>
#include <asm/paccess.h>
#include <asm/amazon/irq.h>
#include <asm/amazon/amazon.h>

#define AMAZON_PCI_REG32( addr )		  (*(volatile u32 *)(addr))
#ifndef AMAZON_PCI_MEM_BASE
#define AMAZON_PCI_MEM_BASE    0xb2000000
#endif
#define AMAZON_PCI_MEM_SIZE    0x00400000
#define AMAZON_PCI_IO_BASE     0xb2400000
#define AMAZON_PCI_IO_SIZE     0x00002000

#define AMAZON_PCI_CFG_BUSNUM_SHF 16
#define AMAZON_PCI_CFG_DEVNUM_SHF 11
#define AMAZON_PCI_CFG_FUNNUM_SHF 8

#define PCI_ACCESS_READ  0
#define PCI_ACCESS_WRITE 1

static inline u32 amazon_r32(u32 addr)
{
	u32 *ptr = (u32 *) addr;
	return __raw_readl(ptr);
}

static inline void amazon_w32(u32 addr, u32 val)
{
	u32 *ptr = (u32 *) addr;
	__raw_writel(val, ptr);
}


static struct resource pci_io_resource = {
	.name = "io pci IO space",
#if 0
	.start = AMAZON_PCI_IO_BASE,
	.end = AMAZON_PCI_IO_BASE + AMAZON_PCI_IO_SIZE - 1,
#endif
	.start = 0,
	.end = AMAZON_PCI_IO_SIZE - 1,
	.flags = IORESOURCE_IO
};

static struct resource pci_mem_resource = {
	.name = "ext pci memory space",
	.start = AMAZON_PCI_MEM_BASE,
	.end = AMAZON_PCI_MEM_BASE + AMAZON_PCI_MEM_SIZE - 1,
	.flags = IORESOURCE_MEM
};

static inline u32 amazon_pci_swap(u32 val)
{
#ifdef CONFIG_AMAZON_PCI_HW_SWAP
	return swab32(val);
#else
	return val;
#endif
}

static int amazon_pci_config_access(unsigned char access_type,
	struct pci_bus *bus, unsigned int devfn, unsigned int where, u32 *data)
{
	unsigned long flags;
	u32 pci_addr;
	u32 val;
	int ret;
   
	/* Amazon support slot from 0 to 15 */
	/* devfn 0 & 0x20 is itself */
	if ((bus != 0) || (devfn == 0) || (devfn == 0x20))
		return 1;

	pci_addr=AMAZON_PCI_CFG_BASE |
		bus->number << AMAZON_PCI_CFG_BUSNUM_SHF |
		devfn << AMAZON_PCI_CFG_FUNNUM_SHF |
		(where & ~0x3);
    
	local_irq_save(flags);
	if (access_type == PCI_ACCESS_WRITE) {
		val = amazon_pci_swap(*data);
		ret = put_dbe(val, (u32 *)pci_addr);
	} else {
		ret = get_dbe(val, (u32 *)pci_addr);
		*data = amazon_pci_swap(val);
	}

	amazon_w32(PCI_MODE, amazon_r32(PCI_MODE) & (~(1<<PCI_MODE_cfgok_bit)));
	amazon_w32(STATUS_COMMAND_ADDR, amazon_r32(STATUS_COMMAND_ADDR));
	amazon_w32(PCI_MODE, amazon_r32(PCI_MODE) | (~(1<<PCI_MODE_cfgok_bit)));
	local_irq_restore(flags);

	return ret; 
}


static int amazon_pci_read(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 *val)
{
	u32 data = 0;
	int ret = PCIBIOS_SUCCESSFUL;

	if (amazon_pci_config_access(PCI_ACCESS_READ, bus, devfn, where, &data)) {
		data = ~0;
		ret = -1;
	}

	switch (size) {
		case 1:
			*((u8 *) val) = (data >> ((where & 3) << 3)) & 0xff;
			break;
		case 2:
			*((u16 *) val) = (data >> ((where & 3) << 3)) & 0xffff;
			break;
		case 4:
			*val = data;
			break;
		default:
			return -1;
	}

	return ret;
}


static int amazon_pci_write(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 val)
{
	if (size != 4) {
		u32 data;

		if (amazon_pci_config_access(PCI_ACCESS_READ, bus, devfn, where, &data))
			return -1;

		if (size == 1)
			val = (data & ~(0xff << ((where & 3) << 3))) | (val << ((where & 3) << 3));
		else if (size == 2)
			val = (data & ~(0xffff << ((where & 3) << 3))) | (val << ((where & 3) << 3));
		else
			return -1;
	}

	if (amazon_pci_config_access(PCI_ACCESS_WRITE, bus, devfn, where, &val))
	       return -1;

	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops amazon_pci_ops = {
	amazon_pci_read,
	amazon_pci_write
};

static struct pci_controller amazon_pci_controller = {
	.pci_ops = &amazon_pci_ops,
	.mem_resource = &pci_mem_resource,
	.io_resource = &pci_io_resource
};

int __init pcibios_map_irq(struct pci_dev *dev, u8 slot, u8 pin)
{
	switch (slot) {
		case 13:
			/* IDSEL = AD29 --> USB Host Controller */
			return INT_NUM_IM2_IRL15;
		case 14:
			/* IDSEL = AD30 --> mini PCI connector */
			return INT_NUM_IM2_IRL14;
		default:
			printk("Warning: no IRQ found for PCI device in slot %d, pin %d\n", slot, pin);
			return 0;
	}
}

int pcibios_plat_dev_init(struct pci_dev *dev)
{
	switch(dev->irq) {
		case INT_NUM_IM2_IRL15:
			/* 
			 * IDSEL = AD29 --> USB Host Controller
			 * PCI_INTA/B/C--GPIO Port0.2--EXIN3
			 * IN/ALT0:1 ALT1:0
			 * PULL UP
			 */
			(*AMAZON_GPIO_P0_DIR) = (*AMAZON_GPIO_P0_DIR) & 0xfffffffb;
			(*AMAZON_GPIO_P0_ALTSEL0) = (*AMAZON_GPIO_P0_ALTSEL0)| 4;
			(*AMAZON_GPIO_P0_ALTSEL1) = (*AMAZON_GPIO_P0_ALTSEL1)& 0xfffffffb;
			(*AMAZON_GPIO_P0_PUDSEL) =  (*AMAZON_GPIO_P0_PUDSEL) | 4;
			(*AMAZON_GPIO_P0_PUDEN) = (*AMAZON_GPIO_P0_PUDEN) | 4;
			//External Interrupt Node
			(*AMAZON_ICU_EXTINTCR) = (*AMAZON_ICU_EXTINTCR)|0x6000; /* Low Level triggered */
			(*AMAZON_ICU_IRNEN) = (*AMAZON_ICU_IRNEN)|0x8;
			pci_write_config_byte(dev, PCI_INTERRUPT_LINE, dev->irq);
			break;
		case INT_NUM_IM2_IRL14:
			/* 
			 * IDSEL = AD30 --> mini PCI connector 
			 * PCI_INTA--GPIO Port0.1--EXIN2
			 * IN/ALT0:1 ALT1:0
			 * PULL UP
			 */
			(*AMAZON_GPIO_P0_DIR) = (*AMAZON_GPIO_P0_DIR) & 0xfffffffd;
			(*AMAZON_GPIO_P0_ALTSEL0) = (*AMAZON_GPIO_P0_ALTSEL0)| 2;
			(*AMAZON_GPIO_P0_ALTSEL1) = (*AMAZON_GPIO_P0_ALTSEL1)& 0xfffffffd;
			(*AMAZON_GPIO_P0_PUDSEL) =  (*AMAZON_GPIO_P0_PUDSEL) | 2;
			(*AMAZON_GPIO_P0_PUDEN) = (*AMAZON_GPIO_P0_PUDEN) | 2;
			//External Interrupt Node
			(*AMAZON_ICU_EXTINTCR) = (*AMAZON_ICU_EXTINTCR)|0x600;
			(*AMAZON_ICU_IRNEN) = (*AMAZON_ICU_IRNEN)|0x4;
			pci_write_config_byte(dev, PCI_INTERRUPT_LINE, dev->irq);
			break;
		default:
			return 1;
	}
	return 0;
}

int amazon_pci_init(void)
{
	u32 temp_buffer;

#ifdef CONFIG_AMAZON_PCI_HW_SWAP
	AMAZON_PCI_REG32(IRM) = AMAZON_PCI_REG32(IRM) | (1<<27) | (1<<28);
	wmb();
#endif

	AMAZON_PCI_REG32(CLOCK_CONTROL) = AMAZON_PCI_REG32(CLOCK_CONTROL) | (1<<ARB_CTRL_bit);
	amazon_w32(PCI_MODE, amazon_r32(PCI_MODE) & (~(1<<PCI_MODE_cfgok_bit)));

	AMAZON_PCI_REG32(STATUS_COMMAND_ADDR) = AMAZON_PCI_REG32(STATUS_COMMAND_ADDR) | (1<<BUS_MASTER_ENABLE_BIT) |(1<<MEM_SPACE_ENABLE_BIT);

	temp_buffer = AMAZON_PCI_REG32(PCI_ARB_CTRL_STATUS_ADDR);
	temp_buffer = temp_buffer | (1<< INTERNAL_ARB_ENABLE_BIT);
	temp_buffer = temp_buffer & ~(3<< PCI_MASTER0_REQ_MASK_2BITS);
	temp_buffer = temp_buffer & ~(3<< PCI_MASTER0_GNT_MASK_2BITS);

	/* flash */
	temp_buffer = temp_buffer & ~(3<< PCI_MASTER1_REQ_MASK_2BITS);
	temp_buffer = temp_buffer & ~(3<< PCI_MASTER1_GNT_MASK_2BITS);

	/* external master */
	temp_buffer = temp_buffer & ~(3<< PCI_MASTER2_REQ_MASK_2BITS);
	temp_buffer = temp_buffer & ~(3<< PCI_MASTER2_GNT_MASK_2BITS);

	AMAZON_PCI_REG32(PCI_ARB_CTRL_STATUS_ADDR) = temp_buffer;
	wmb();

	AMAZON_PCI_REG32(FPI_ADDRESS_MAP_0) = 0xb2000000;
	AMAZON_PCI_REG32(FPI_ADDRESS_MAP_1) = 0xb2100000;
	AMAZON_PCI_REG32(FPI_ADDRESS_MAP_2) = 0xb2200000;
	AMAZON_PCI_REG32(FPI_ADDRESS_MAP_3) = 0xb2300000;
	AMAZON_PCI_REG32(FPI_ADDRESS_MAP_4) = 0xb2400000;
	AMAZON_PCI_REG32(FPI_ADDRESS_MAP_5) = 0xb2500000;
	AMAZON_PCI_REG32(FPI_ADDRESS_MAP_6) = 0xb2600000;
	AMAZON_PCI_REG32(FPI_ADDRESS_MAP_7) = 0xb2700000;
	   
	AMAZON_PCI_REG32(BAR11_MASK) = 0x0f000008;
	AMAZON_PCI_REG32(PCI_ADDRESS_MAP_11) = 0x0;
	AMAZON_PCI_REG32(BAR1_ADDR) = 0x0;
	amazon_w32(PCI_MODE, amazon_r32(PCI_MODE) | (~(1<<PCI_MODE_cfgok_bit)));
	//use 8 dw burse length
	AMAZON_PCI_REG32(FPI_BURST_LENGTH) = 0x303;

	set_io_port_base(ioremap(AMAZON_PCI_IO_BASE, AMAZON_PCI_IO_SIZE));
	register_pci_controller(&amazon_pci_controller);
	return 0;
}
arch_initcall(amazon_pci_init);
