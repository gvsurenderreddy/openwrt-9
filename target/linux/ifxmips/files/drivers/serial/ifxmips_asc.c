/*
 *  Based on drivers/char/serial.c, by Linus Torvalds, Theodore Ts'o.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Copyright (C) 2004 Infineon IFAP DC COM CPE
 * Copyright (C) 2007 Felix Fietkau <nbd@openwrt.org>
 * Copyright (C) 2007 John Crispin <blogic@openwrt.org>
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/fcntl.h>
#include <linux/ptrace.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/circ_buf.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/console.h>
#include <linux/sysrq.h>
#include <linux/irq.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/bitops.h>

#include <asm/system.h>

#include <ifxmips.h>
#include <ifxmips_irq.h>

#define PORT_IFXMIPSASC  111

#include <linux/serial_core.h>

#define UART_DUMMY_UER_RX 1

static void ifxmipsasc_tx_chars(struct uart_port *port);
extern void prom_printf(const char *fmt, ...);
static struct uart_port ifxmipsasc_port[2];
static struct uart_driver ifxmipsasc_reg;
extern unsigned int ifxmips_get_fpi_hz(void);

static void ifxmipsasc_stop_tx(struct uart_port *port)
{
	return;
}

static void ifxmipsasc_start_tx(struct uart_port *port)
{
	unsigned long flags;
	local_irq_save(flags);
	ifxmipsasc_tx_chars(port);
	local_irq_restore(flags);
	return;
}

static void ifxmipsasc_stop_rx(struct uart_port *port)
{
	ifxmips_w32(ASCWHBSTATE_CLRREN, port->membase + IFXMIPS_ASC_WHBSTATE);
}

static void ifxmipsasc_enable_ms(struct uart_port *port)
{
}

#include <linux/version.h>

static void ifxmipsasc_rx_chars(struct uart_port *port)
{
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 26))
	struct tty_struct *tty = port->info->port.tty;
#else
	struct tty_struct *tty = port->info->tty;
#endif
	unsigned int ch = 0, rsr = 0, fifocnt;

	fifocnt = ifxmips_r32(port->membase + IFXMIPS_ASC_FSTAT) & ASCFSTAT_RXFFLMASK;
	while (fifocnt--) {
		u8 flag = TTY_NORMAL;
		ch = ifxmips_r32(port->membase + IFXMIPS_ASC_RBUF);
		rsr = (ifxmips_r32(port->membase + IFXMIPS_ASC_STATE) & ASCSTATE_ANY) | UART_DUMMY_UER_RX;
		tty_flip_buffer_push(tty);
		port->icount.rx++;

		/*
		 * Note that the error handling code is
		 * out of the main execution path
		 */
		if (rsr & ASCSTATE_ANY) {
			if (rsr & ASCSTATE_PE) {
				port->icount.parity++;
				ifxmips_w32(ifxmips_r32(port->membase + IFXMIPS_ASC_WHBSTATE) | ASCWHBSTATE_CLRPE, port->membase + IFXMIPS_ASC_WHBSTATE);
			} else if (rsr & ASCSTATE_FE) {
				port->icount.frame++;
				ifxmips_w32(ifxmips_r32(port->membase + IFXMIPS_ASC_WHBSTATE) | ASCWHBSTATE_CLRFE, port->membase + IFXMIPS_ASC_WHBSTATE);
			}
			if (rsr & ASCSTATE_ROE) {
				port->icount.overrun++;
				ifxmips_w32(ifxmips_r32(port->membase + IFXMIPS_ASC_WHBSTATE) | ASCWHBSTATE_CLRROE, port->membase + IFXMIPS_ASC_WHBSTATE);
			}

			rsr &= port->read_status_mask;

			if (rsr & ASCSTATE_PE)
				flag = TTY_PARITY;
			else if (rsr & ASCSTATE_FE)
				flag = TTY_FRAME;
		}

		if ((rsr & port->ignore_status_mask) == 0)
			tty_insert_flip_char(tty, ch, flag);

		if (rsr & ASCSTATE_ROE)
			/*
			 * Overrun is special, since it's reported
			 * immediately, and doesn't affect the current
			 * character
			 */
			tty_insert_flip_char(tty, 0, TTY_OVERRUN);
	}
	if (ch != 0)
		tty_flip_buffer_push(tty);
	return;
}


static void ifxmipsasc_tx_chars(struct uart_port *port)
{
	struct circ_buf *xmit = &port->info->xmit;
	if (uart_tx_stopped(port)) {
		ifxmipsasc_stop_tx(port);
		return;
	}

	while (((ifxmips_r32(port->membase + IFXMIPS_ASC_FSTAT) & ASCFSTAT_TXFFLMASK)
			>> ASCFSTAT_TXFFLOFF) != TXFIFO_FULL) {
		if (port->x_char) {
			ifxmips_w32(port->x_char, port->membase + IFXMIPS_ASC_TBUF);
			port->icount.tx++;
			port->x_char = 0;
			continue;
		}

		if (uart_circ_empty(xmit))
			break;

		ifxmips_w32(port->info->xmit.buf[port->info->xmit.tail], port->membase + IFXMIPS_ASC_TBUF);
		xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
		port->icount.tx++;
	}

	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(port);
}

static irqreturn_t ifxmipsasc_tx_int(int irq, void *_port)
{
	struct uart_port *port = (struct uart_port *)_port;
	ifxmips_w32(ASC_IRNCR_TIR, port->membase + IFXMIPS_ASC_IRNCR);
	ifxmipsasc_start_tx(port);
	ifxmips_mask_and_ack_irq(irq);
	return IRQ_HANDLED;
}

static irqreturn_t ifxmipsasc_er_int(int irq, void *_port)
{
	struct uart_port *port = (struct uart_port *)_port;
	/* clear any pending interrupts */
	ifxmips_w32(ifxmips_r32(port->membase + IFXMIPS_ASC_WHBSTATE) | ASCWHBSTATE_CLRPE |
			ASCWHBSTATE_CLRFE | ASCWHBSTATE_CLRROE, port->membase + IFXMIPS_ASC_WHBSTATE);
	return IRQ_HANDLED;
}

static irqreturn_t ifxmipsasc_rx_int(int irq, void *_port)
{
	struct uart_port *port = (struct uart_port *)_port;
	ifxmips_w32(ASC_IRNCR_RIR, port->membase + IFXMIPS_ASC_IRNCR);
	ifxmipsasc_rx_chars((struct uart_port *)port);
	ifxmips_mask_and_ack_irq(irq);
	return IRQ_HANDLED;
}

static unsigned int ifxmipsasc_tx_empty(struct uart_port *port)
{
	int status;
	status = ifxmips_r32(port->membase + IFXMIPS_ASC_FSTAT) & ASCFSTAT_TXFFLMASK;
	return status ? 0 : TIOCSER_TEMT;
}

static unsigned int ifxmipsasc_get_mctrl(struct uart_port *port)
{
	return TIOCM_CTS | TIOCM_CAR | TIOCM_DSR;
}

static void ifxmipsasc_set_mctrl(struct uart_port *port, u_int mctrl)
{
}

static void ifxmipsasc_break_ctl(struct uart_port *port, int break_state)
{
}

static int ifxmipsasc_startup(struct uart_port *port)
{
	unsigned long flags;
	int retval;

	port->uartclk = ifxmips_get_fpi_hz();

	ifxmips_w32(ifxmips_r32(port->membase + IFXMIPS_ASC_CLC) & ~IFXMIPS_ASC_CLC_DISS, port->membase + IFXMIPS_ASC_CLC);
	ifxmips_w32(((ifxmips_r32(port->membase + IFXMIPS_ASC_CLC) & ~ASCCLC_RMCMASK)) | (1 << ASCCLC_RMCOFFSET), port->membase + IFXMIPS_ASC_CLC);
	ifxmips_w32(0, port->membase + IFXMIPS_ASC_PISEL);
	ifxmips_w32(((TXFIFO_FL << ASCTXFCON_TXFITLOFF) & ASCTXFCON_TXFITLMASK) | ASCTXFCON_TXFEN | ASCTXFCON_TXFFLU, port->membase + IFXMIPS_ASC_TXFCON);
	ifxmips_w32(((RXFIFO_FL << ASCRXFCON_RXFITLOFF) & ASCRXFCON_RXFITLMASK) | ASCRXFCON_RXFEN | ASCRXFCON_RXFFLU, port->membase + IFXMIPS_ASC_RXFCON);
	wmb();
	ifxmips_w32(ifxmips_r32(port->membase + IFXMIPS_ASC_CON) | ASCCON_M_8ASYNC | ASCCON_FEN | ASCCON_TOEN | ASCCON_ROEN, port->membase + IFXMIPS_ASC_CON);

	local_irq_save(flags);

	retval = request_irq(port->irq, ifxmipsasc_tx_int, IRQF_DISABLED, "asc_tx", port);
	if (retval) {
		printk(KERN_ERR "failed to request ifxmipsasc_tx_int\n");
		return retval;
	}

	retval = request_irq(port->irq + 2, ifxmipsasc_rx_int, IRQF_DISABLED, "asc_rx", port);
	if (retval) {
		printk(KERN_ERR "failed to request ifxmipsasc_rx_int\n");
		goto err1;
	}

	retval = request_irq(port->irq + 3, ifxmipsasc_er_int, IRQF_DISABLED, "asc_er", port);
	if (retval) {
		printk(KERN_ERR "failed to request ifxmipsasc_er_int\n");
		goto err2;
	}

	ifxmips_w32(ASC_IRNREN_RX_BUF | ASC_IRNREN_TX_BUF | ASC_IRNREN_ERR | ASC_IRNREN_TX, port->membase + IFXMIPS_ASC_IRNREN);

	local_irq_restore(flags);
	return 0;

err2:
	free_irq(port->irq + 2, port);
err1:
	free_irq(port->irq, port);
	local_irq_restore(flags);
	return retval;
}

static void ifxmipsasc_shutdown(struct uart_port *port)
{
	free_irq(port->irq, port);
	free_irq(port->irq + 2, port);
	free_irq(port->irq + 3, port);

	ifxmips_w32(0, port->membase + IFXMIPS_ASC_CON);
	ifxmips_w32(ifxmips_r32(port->membase + IFXMIPS_ASC_RXFCON) | ASCRXFCON_RXFFLU, port->membase + IFXMIPS_ASC_RXFCON);
	ifxmips_w32(ifxmips_r32(port->membase + IFXMIPS_ASC_RXFCON) & ~ASCRXFCON_RXFEN, port->membase + IFXMIPS_ASC_RXFCON);
	ifxmips_w32(ifxmips_r32(port->membase + IFXMIPS_ASC_TXFCON) | ASCTXFCON_TXFFLU, port->membase + IFXMIPS_ASC_TXFCON);
	ifxmips_w32(ifxmips_r32(port->membase + IFXMIPS_ASC_TXFCON) & ~ASCTXFCON_TXFEN, port->membase + IFXMIPS_ASC_TXFCON);
}

static void ifxmipsasc_set_termios(struct uart_port *port, struct ktermios *new, struct ktermios *old)
{
	unsigned int cflag;
	unsigned int iflag;
	unsigned int quot;
	unsigned int baud;
	unsigned int con = 0;
	unsigned long flags;

	cflag = new->c_cflag;
	iflag = new->c_iflag;

	switch (cflag & CSIZE) {
	case CS7:
		con = ASCCON_M_7ASYNC;
		break;

	case CS5:
	case CS6:
	default:
		con = ASCCON_M_8ASYNC;
		break;
	}

	if (cflag & CSTOPB)
		con |= ASCCON_STP;

	if (cflag & PARENB) {
		if (!(cflag & PARODD))
			con &= ~ASCCON_ODD;
		else
			con |= ASCCON_ODD;
	}

	port->read_status_mask = ASCSTATE_ROE;
	if (iflag & INPCK)
		port->read_status_mask |= ASCSTATE_FE | ASCSTATE_PE;

	port->ignore_status_mask = 0;
	if (iflag & IGNPAR)
		port->ignore_status_mask |= ASCSTATE_FE | ASCSTATE_PE;

	if (iflag & IGNBRK) {
		/*
		 * If we're ignoring parity and break indicators,
		 * ignore overruns too (for real raw support).
		 */
		if (iflag & IGNPAR)
			port->ignore_status_mask |= ASCSTATE_ROE;
	}

	if ((cflag & CREAD) == 0)
		port->ignore_status_mask |= UART_DUMMY_UER_RX;

	/* set error signals  - framing, parity  and overrun, enable receiver */
	con |= ASCCON_FEN | ASCCON_TOEN | ASCCON_ROEN;

	local_irq_save(flags);

	/* set up CON */
	ifxmips_w32(ifxmips_r32(port->membase + IFXMIPS_ASC_CON) | con, port->membase + IFXMIPS_ASC_CON);

	/* Set baud rate - take a divider of 2 into account */
	baud = uart_get_baud_rate(port, new, old, 0, port->uartclk / 16);
	quot = uart_get_divisor(port, baud);
	quot = quot / 2 - 1;

	/* disable the baudrate generator */
	ifxmips_w32(ifxmips_r32(port->membase + IFXMIPS_ASC_CON) & ~ASCCON_R, port->membase + IFXMIPS_ASC_CON);

	/* make sure the fractional divider is off */
	ifxmips_w32(ifxmips_r32(port->membase + IFXMIPS_ASC_CON) & ~ASCCON_FDE, port->membase + IFXMIPS_ASC_CON);

	/* set up to use divisor of 2 */
	ifxmips_w32(ifxmips_r32(port->membase + IFXMIPS_ASC_CON) & ~ASCCON_BRS, port->membase + IFXMIPS_ASC_CON);

	/* now we can write the new baudrate into the register */
	ifxmips_w32(quot, port->membase + IFXMIPS_ASC_BG);

	/* turn the baudrate generator back on */
	ifxmips_w32(ifxmips_r32(port->membase + IFXMIPS_ASC_CON) | ASCCON_R, port->membase + IFXMIPS_ASC_CON);

	/* enable rx */
	ifxmips_w32(ASCWHBSTATE_SETREN, port->membase + IFXMIPS_ASC_WHBSTATE);

	local_irq_restore(flags);
}

static const char *ifxmipsasc_type(struct uart_port *port)
{
	if (port->type == PORT_IFXMIPSASC) {
		if (port->membase == (void *)IFXMIPS_ASC_BASE_ADDR)
			return "asc0";
		else
			return "asc1";
	} else {
		return NULL;
	}
}

static void ifxmipsasc_release_port(struct uart_port *port)
{
}

static int ifxmipsasc_request_port(struct uart_port *port)
{
	return 0;
}

static void ifxmipsasc_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE) {
		port->type = PORT_IFXMIPSASC;
		ifxmipsasc_request_port(port);
	}
}

static int ifxmipsasc_verify_port(struct uart_port *port, struct serial_struct *ser)
{
	int ret = 0;
	if (ser->type != PORT_UNKNOWN && ser->type != PORT_IFXMIPSASC)
		ret = -EINVAL;
	if (ser->irq < 0 || ser->irq >= NR_IRQS)
		ret = -EINVAL;
	if (ser->baud_base < 9600)
		ret = -EINVAL;
	return ret;
}

static struct uart_ops ifxmipsasc_pops = {
	.tx_empty =	ifxmipsasc_tx_empty,
	.set_mctrl =	ifxmipsasc_set_mctrl,
	.get_mctrl =	ifxmipsasc_get_mctrl,
	.stop_tx =	ifxmipsasc_stop_tx,
	.start_tx =	ifxmipsasc_start_tx,
	.stop_rx =	ifxmipsasc_stop_rx,
	.enable_ms =	ifxmipsasc_enable_ms,
	.break_ctl =	ifxmipsasc_break_ctl,
	.startup =	ifxmipsasc_startup,
	.shutdown =	ifxmipsasc_shutdown,
	.set_termios =	ifxmipsasc_set_termios,
	.type =		ifxmipsasc_type,
	.release_port =	ifxmipsasc_release_port,
	.request_port =	ifxmipsasc_request_port,
	.config_port =	ifxmipsasc_config_port,
	.verify_port =	ifxmipsasc_verify_port,
};

static struct uart_port ifxmipsasc_port[2] = {
	{
		.membase =		(void *)IFXMIPS_ASC_BASE_ADDR,
		.mapbase =		IFXMIPS_ASC_BASE_ADDR,
		.iotype =		SERIAL_IO_MEM,
		.irq =			IFXMIPSASC_TIR(0),
		.uartclk =		0,
		.fifosize =		16,
		.type =			PORT_IFXMIPSASC,
		.ops =			&ifxmipsasc_pops,
		.flags =		ASYNC_BOOT_AUTOCONF,
		.line =			0
	}, {
		.membase =		(void *)(IFXMIPS_ASC_BASE_ADDR + IFXMIPS_ASC_BASE_DIFF),
		.mapbase =		IFXMIPS_ASC_BASE_ADDR + IFXMIPS_ASC_BASE_DIFF,
		.iotype =		SERIAL_IO_MEM,
		.irq =			IFXMIPSASC_TIR(1),
		.uartclk =		0,
		.fifosize =		16,
		.type =			PORT_IFXMIPSASC,
		.ops =			&ifxmipsasc_pops,
		.flags =		ASYNC_BOOT_AUTOCONF,
		.line =			1
	}
};

static void ifxmipsasc_console_write(struct console *co, const char *s, u_int count)
{
	int port = co->index;
	int i, fifocnt;
	unsigned long flags;
	local_irq_save(flags);
	for (i = 0; i < count; i++) {
		do {
			fifocnt = (ifxmips_r32((u32 *)(IFXMIPS_ASC_BASE_ADDR + (port * IFXMIPS_ASC_BASE_DIFF) + IFXMIPS_ASC_FSTAT)) & ASCFSTAT_TXFFLMASK)
				>> ASCFSTAT_TXFFLOFF;
		} while (fifocnt == TXFIFO_FULL);

		if (s[i] == '\0')
			break;

		if (s[i] == '\n') {
			ifxmips_w32('\r', (u32 *)(IFXMIPS_ASC_BASE_ADDR + (port * IFXMIPS_ASC_BASE_DIFF) + IFXMIPS_ASC_TBUF));
			do {
				fifocnt = (ifxmips_r32((u32 *)(IFXMIPS_ASC_BASE_ADDR + (port * IFXMIPS_ASC_BASE_DIFF) + IFXMIPS_ASC_FSTAT)) & ASCFSTAT_TXFFLMASK)
					>> ASCFSTAT_TXFFLOFF;
			} while (fifocnt == TXFIFO_FULL);
		}
		ifxmips_w32(s[i], (u32 *)(IFXMIPS_ASC_BASE_ADDR + (port * IFXMIPS_ASC_BASE_DIFF) + IFXMIPS_ASC_TBUF));
	}

	local_irq_restore(flags);
}

static int __init ifxmipsasc_console_setup(struct console *co, char *options)
{
	int port = co->index;
	int baud = 115200;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';
	ifxmipsasc_port[port].uartclk = ifxmips_get_fpi_hz();
	ifxmipsasc_port[port].type = PORT_IFXMIPSASC;
	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);
	return uart_set_options(&ifxmipsasc_port[port], co, baud, parity, bits, flow);
}

static struct console ifxmipsasc_console[2] =
{
	{
		.name =		"ttyS",
		.write =		ifxmipsasc_console_write,
		.device =		uart_console_device,
		.setup =		ifxmipsasc_console_setup,
		.flags =		CON_PRINTBUFFER,
		.index =		0,
		.data =		&ifxmipsasc_reg,
	}, {
		.name =		"ttyS",
		.write =	ifxmipsasc_console_write,
		.device =	uart_console_device,
		.setup =	ifxmipsasc_console_setup,
		.flags =	CON_PRINTBUFFER,
		.index =	1,
		.data =		&ifxmipsasc_reg,
	}
};

static int __init ifxmipsasc_console_init(void)
{
	register_console(&ifxmipsasc_console[0]);
	register_console(&ifxmipsasc_console[1]);
	return 0;
}
console_initcall(ifxmipsasc_console_init);

static struct uart_driver ifxmipsasc_reg = {
	.owner =	THIS_MODULE,
	.driver_name =	"serial",
	.dev_name =	"ttyS",
	.major =	TTY_MAJOR,
	.minor =	64,
	.nr =		2,
	.cons =		&ifxmipsasc_console[1],
};

int __init ifxmipsasc_init(void)
{
	int ret;
	uart_register_driver(&ifxmipsasc_reg);
	ret = uart_add_one_port(&ifxmipsasc_reg, &ifxmipsasc_port[0]);
	ret = uart_add_one_port(&ifxmipsasc_reg, &ifxmipsasc_port[1]);
	return 0;
}

void __exit ifxmipsasc_exit(void)
{
	uart_unregister_driver(&ifxmipsasc_reg);
}

module_init(ifxmipsasc_init);
module_exit(ifxmipsasc_exit);

MODULE_AUTHOR("John Crispin <blogic@openwrt.org>");
MODULE_DESCRIPTION("MIPS IFXMips serial port driver");
MODULE_LICENSE("GPL");
