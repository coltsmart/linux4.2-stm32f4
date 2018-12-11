/*
 * Copyright (C) Maxime Coquelin 2015
 * Author:  Maxime Coquelin <mcoquelin.stm32@gmail.com>
 * License terms:  GNU General Public License (GPL), version 2
 *
 * Inspired by st-asc.c from STMicroelectronics (c)
 */

#if defined(CONFIG_SERIAL_STM32_USART_CONSOLE) && defined(CONFIG_MAGIC_SYSRQ)
#define SUPPORT_SYSRQ
#endif

#include <linux/module.h>
#include <linux/serial.h>
#include <linux/console.h>
#include <linux/sysrq.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/serial_core.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>

#define DRIVER_NAME "stm32-usart"

/* Register offsets */
#define USART_SR		0x00
#define USART_DR		0x04
#define USART_BRR		0x08
#define USART_CR1		0x0c
#define USART_CR2		0x10
#define USART_CR3		0x14
#define USART_GTPR		0x18

/* USART_SR */
#define USART_SR_PE		BIT(0)
#define USART_SR_FE		BIT(1)
#define USART_SR_NF		BIT(2)
#define USART_SR_ORE		BIT(3)
#define USART_SR_IDLE		BIT(4)
#define USART_SR_RXNE		BIT(5)
#define USART_SR_TC		BIT(6)
#define USART_SR_TXE		BIT(7)
#define USART_SR_LBD		BIT(8)
#define USART_SR_CTS		BIT(9)
#define USART_SR_ERR_MASK	(USART_SR_LBD | USART_SR_ORE | \
				 USART_SR_FE | USART_SR_PE)
/* Dummy bits */
#define USART_SR_DUMMY_RX	BIT(16)

/* USART_DR */
#define USART_DR_MASK		GENMASK(8, 0)

/* USART_BRR */
#define USART_BRR_DIV_F_MASK	GENMASK(3, 0)
#define USART_BRR_DIV_M_MASK	GENMASK(15, 4)
#define USART_BRR_DIV_M_SHIFT	4

/* USART_CR1 */
#define USART_CR1_SBK		BIT(0)
#define USART_CR1_RWU		BIT(1)
#define USART_CR1_RE		BIT(2)
#define USART_CR1_TE		BIT(3)
#define USART_CR1_IDLEIE	BIT(4)
#define USART_CR1_RXNEIE	BIT(5)
#define USART_CR1_TCIE		BIT(6)
#define USART_CR1_TXEIE		BIT(7)
#define USART_CR1_PEIE		BIT(8)
#define USART_CR1_PS		BIT(9)
#define USART_CR1_PCE		BIT(10)
#define USART_CR1_WAKE		BIT(11)
#define USART_CR1_M		BIT(12)
#define USART_CR1_UE		BIT(13)
#define USART_CR1_OVER8		BIT(15)
#define USART_CR1_IE_MASK	GENMASK(8, 4)

/* USART_CR2 */
#define USART_CR2_ADD_MASK	GENMASK(3, 0)
#define USART_CR2_LBDL		BIT(5)
#define USART_CR2_LBDIE		BIT(6)
#define USART_CR2_LBCL		BIT(8)
#define USART_CR2_CPHA		BIT(9)
#define USART_CR2_CPOL		BIT(10)
#define USART_CR2_CLKEN		BIT(11)
#define USART_CR2_STOP_2B	BIT(13)
#define USART_CR2_STOP_MASK	GENMASK(13, 12)
#define USART_CR2_LINEN		BIT(14)

/* USART_CR3 */
#define USART_CR3_EIE		BIT(0)
#define USART_CR3_IREN		BIT(1)
#define USART_CR3_IRLP		BIT(2)
#define USART_CR3_HDSEL		BIT(3)
#define USART_CR3_NACK		BIT(4)
#define USART_CR3_SCEN		BIT(5)
#define USART_CR3_DMAR		BIT(6)
#define USART_CR3_DMAT		BIT(7)
#define USART_CR3_RTSE		BIT(8)
#define USART_CR3_CTSE		BIT(9)
#define USART_CR3_CTSIE		BIT(10)
#define USART_CR3_ONEBIT	BIT(11)

/* USART_GTPR */
#define USART_GTPR_PSC_MASK	GENMASK(7, 0)
#define USART_GTPR_GT_MASK	GENMASK(15, 8)

#define DRIVER_NAME		"stm32-usart"
#define STM32_SERIAL_NAME	"ttyS"
#define STM32_MAX_PORTS		8
#define STM32_SERIAL_RINGSIZE	1024

struct stm32_uart_char {
	u32			sr;
	u32			ch;
};

struct stm32_dma_data {
	struct dma_async_tx_descriptor *dsc;
	struct dma_chan		*chan;
	dma_cookie_t		cookie;
	struct scatterlist	sg;
	spinlock_t		lock;
	bool			use;
};

struct stm32_port {
	struct uart_port	port;
	struct clk		*clk;
	bool			hw_flow_control;

	struct circ_buf		rx_ring;
	struct tasklet_struct	tasklet;

	struct stm32_dma_data	dma_rx;
	struct stm32_dma_data	dma_tx;

	int (*prepare_rx)(struct uart_port *port);
	void (*schedule_rx)(struct uart_port *port);
	void (*release_rx)(struct uart_port *port);

	int (*prepare_tx)(struct uart_port *port);
	void (*schedule_tx)(struct uart_port *port);
	void (*release_tx)(struct uart_port *port);
};

static struct stm32_port stm32_ports[STM32_MAX_PORTS];
static struct uart_driver stm32_usart_driver;

static void stm32_stop_tx(struct uart_port *port);

static inline struct stm32_port *to_stm32_port(struct uart_port *port)
{
	return container_of(port, struct stm32_port, port);
}

static bool stm32_use_dma_rx(struct uart_port *port)
{
	struct stm32_port *stm32_port = to_stm32_port(port);

	return stm32_port->dma_rx.use;
}

static bool stm32_use_dma_tx(struct uart_port *port)
{
	struct stm32_port *stm32_port = to_stm32_port(port);

	return stm32_port->dma_tx.use;
}

static void stm32_set_bits(struct uart_port *port, u32 reg, u32 bits)
{
	u32 val;

	val = readl_relaxed(port->membase + reg);
	val |= bits;
	writel_relaxed(val, port->membase + reg);
}

static void stm32_clr_bits(struct uart_port *port, u32 reg, u32 bits)
{
	u32 val;

	val = readl_relaxed(port->membase + reg);
	val &= ~bits;
	writel_relaxed(val, port->membase + reg);
}

static void stm32_receive_chars(struct uart_port *port)
{
	struct stm32_port *stm32_port = to_stm32_port(port);
	struct circ_buf *ring = &stm32_port->rx_ring;
	struct stm32_uart_char *c;
	u32 sr, ch;

	while ((sr = readl_relaxed(port->membase + USART_SR)) & USART_SR_RXNE) {
		ch = readl_relaxed(port->membase + USART_DR);

		if (!CIRC_SPACE(ring->head, ring->tail, STM32_SERIAL_RINGSIZE)) {
			/* Buffer overflow, ignore char */
			continue;
		}

		c = &((struct stm32_uart_char *)ring->buf)[ring->head];
		c->sr = sr;
		c->ch = ch;

		/* Make sure the character is stored before we update head. */
		smp_wmb();
		ring->head = (ring->head + 1) & (STM32_SERIAL_RINGSIZE - 1);
	}

	tasklet_schedule(&stm32_port->tasklet);
}

static void stm32_rx_from_ring(struct uart_port *port)
{
	struct stm32_port *stm32_port = to_stm32_port(port);
	struct circ_buf *ring = &stm32_port->rx_ring;
	struct tty_port *tport = &port->state->port;
	u32 sr;
	char flag;

	if (port->irq_wake)
		pm_wakeup_event(tport->tty->dev, 0);

	while (ring->head != ring->tail) {
		struct stm32_uart_char	c;

		/* Make sure c is loaded after head */
		smp_rmb();

		c = ((struct stm32_uart_char *)ring->buf)[ring->tail];
		ring->tail = (ring->tail + 1) & (STM32_SERIAL_RINGSIZE - 1);

		port->icount.rx++;
		sr = c.sr | USART_SR_DUMMY_RX;
		flag = TTY_NORMAL;

		if (sr & USART_SR_ERR_MASK) {
			if (sr & USART_SR_LBD) {
				port->icount.brk++;
				if (uart_handle_break(port))
					continue;
			} else if (sr & USART_SR_ORE) {
				port->icount.overrun++;
			} else if (sr & USART_SR_PE) {
				port->icount.parity++;
			} else if (sr & USART_SR_FE) {
				port->icount.frame++;
			}

			sr &= port->read_status_mask;

			if (sr & USART_SR_LBD)
				flag = TTY_BREAK;
			else if (sr & USART_SR_PE)
				flag = TTY_PARITY;
			else if (sr & USART_SR_FE)
				flag = TTY_FRAME;
		}

		if (uart_handle_sysrq_char(port, c.ch))
			continue;
		uart_insert_char(port, sr, USART_SR_ORE, c.ch, flag);
	}

	spin_unlock(&port->lock);
	tty_flip_buffer_push(tport);
	spin_lock(&port->lock);
}

static void stm32_transmit_chars(struct uart_port *port)
{
	struct circ_buf *xmit = &port->state->xmit;

	if (port->x_char) {
		writel_relaxed(port->x_char, port->membase + USART_DR);
		port->x_char = 0;
		port->icount.tx++;
		return;
	}

	if (uart_circ_empty(xmit) || uart_tx_stopped(port)) {
		stm32_stop_tx(port);
		return;
	}

	writel_relaxed(xmit->buf[xmit->tail], port->membase + USART_DR);
	stm32_set_bits(port, USART_CR1, USART_CR1_TXEIE);
	xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
	port->icount.tx++;

	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(port);

	if (uart_circ_empty(xmit))
		stm32_stop_tx(port);
}

static irqreturn_t stm32_interrupt(int irq, void *ptr)
{
	struct uart_port *port = ptr;
	struct stm32_port *stm32_port = to_stm32_port(port);
	u32 sr;

	spin_lock(&port->lock);

	sr = readl_relaxed(port->membase + USART_SR);

	if (stm32_use_dma_rx(port)) {
		if (sr & USART_SR_IDLE) {
			/* Read DR to clear IDLE interrupt */
			readl_relaxed(port->membase + USART_DR);
			tasklet_schedule(&stm32_port->tasklet);
		}
	}

	if (sr & USART_SR_RXNE)
		stm32_receive_chars(port);

	if (sr & USART_SR_TXE) {
		stm32_clr_bits(port, USART_CR1, USART_CR1_TXEIE);
		tasklet_schedule(&stm32_port->tasklet);
	}

	spin_unlock(&port->lock);

	return IRQ_HANDLED;
}

static unsigned int stm32_tx_empty(struct uart_port *port)
{
	return readl_relaxed(port->membase + USART_SR) & USART_SR_TXE;
}

static void stm32_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	if ((mctrl & TIOCM_RTS) && (port->status & UPSTAT_AUTORTS))
		stm32_set_bits(port, USART_CR3, USART_CR3_RTSE);
	else
		stm32_clr_bits(port, USART_CR3, USART_CR3_RTSE);
}

static unsigned int stm32_get_mctrl(struct uart_port *port)
{
	/* This routine is used to get signals of: DCD, DSR, RI, and CTS */
	return TIOCM_CAR | TIOCM_DSR | TIOCM_CTS;
}

/* Transmit stop */
static void stm32_stop_tx(struct uart_port *port)
{
	stm32_clr_bits(port, USART_CR1, USART_CR1_TXEIE);
}

/* There are probably characters waiting to be transmitted. */
static void stm32_start_tx(struct uart_port *port)
{
	struct circ_buf *xmit = &port->state->xmit;

	if (uart_circ_empty(xmit))
		return;

	stm32_set_bits(port, USART_CR1, USART_CR1_TXEIE);
}

/* Throttle the remote when input buffer is about to overflow. */
static void stm32_throttle(struct uart_port *port)
{
	unsigned long flags;

	spin_lock_irqsave(&port->lock, flags);
	stm32_clr_bits(port, USART_CR1, USART_CR1_RE);
	spin_unlock_irqrestore(&port->lock, flags);
}

/* Unthrottle the remote, the input buffer can now accept data. */
static void stm32_unthrottle(struct uart_port *port)
{
	unsigned long flags;

	spin_lock_irqsave(&port->lock, flags);
	stm32_set_bits(port, USART_CR1, USART_CR1_RE);
	spin_unlock_irqrestore(&port->lock, flags);
}

/* Receive stop */
static void stm32_stop_rx(struct uart_port *port)
{
	stm32_clr_bits(port, USART_CR1, USART_CR1_RE);
}

/* Handle breaks - ignored by us */
static void stm32_break_ctl(struct uart_port *port, int break_state)
{
}

static void stm32_complete_rx_dma(void *arg)
{
	struct uart_port *port = arg;
	struct stm32_port *stm32_port = to_stm32_port(port);

	tasklet_schedule(&stm32_port->tasklet);
}

static void stm32_rx_from_dma(struct uart_port *port)
{
	struct stm32_port *stm32_port = to_stm32_port(port);
	struct stm32_dma_data *dma = &stm32_port->dma_rx;
	struct tty_port *tport = &port->state->port;
	struct circ_buf *ring = &stm32_port->rx_ring;
	struct dma_chan *chan = dma->chan;
	struct dma_tx_state state;
	enum dma_status dmastat;
	size_t count;

	dmastat = dmaengine_tx_status(chan, dma->cookie, &state);
	/* Restart a new tasklet if DMA status is error */
	if (dmastat == DMA_ERROR) {
		dev_dbg(port->dev, "Get residue error, restart tasklet\n");
		tasklet_schedule(&stm32_port->tasklet);
		return;
	}

	/* CPU claims ownership of RX DMA buffer */
	dma_sync_sg_for_cpu(port->dev, &dma->sg, 1, DMA_FROM_DEVICE);

	/*
	 * ring->head points to the end of data already written by the DMA.
	 * ring->tail points to the beginning of data to be read by the
	 * framework.
	 * The current transfer size should not be larger than the dma buffer
	 * length.
	 */
	ring->head = sg_dma_len(&dma->sg) - state.residue;
	BUG_ON(ring->head > sg_dma_len(&dma->sg));
	/*
	 * At this point ring->head may point to the first byte right after the
	 * last byte of the dma buffer:
	 * 0 <= ring->head <= sg_dma_len(&stm32_port->sg_rx)
	 *
	 * However ring->tail must always points inside the dma buffer:
	 * 0 <= ring->tail <= sg_dma_len(&stm32_port->sg_rx) - 1
	 *
	 * Since we use a ring buffer, we have to handle the case
	 * where head is lower than tail. In such a case, we first read from
	 * tail to the end of the buffer then reset tail.
	 */
	if (ring->head < ring->tail) {
		count = sg_dma_len(&dma->sg) - ring->tail;

		tty_insert_flip_string(tport, ring->buf + ring->tail, count);
		ring->tail = 0;
		port->icount.rx += count;
	}

	/* Finally we read data from tail to head */
	if (ring->tail < ring->head) {
		count = ring->head - ring->tail;

		tty_insert_flip_string(tport, ring->buf + ring->tail, count);
		/* Wrap ring->head if needed */
		if (ring->head >= sg_dma_len(&dma->sg))
			ring->head = 0;
		ring->tail = ring->head;
		port->icount.rx += count;
	}

	/* USART retreives ownership of RX DMA buffer */
	dma_sync_sg_for_device(port->dev, &dma->sg, 1, DMA_FROM_DEVICE);

	/*
	 * Drop the lock here since it might end up calling
	 * uart_start(), which takes the lock.
	 */
	spin_unlock(&port->lock);
	tty_flip_buffer_push(tport);
	spin_lock(&port->lock);
}

static void stm32_release_rx_dma(struct uart_port *port)
{
	struct stm32_port *stm32_port = to_stm32_port(port);
	struct stm32_dma_data *dma = &stm32_port->dma_rx;
	struct dma_chan *chan = dma->chan;

	if (chan) {
		dmaengine_terminate_all(chan);
		dma_release_channel(chan);
		dma_unmap_sg(port->dev, &dma->sg, 1, DMA_FROM_DEVICE);
	}

	dma->dsc = NULL;
	dma->chan = NULL;
	dma->cookie = -EINVAL;
}

static int stm32_prepare_rx_dma(struct uart_port *port)
{
	struct stm32_port *stm32_port = to_stm32_port(port);
	struct stm32_dma_data *dma = &stm32_port->dma_rx;
	struct dma_async_tx_descriptor *dsc;
	struct circ_buf *ring;
	dma_cap_mask_t mask;
	struct dma_slave_config config;
	int ret, nent;

	ring = &stm32_port->rx_ring;

	dma_cap_zero(mask);
	dma_cap_set(DMA_CYCLIC, mask);

	dma->chan = dma_request_slave_channel(port->dev, "rx");
	if (!dma->chan)
		goto chan_err;

	spin_lock_init(&dma->lock);
	sg_init_table(&dma->sg, 1);
	BUG_ON(!PAGE_ALIGNED(ring->buf));
	sg_set_page(&dma->sg, virt_to_page(ring->buf),
		    sizeof(struct stm32_uart_char) * STM32_SERIAL_RINGSIZE,
		    (int)ring->buf & ~PAGE_MASK);

	nent = dma_map_sg(port->dev, &dma->sg, 1, DMA_FROM_DEVICE);
	if (!nent) {
		dev_dbg(port->dev, "need to release resource of dma\n");
		goto chan_err;
	}
	dev_dbg(port->dev, "%s: mapped %d@%p to %x\n", __func__,
		sg_dma_len(&dma->sg), ring->buf, sg_dma_address(&dma->sg));

	/* Configure the slave DMA */
	memset(&config, 0, sizeof(config));
	config.direction = DMA_DEV_TO_MEM;
	config.src_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
	config.dst_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
	config.src_addr = port->mapbase + USART_DR;
	config.src_maxburst = 1;
	ret = dmaengine_slave_config(dma->chan, &config);
	if (ret) {
		dev_err(port->dev, "DMA rx slave configuration failed\n");
		goto chan_err;
	}

	/*
	 * Prepare a cyclic dma transfer with half ring buffer size
	 * period
	 */
	dsc = dmaengine_prep_dma_cyclic(dma->chan, sg_dma_address(&dma->sg),
				sg_dma_len(&dma->sg), sg_dma_len(&dma->sg) / 2,
				DMA_DEV_TO_MEM, DMA_PREP_INTERRUPT);
	dsc->callback = stm32_complete_rx_dma;
	dsc->callback_param = port;
	dma->dsc = dsc;
	dma->cookie = dmaengine_submit(dsc);

	dev_dbg(port->dev, "using %s for rx DMA transfers\n",
		 dma_chan_name(dma->chan));
	return 0;
chan_err:
	dev_err(port->dev, "DMA RX channel not available, switch to pio\n");
	dma->use = 0;
	if (dma->chan)
		stm32_release_rx_dma(port);
	return -EINVAL;
}

static void stm32_complete_tx_dma(void *arg)
{
	struct stm32_port *stm32_port = arg;
	struct uart_port *port = &stm32_port->port;
	struct circ_buf *xmit = &port->state->xmit;
	struct stm32_dma_data *dma = &stm32_port->dma_tx;
	struct dma_chan *chan = dma->chan;
	unsigned long flags;

	spin_lock_irqsave(&port->lock, flags);
	if (chan)
		dmaengine_terminate_all(chan);
	xmit->tail += sg_dma_len(&dma->sg);
	xmit->tail &= UART_XMIT_SIZE - 1;

	port->icount.tx += sg_dma_len(&dma->sg);

	spin_lock_irq(&dma->lock);
	async_tx_ack(dma->dsc);
	dma->cookie = -EINVAL;
	dma->dsc = NULL;
	spin_unlock_irq(&dma->lock);
	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(port);

	/*
	 * xmit is a circular buffer so, if we have just send data from
	 * xmit->tail to the end of xmit->buf, now we have to transmit the
	 * remaining data from the beginning of xmit->buf to xmit->head.
	 */
	if (!uart_circ_empty(xmit))
		tasklet_schedule(&stm32_port->tasklet);

	spin_unlock_irqrestore(&port->lock, flags);
}

/*
 * Called from tasklet with TXRDY interrupt is disabled.
 */
static void stm32_tx_with_dma(struct uart_port *port)
{
	struct stm32_port *stm32_port = to_stm32_port(port);
	struct circ_buf *xmit = &port->state->xmit;
	struct stm32_dma_data *dma = &stm32_port->dma_tx;
	struct dma_chan *chan = dma->chan;
	struct dma_async_tx_descriptor *dsc;
	struct scatterlist *sg = &dma->sg;

	/* Make sure we have an idle channel */
	if (dma->dsc)
		return;

	if (!uart_circ_empty(xmit) && !uart_tx_stopped(port)) {
		/*
		 * DMA is idle now.
		 * Port xmit buffer is already mapped,
		 * and it is one page... Just adjust
		 * offsets and lengths. Since it is a circular buffer,
		 * we have to transmit till the end, and then the rest.
		 * Take the port lock to get a
		 * consistent xmit buffer state.
		 */
		sg->offset = xmit->tail & (UART_XMIT_SIZE - 1);
		sg_dma_address(sg) = (sg_dma_address(sg) &
					~(UART_XMIT_SIZE - 1))
					+ sg->offset;
		sg_dma_len(sg) = CIRC_CNT_TO_END(xmit->head,
						xmit->tail,
						UART_XMIT_SIZE);
		BUG_ON(!sg_dma_len(sg));

		dsc = dmaengine_prep_slave_sg(chan, sg, 1,
			DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
		if (!dsc) {
			dev_err(port->dev, "Failed to send via dma!\n");
			return;
		}

		dma_sync_sg_for_device(port->dev, sg, 1, DMA_TO_DEVICE);
		dma->dsc = dsc;
		dsc->callback = stm32_complete_tx_dma;
		dsc->callback_param = stm32_port;
		dsc->cookie = dmaengine_submit(dsc);
	}

	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(port);
}

static void stm32_release_tx_dma(struct uart_port *port)
{
	struct stm32_port *stm32_port = to_stm32_port(port);
	struct stm32_dma_data *dma = &stm32_port->dma_tx;
	struct dma_chan *chan = dma->chan;

	if (chan) {
		dmaengine_terminate_all(chan);
		dma_release_channel(chan);
		dma_unmap_sg(port->dev, &dma->sg, 1, DMA_TO_DEVICE);
	}

	dma->dsc = NULL;
	dma->chan = NULL;
	dma->cookie = -EINVAL;
}

static int stm32_prepare_tx_dma(struct uart_port *port)
{
	struct stm32_port *stm32_port = to_stm32_port(port);
	struct stm32_dma_data *dma = &stm32_port->dma_tx;
	dma_cap_mask_t mask;
	struct dma_slave_config config;
	int ret, nent;

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);

	dma->chan = dma_request_slave_channel(port->dev, "tx");
	if (!dma->chan)
		goto chan_err;
	dev_dbg(port->dev, "using %s for tx DMA transfers\n",
		dma_chan_name(dma->chan));

	spin_lock_init(&dma->lock);
	sg_init_table(&dma->sg, 1);
	/* UART circular tx buffer is an aligned page. */
	BUG_ON(!PAGE_ALIGNED(port->state->xmit.buf));
	sg_set_page(&dma->sg, virt_to_page(port->state->xmit.buf),
		    UART_XMIT_SIZE, (int)port->state->xmit.buf & ~PAGE_MASK);
	nent = dma_map_sg(port->dev, &dma->sg, 1, DMA_TO_DEVICE);
	if (!nent) {
		dev_dbg(port->dev, "need to release resource of dma\n");
		goto chan_err;
	} else {
		dev_dbg(port->dev, "%s: mapped %d@%p to %x\n", __func__,
			sg_dma_len(&dma->sg),
			port->state->xmit.buf,
			sg_dma_address(&dma->sg));
	}

	/* Configure the slave DMA */
	memset(&config, 0, sizeof(config));
	config.direction = DMA_MEM_TO_DEV;
	config.dst_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
	config.src_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
	config.dst_addr = port->mapbase + USART_DR;
	config.dst_maxburst = 1;

	ret = dmaengine_slave_config(dma->chan, &config);
	if (ret) {
		dev_err(port->dev, "DMA tx slave configuration failed\n");
		goto chan_err;
	}

	return 0;

chan_err:
	dev_err(port->dev, "TX channel not available, switch to pio\n");
	dma->use = 0;
	if (dma->chan)
		stm32_release_tx_dma(port);
	return -EINVAL;
}

static void stm32_set_ops(struct uart_port *port)
{
	struct stm32_port	*stm32_port = to_stm32_port(port);

	if (stm32_use_dma_rx(port)) {
		stm32_port->prepare_rx = stm32_prepare_rx_dma;
		stm32_port->schedule_rx = stm32_rx_from_dma;
		stm32_port->release_rx = stm32_release_rx_dma;
	} else {
		stm32_port->prepare_rx = NULL;
		stm32_port->schedule_rx = stm32_rx_from_ring;
		stm32_port->release_rx = NULL;
	}

	if (stm32_use_dma_tx(port)) {
		stm32_port->prepare_tx = stm32_prepare_tx_dma;
		stm32_port->schedule_tx = stm32_tx_with_dma;
		stm32_port->release_tx = stm32_release_tx_dma;
	} else {
		stm32_port->prepare_tx = NULL;
		stm32_port->schedule_tx = stm32_transmit_chars;
		stm32_port->release_tx = NULL;
	}
}

/*
 * tasklet handling tty stuff outside the interrupt handler.
 */
static void stm32_tasklet_func(unsigned long data)
{
	struct uart_port *port = (struct uart_port *)data;
	struct stm32_port *stm32_port = to_stm32_port(port);

	/* The interrupt handler does not take the lock */
	spin_lock(&port->lock);

	stm32_port->schedule_tx(port);
	stm32_port->schedule_rx(port);

	spin_unlock(&port->lock);
}

static void stm32_init_property(struct uart_port *port,
				struct platform_device *pdev)
{
	struct stm32_port *stm32_port = to_stm32_port(port);
	struct device_node *np = pdev->dev.of_node;

	/*
	 * Don't use DMA by defaul
	 */
	stm32_port->dma_rx.use = false;
	stm32_port->dma_tx.use = false;

	if (np) {
		if (of_get_property(np, "st,use-dma-rx", NULL) &&
		    of_get_property(np, "dmas", NULL)) {
			stm32_port->dma_rx.use  = true;
		}

		if (of_get_property(np, "st,use-dma-tx", NULL) &&
		    of_get_property(np, "dmas", NULL)) {
			stm32_port->dma_tx.use = true;
		}
	}
}

static int stm32_startup(struct uart_port *port)
{
	struct platform_device *pdev = to_platform_device(port->dev);
	struct stm32_port *stm32_port = to_stm32_port(port);
	const char *name = pdev->name;
	u32 cr1, cr3;
	int ret;

	ret = request_irq(port->irq, stm32_interrupt, IRQF_NO_SUSPEND,
			  name, port);
	if (ret)
		return ret;

	tasklet_enable(&stm32_port->tasklet);

	/*
	 * Initialize DMA (if necessary)
	 */
	stm32_init_property(port, pdev);
	stm32_set_ops(port);

	if (stm32_port->prepare_rx) {
		ret = stm32_port->prepare_rx(port);
		if (ret < 0)
			stm32_set_ops(port);
	}
	if (stm32_port->prepare_tx) {
		ret = stm32_port->prepare_tx(port);
		if (ret < 0)
			stm32_set_ops(port);
	}

	/*
	 * Init UART
	 */
	cr1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
	if (stm32_use_dma_rx(port))
		cr1 |= USART_CR1_IDLEIE;
	else
		cr1 |= USART_CR1_RXNEIE;

	cr3 = 0;
	if (stm32_port->dma_rx.use)
		cr3 |= USART_CR3_DMAR;
	if (stm32_port->dma_tx.use)
		cr3 |= USART_CR3_DMAT;

	stm32_set_bits(port, USART_CR1, cr1);
	stm32_set_bits(port, USART_CR3, cr3);

	return 0;
}

static void stm32_shutdown(struct uart_port *port)
{
	struct stm32_port *stm32_port = to_stm32_port(port);

	/*
	 * Clear out any scheduled tasklets before
	 * we destroy the buffers
	 */
	tasklet_disable(&stm32_port->tasklet);
	tasklet_kill(&stm32_port->tasklet);

	/*
	 * Shut-down serial
	 */
	writel_relaxed(0, port->membase + USART_CR1);

	/*
	 * Shut-down the DMA
	 */
	if (stm32_port->release_rx)
		stm32_port->release_rx(port);
	if (stm32_port->release_tx)
		stm32_port->release_tx(port);

	stm32_port->rx_ring.head = 0;
	stm32_port->rx_ring.tail = 0;

	free_irq(port->irq, port);
}

static void stm32_set_termios(struct uart_port *port, struct ktermios *termios,
			    struct ktermios *old)
{
	struct stm32_port *stm32_port = to_stm32_port(port);
	unsigned int baud;
	u32 usartdiv, mantissa, fraction, oversampling;
	tcflag_t cflag = termios->c_cflag;
	u32 cr1, cr2, cr3;
	unsigned long flags;

	if (!stm32_port->hw_flow_control)
		cflag &= ~CRTSCTS;

	baud = uart_get_baud_rate(port, termios, old, 0, port->uartclk / 8);

	spin_lock_irqsave(&port->lock, flags);

	cr1 = readl_relaxed(port->membase + USART_CR1);
	cr2 = readl_relaxed(port->membase + USART_CR2);
	cr3 = readl_relaxed(port->membase + USART_CR3);

	if (cflag & CSTOPB)
		cr2 |= USART_CR2_STOP_2B;

	if (cflag & PARENB) {
		cr1 |= USART_CR1_PCE;
		if ((cflag & CSIZE) == CS8)
			cr1 |= USART_CR1_M;
	}

	if (cflag & PARODD)
		cr1 |= USART_CR1_PS;

	port->status &= ~(UPSTAT_AUTOCTS | UPSTAT_AUTORTS);
	if (cflag & CRTSCTS) {
		port->status |= UPSTAT_AUTOCTS | UPSTAT_AUTORTS;
		cr3 |= USART_CR3_CTSE;
	}

	usartdiv = DIV_ROUND_CLOSEST(port->uartclk, baud);

	/*
	 * The USART supports 16 or 8 times oversampling.
	 * By default we prefer 16 times oversampling, so that the receiver
	 * has a better tolerance to clock deviations.
	 * 8 times oversampling is only used to achieve higher speeds.
	 */
	if (usartdiv < 16) {
		oversampling = 8;
		stm32_set_bits(port, USART_CR1, USART_CR1_OVER8);
	} else {
		oversampling = 16;
		stm32_clr_bits(port, USART_CR1, USART_CR1_OVER8);
	}

	mantissa = (usartdiv / oversampling) << USART_BRR_DIV_M_SHIFT;
	fraction = usartdiv % oversampling;
	writel_relaxed(mantissa | fraction, port->membase + USART_BRR);

	uart_update_timeout(port, cflag, baud);

	port->read_status_mask = USART_SR_ORE;
	if (termios->c_iflag & INPCK)
		port->read_status_mask |= USART_SR_PE | USART_SR_FE;
	if (termios->c_iflag & (IGNBRK | BRKINT | PARMRK))
		port->read_status_mask |= USART_SR_LBD;

	/* Characters to ignore */
	port->ignore_status_mask = 0;
	if (termios->c_iflag & IGNPAR)
		port->ignore_status_mask = USART_SR_PE | USART_SR_FE;
	if (termios->c_iflag & IGNBRK) {
		port->ignore_status_mask |= USART_SR_LBD;
		/*
		 * If we're ignoring parity and break indicators,
		 * ignore overruns too (for real raw support).
		 */
		if (termios->c_iflag & IGNPAR)
			port->ignore_status_mask |= USART_SR_ORE;
	}

	/* Ignore all characters if CREAD is not set */
	if ((termios->c_cflag & CREAD) == 0)
		port->ignore_status_mask |= USART_SR_DUMMY_RX;

	writel_relaxed(cr1, port->membase + USART_CR1);
	writel_relaxed(cr2, port->membase + USART_CR2);
	writel_relaxed(cr3, port->membase + USART_CR3);

	spin_unlock_irqrestore(&port->lock, flags);
}

static const char *stm32_type(struct uart_port *port)
{
	return (port->type == PORT_STM32) ? DRIVER_NAME : NULL;
}

static void stm32_release_port(struct uart_port *port)
{
}

static int stm32_request_port(struct uart_port *port)
{
	return 0;
}

static void stm32_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_STM32;
}

static int
stm32_verify_port(struct uart_port *port, struct serial_struct *ser)
{
	/* No user changeable parameters */
	return -EINVAL;
}

static void stm32_pm(struct uart_port *port, unsigned int state,
		unsigned int oldstate)
{
	struct stm32_port *stm32port = container_of(port,
			struct stm32_port, port);
	unsigned long flags = 0;

	switch (state) {
	case UART_PM_STATE_ON:
		clk_prepare_enable(stm32port->clk);
		break;
	case UART_PM_STATE_OFF:
		spin_lock_irqsave(&port->lock, flags);
		stm32_clr_bits(port, USART_CR1, USART_CR1_UE);
		spin_unlock_irqrestore(&port->lock, flags);
		clk_disable_unprepare(stm32port->clk);
		break;
	}
}

static const struct uart_ops stm32_uart_ops = {
	.tx_empty	= stm32_tx_empty,
	.set_mctrl	= stm32_set_mctrl,
	.get_mctrl	= stm32_get_mctrl,
	.stop_tx	= stm32_stop_tx,
	.start_tx	= stm32_start_tx,
	.throttle	= stm32_throttle,
	.unthrottle	= stm32_unthrottle,
	.stop_rx	= stm32_stop_rx,
	.break_ctl	= stm32_break_ctl,
	.startup	= stm32_startup,
	.shutdown	= stm32_shutdown,
	.set_termios	= stm32_set_termios,
	.pm		= stm32_pm,
	.type		= stm32_type,
	.release_port	= stm32_release_port,
	.request_port	= stm32_request_port,
	.config_port	= stm32_config_port,
	.verify_port	= stm32_verify_port,
};

static int stm32_init_port(struct stm32_port *stm32port,
			  struct platform_device *pdev)
{
	struct uart_port *port = &stm32port->port;
	struct resource *res;
	int ret;

	port->iotype	= UPIO_MEM;
	port->flags	= UPF_BOOT_AUTOCONF;
	port->ops	= &stm32_uart_ops;
	port->dev	= &pdev->dev;
	port->irq	= platform_get_irq(pdev, 0);

	tasklet_init(&stm32port->tasklet, stm32_tasklet_func,
		     (unsigned long)port);
	tasklet_disable(&stm32port->tasklet);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	port->membase = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(port->membase))
		return PTR_ERR(port->membase);
	port->mapbase = res->start;

	spin_lock_init(&port->lock);

	stm32port->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(stm32port->clk))
		return PTR_ERR(stm32port->clk);

	/* Ensure that clk rate is correct by enabling the clk */
	ret = clk_prepare_enable(stm32port->clk);
	if (ret)
		return ret;

	stm32port->port.uartclk = clk_get_rate(stm32port->clk);
	if (!stm32port->port.uartclk)
		ret = -EINVAL;

	/*
	 * Don't stop clocks if early printk is on, to avoid locking in
	 * one of waituart()/busyuart() polls.
	 */
#ifndef CONFIG_EARLY_PRINTK
	clk_disable_unprepare(stm32port->clk);
#endif

	return ret;
}

static struct stm32_port *stm32_of_get_stm32_port(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	int id;

	if (!np)
		return NULL;

	id = of_alias_get_id(np, "serial");
	if (id < 0)
		id = 0;

	if (WARN_ON(id >= STM32_MAX_PORTS))
		return NULL;

	stm32_ports[id].hw_flow_control = of_property_read_bool(np,
							"auto-flow-control");
	stm32_ports[id].port.line = id;
	return &stm32_ports[id];
}

#ifdef CONFIG_OF
static const struct of_device_id stm32_match[] = {
	{ .compatible = "st,stm32-usart", },
	{ .compatible = "st,stm32-uart", },
	{},
};

MODULE_DEVICE_TABLE(of, stm32_match);
#endif

static int stm32_serial_probe(struct platform_device *pdev)
{
	int ret;
	struct stm32_port *stm32port;

	stm32port = stm32_of_get_stm32_port(pdev);
	if (!stm32port)
		return -ENODEV;

	ret = stm32_init_port(stm32port, pdev);
	if (ret)
		return ret;

	stm32port->rx_ring.buf = kmalloc(sizeof(struct stm32_uart_char) *
					 STM32_SERIAL_RINGSIZE, GFP_KERNEL);
	if (!stm32port->rx_ring.buf)
		return -ENOMEM;

	ret = uart_add_one_port(&stm32_usart_driver, &stm32port->port);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, &stm32port->port);

	return 0;
}

static int stm32_serial_remove(struct platform_device *pdev)
{
	struct uart_port *port = platform_get_drvdata(pdev);
	struct stm32_port *stm32_port = to_stm32_port(port);
	int ret;

	tasklet_kill(&stm32_port->tasklet);

	ret = uart_remove_one_port(&stm32_usart_driver, port);

	kfree(stm32_port->rx_ring.buf);

	return ret;
}


#ifdef CONFIG_SERIAL_STM32_CONSOLE
static void stm32_console_putchar(struct uart_port *port, int ch)
{
	while (!(readl_relaxed(port->membase + USART_SR) & USART_SR_TXE))
		cpu_relax();

	writel_relaxed(ch, port->membase + USART_DR);
}

static void stm32_console_write(struct console *co, const char *s, unsigned cnt)
{
	struct uart_port *port = &stm32_ports[co->index].port;
	unsigned long flags;
	u32 old_cr1, new_cr1;
	int locked = 1;

	local_irq_save(flags);
	if (port->sysrq)
		locked = 0;
	else if (oops_in_progress)
		locked = spin_trylock(&port->lock);
	else
		spin_lock(&port->lock);

	/* Save and disable interrupts */
	old_cr1 = readl_relaxed(port->membase + USART_CR1);
	new_cr1 = old_cr1 & ~USART_CR1_IE_MASK;
	writel_relaxed(new_cr1, port->membase + USART_CR1);

	uart_console_write(port, s, cnt, stm32_console_putchar);

	/* Restore interrupt state */
	writel_relaxed(old_cr1, port->membase + USART_CR1);

	if (locked)
		spin_unlock(&port->lock);
	local_irq_restore(flags);
}

static int stm32_console_setup(struct console *co, char *options)
{
	struct stm32_port *stm32port;
	int baud = 9600;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';

	if (co->index >= STM32_MAX_PORTS)
		return -ENODEV;

	stm32port = &stm32_ports[co->index];

	/*
	 * This driver does not support early console initialization
	 * (use ARM early printk support instead), so we only expect
	 * this to be called during the uart port registration when the
	 * driver gets probed and the port should be mapped at that point.
	 */
	if (stm32port->port.mapbase == 0 || stm32port->port.membase == NULL)
		return -ENXIO;

	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);

	return uart_set_options(&stm32port->port, co, baud, parity, bits, flow);
}

static struct console stm32_console = {
	.name		= STM32_SERIAL_NAME,
	.device		= uart_console_device,
	.write		= stm32_console_write,
	.setup		= stm32_console_setup,
	.flags		= CON_PRINTBUFFER,
	.index		= -1,
	.data		= &stm32_usart_driver,
};

#define STM32_SERIAL_CONSOLE (&stm32_console)

#else
#define STM32_SERIAL_CONSOLE NULL
#endif /* CONFIG_SERIAL_STM32_CONSOLE */

static struct uart_driver stm32_usart_driver = {
	.driver_name	= DRIVER_NAME,
	.dev_name	= STM32_SERIAL_NAME,
	.major		= TTY_MAJOR,
	.minor		= 64,
	.nr		= STM32_MAX_PORTS,
	.cons		= STM32_SERIAL_CONSOLE,
};

static struct platform_driver stm32_serial_driver = {
	.probe		= stm32_serial_probe,
	.remove		= stm32_serial_remove,
	.driver	= {
		.name	= DRIVER_NAME,
		.of_match_table = of_match_ptr(stm32_match),
	},
};

static int __init usart_init(void)
{
	static char banner[] __initdata = "STM32 USART driver initialized";
	int ret;

	pr_info("%s\n", banner);

	ret = uart_register_driver(&stm32_usart_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&stm32_serial_driver);
	if (ret)
		uart_unregister_driver(&stm32_usart_driver);

	return ret;
}

static void __exit usart_exit(void)
{
	platform_driver_unregister(&stm32_serial_driver);
	uart_unregister_driver(&stm32_usart_driver);
}

module_init(usart_init);
module_exit(usart_exit);

MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_DESCRIPTION("STMicroelectronics STM32 serial port driver");
MODULE_LICENSE("GPL v2");
