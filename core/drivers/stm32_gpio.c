// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2017-2024, STMicroelectronics
 *
 * STM32 GPIO driver is used as pin controller for stm32mp SoCs.
 */

#include <assert.h>
#include <compiler.h>
#include <drivers/clk.h>
#include <drivers/clk_dt.h>
#include <drivers/firewall.h>
#include <drivers/gpio.h>
#include <drivers/pinctrl.h>
#include <drivers/stm32_gpio.h>
#include <drivers/stm32_rif.h>
#include <dt-bindings/gpio/stm32mp_gpio.h>
#include <dt-bindings/pinctrl/stm32-pinfunc.h>
#include <io.h>
#include <keep.h>
#include <kernel/dt.h>
#include <kernel/boot.h>
#include <kernel/panic.h>
#include <kernel/pm.h>
#include <kernel/spinlock.h>
#include <libfdt.h>
#include <mm/core_memprot.h>
#include <stdbool.h>
#include <stdint.h>
#include <stm32_util.h>
#include <sys/queue.h>
#include <trace.h>
#include <util.h>

#ifndef CFG_DRIVERS_GPIO
#error stm32_gpio driver expects CFG_DRIVERS_GPIO
#endif

#define GPIO_PIN_MAX		15

#define GPIO_MODER_OFFSET	U(0x00)
#define GPIO_OTYPER_OFFSET	U(0x04)
#define GPIO_OSPEEDR_OFFSET	U(0x08)
#define GPIO_PUPDR_OFFSET	U(0x0c)
#define GPIO_IDR_OFFSET		U(0x10)
#define GPIO_ODR_OFFSET		U(0x14)
#define GPIO_BSRR_OFFSET	U(0x18)
#define GPIO_AFRL_OFFSET	U(0x20)
#define GPIO_AFRH_OFFSET	U(0x24)
#define GPIO_SECR_OFFSET	U(0x30)
#define GPIO_PRIVCFGR_OFFSET	U(0x34)
#define GPIO_RCFGLOCKR_OFFSET	U(0x38)
#define GPIO_CIDCFGR(x)		(U(0x50) + U(0x8) * (x))
#define GPIO_SEMCR(x)		(U(0x54) + U(0x8) * (x))

#define GPIO_ALT_LOWER_LIMIT	U(0x8)

/*
 * CIDCFGR register bitfields
 */
#define GPIO_CIDCFGR_SEMWL_MASK	GENMASK_32(23, 16)
#define GPIO_CIDCFGR_SCID_MASK	GENMASK_32(6, 4)
#define GPIO_CIDCFGR_CONF_MASK	(_CIDCFGR_CFEN | _CIDCFGR_SEMEN |	\
				 GPIO_CIDCFGR_SCID_MASK |		\
				 GPIO_CIDCFGR_SEMWL_MASK)

/*
 * PRIVCFGR register bitfields
 */
#define GPIO_PRIVCFGR_MASK	GENMASK_32(15, 0)

/*
 * SECCFGR register bitfields
 */
#define GPIO_SECCFGR_MASK	GENMASK_32(15, 0)

/*
 * RCFGLOCKR register bitfields
 */
#define GPIO_RCFGLOCKR_MASK	GENMASK_32(15, 0)

/*
 * SEMCR register bitfields
 */
#define GPIO_SEMCR_SCID_M	GENMASK_32(6, 4)

#define GPIO_MODE_MASK		GENMASK_32(1, 0)
#define GPIO_OSPEED_MASK	GENMASK_32(1, 0)
#define GPIO_PUPD_PULL_MASK	GENMASK_32(1, 0)
#define GPIO_ALTERNATE_MASK	GENMASK_32(3, 0)

#define DT_GPIO_BANK_SHIFT	U(12)
#define DT_GPIO_BANK_MASK	GENMASK_32(16, 12)
#define DT_GPIO_PIN_SHIFT	U(8)
#define DT_GPIO_PIN_MASK	GENMASK_32(11, 8)
#define DT_GPIO_MODE_MASK	GENMASK_32(7, 0)

#define DT_GPIO_BANK_NAME0	"GPIOA"

#define GPIO_MODE_INPUT		U(0x0)
#define GPIO_MODE_OUTPUT	U(0x1)
#define GPIO_MODE_ALTERNATE	U(0x2)
#define GPIO_MODE_ANALOG	U(0x3)

#define GPIO_OTYPE_PUSH_PULL	U(0x0)
#define GPIO_OTYPE_OPEN_DRAIN	U(0x1)

#define GPIO_OSPEED_LOW		U(0x0)
#define GPIO_OSPEED_MEDIUM	U(0x1)
#define GPIO_OSPEED_HIGH	U(0x2)
#define GPIO_OSPEED_VERY_HIGH	U(0x3)

#define GPIO_PUPD_NO_PULL	U(0x0)
#define GPIO_PUPD_PULL_UP	U(0x1)
#define GPIO_PUPD_PULL_DOWN	U(0x2)

#define GPIO_OD_LEVEL_LOW	U(0x0)
#define GPIO_OD_LEVEL_HIGH	U(0x1)

#define GPIO_MAX_CID_SUPPORTED	U(3)

/*
 * GPIO configuration description structured as single 16bit word
 * for efficient save/restore when GPIO pin suspends or resumes.
 *
 * @mode: One of GPIO_MODE_*
 * @otype: One of GPIO_OTYPE_*
 * @ospeed: One of GPIO_OSPEED_*
 * @pupd: One of GPIO_PUPD_*
 * @od: One of GPIO_OD_*
 * @af: Alternate function numerical ID between 0 and 15
 * @nsec: Hint on expected secure state of the pin: 0 if secure, 1 otherwise
 */
struct gpio_cfg {
	uint16_t mode:		2;
	uint16_t otype:		1;
	uint16_t ospeed:	2;
	uint16_t pupd:		2;
	uint16_t od:		1;
	uint16_t af:		4;
	uint16_t nsec:		1;
};

/*
 * Description of a pin and its muxing
 *
 * @bank: GPIO bank identifier as assigned by the platform
 * @pin: Pin number in the GPIO bank
 * @cfg: Pin configuration
 */
struct stm32_pinctrl {
	uint8_t bank;
	uint8_t pin;
	struct gpio_cfg cfg;
};

/*
 * struct stm32_pinctrl_array - Array of pins in a pin control state
 * @count: Number of cells in @pinctrl
 * @pinctrl: Pin control configuration
 */
struct stm32_pinctrl_array {
	size_t count;
	struct stm32_pinctrl pinctrl[];
};

/**
 * struct stm32_gpio_bank - GPIO bank instance
 *
 * @base: base address of the GPIO controller registers.
 * @clock: clock identifier.
 * @gpio_chip: GPIO chip reference for that GPIO bank
 * @ngpios: number of GPIOs.
 * @bank_id: Id of the bank.
 * @lock: lock protecting the GPIO bank access.
 * @rif_cfg: RIF configuration data
 * @sec_support: True if bank supports pin security protection, else false
 * @ready: True if configuration is applied, else false
 * @is_tdcid: True if OP-TEE runs as Trusted Domain CID
 * @link: Link in bank list
 */
struct stm32_gpio_bank {
	vaddr_t base;
	struct clk *clock;
	struct gpio_chip gpio_chip;
	unsigned int ngpios;
	unsigned int bank_id;
	unsigned int lock;
	struct rif_conf_data *rif_cfg;
	uint32_t seccfgr;
	bool sec_support;
	bool ready;
	bool is_tdcid;
	STAILQ_ENTRY(stm32_gpio_bank) link;
};

/*
 * struct stm32_gpios_pm_state - Consumed GPIO for PM purpose
 * @gpio_pinctrl: Reference and configuration state for a consumed GPIO
 * @level: GPIO level
 * @link: Link consumed GPIO list
 */
struct stm32_gpio_pm_state {
	struct stm32_pinctrl gpio_pinctrl;
	uint8_t level;
	SLIST_ENTRY(stm32_gpio_pm_state) link;
};

/**
 * Compatibility information of supported banks
 *
 * @gpioz: True if bank is a GPIOZ bank
 * @secure_control: Identify GPIO security bank capability.
 * @secure_control: Identify RIF presence.
 */
struct bank_compat {
	bool gpioz;
	bool secure_control;
	bool secure_extended;
};

static unsigned int gpio_lock;

static STAILQ_HEAD(, stm32_gpio_bank) bank_list =
		STAILQ_HEAD_INITIALIZER(bank_list);

static SLIST_HEAD(, stm32_gpio_pm_state) consumed_gpios_head;

static bool is_stm32_gpio_chip(struct gpio_chip *chip);

static struct stm32_gpio_bank *gpio_chip_to_bank(struct gpio_chip *chip)
{
	return container_of(chip, struct stm32_gpio_bank, gpio_chip);
}

unsigned int stm32_gpio_chip_bank_id(struct gpio_chip *chip)
{
	struct stm32_gpio_bank *bank = gpio_chip_to_bank(chip);

	assert(bank);

	return bank->bank_id;
}

static enum gpio_level stm32_gpio_get_level(struct gpio_chip *chip,
					    unsigned int gpio_pin)
{
	struct stm32_gpio_bank *bank = gpio_chip_to_bank(chip);
	enum gpio_level level = GPIO_LEVEL_HIGH;
	unsigned int reg_offset = 0;
	unsigned int mode = 0;

	assert(gpio_pin < bank->ngpios);

	if (clk_enable(bank->clock))
		panic();

	mode = (io_read32(bank->base + GPIO_MODER_OFFSET) >> (gpio_pin << 1)) &
	       GPIO_MODE_MASK;

	switch (mode) {
	case GPIO_MODE_INPUT:
		reg_offset = GPIO_IDR_OFFSET;
		break;
	case GPIO_MODE_OUTPUT:
		reg_offset = GPIO_ODR_OFFSET;
		break;
	default:
		panic();
	}

	if (io_read32(bank->base + reg_offset) & BIT(gpio_pin))
		level = GPIO_LEVEL_HIGH;
	else
		level = GPIO_LEVEL_LOW;

	clk_disable(bank->clock);

	return level;
}

static void stm32_gpio_set_level(struct gpio_chip *chip, unsigned int gpio_pin,
				 enum gpio_level level)
{
	struct stm32_gpio_bank *bank = gpio_chip_to_bank(chip);

	assert(gpio_pin < bank->ngpios);

	if (clk_enable(bank->clock))
		panic();

	assert(((io_read32(bank->base + GPIO_MODER_OFFSET) >>
		 (gpio_pin << 1)) & GPIO_MODE_MASK) == GPIO_MODE_OUTPUT);

	if (level == GPIO_LEVEL_HIGH)
		io_write32(bank->base + GPIO_BSRR_OFFSET, BIT(gpio_pin));
	else
		io_write32(bank->base + GPIO_BSRR_OFFSET, BIT(gpio_pin + 16));

	clk_disable(bank->clock);
}

static enum gpio_dir stm32_gpio_get_direction(struct gpio_chip *chip,
					      unsigned int gpio_pin)
{
	struct stm32_gpio_bank *bank = gpio_chip_to_bank(chip);
	uint32_t mode = 0;

	assert(gpio_pin < bank->ngpios);

	if (clk_enable(bank->clock))
		panic();

	mode = (io_read32(bank->base + GPIO_MODER_OFFSET) >> (gpio_pin << 1)) &
	       GPIO_MODE_MASK;

	clk_disable(bank->clock);

	switch (mode) {
	case GPIO_MODE_INPUT:
		return GPIO_DIR_IN;
	case GPIO_MODE_OUTPUT:
		return GPIO_DIR_OUT;
	default:
		panic();
	}
}

static void stm32_gpio_set_direction(struct gpio_chip *chip,
				     unsigned int gpio_pin,
				     enum gpio_dir direction)
{
	struct stm32_gpio_bank *bank = gpio_chip_to_bank(chip);
	uint32_t exceptions = 0;
	uint32_t mode = 0;

	assert(gpio_pin < bank->ngpios);

	if (direction == GPIO_DIR_IN)
		mode = GPIO_MODE_INPUT;
	else
		mode = GPIO_MODE_OUTPUT;

	if (clk_enable(bank->clock))
		panic();
	exceptions = cpu_spin_lock_xsave(&gpio_lock);
	io_clrsetbits32(bank->base + GPIO_MODER_OFFSET,
			SHIFT_U32(GPIO_MODE_MASK, gpio_pin << 1),
			SHIFT_U32(mode, gpio_pin << 1));
	cpu_spin_unlock_xrestore(&gpio_lock, exceptions);
	clk_disable(bank->clock);
}

/* Forward reference to the PM callback handler for consumed GPIOs */
static TEE_Result consumed_gpios_pm(enum pm_op op, unsigned int pm_hint,
				    const struct pm_callback_handle *pm_hdl);

static void stm32_gpio_put_gpio(struct gpio_chip *chip, struct gpio *gpio)
{
	struct stm32_gpio_bank *bank = gpio_chip_to_bank(chip);
	struct stm32_gpio_pm_state *tstate = NULL;
	struct stm32_gpio_pm_state *state = NULL;
	uint32_t exceptions = 0;

	assert(is_stm32_gpio_chip(chip));

	exceptions = cpu_spin_lock_xsave(&gpio_lock);

	SLIST_FOREACH_SAFE(state, &consumed_gpios_head, link, tstate) {
		if (state->gpio_pinctrl.bank == bank->bank_id &&
		    state->gpio_pinctrl.pin == gpio->pin) {
			SLIST_REMOVE(&consumed_gpios_head, state,
				     stm32_gpio_pm_state, link);
			unregister_pm_driver_cb(consumed_gpios_pm, state);
			free(state);
			free(gpio);
			break;
		}
	}
	assert(state);

	cpu_spin_unlock_xrestore(&gpio_lock, exceptions);
}

static const struct gpio_ops stm32_gpio_ops = {
	.get_direction = stm32_gpio_get_direction,
	.set_direction = stm32_gpio_set_direction,
	.get_value = stm32_gpio_get_level,
	.set_value = stm32_gpio_set_level,
	.put = stm32_gpio_put_gpio,
};

static bool __maybe_unused is_stm32_gpio_chip(struct gpio_chip *chip)
{
	return chip && chip->ops == &stm32_gpio_ops;
}

static struct stm32_gpio_bank *stm32_gpio_get_bank(unsigned int bank_id)
{
	struct stm32_gpio_bank *bank = NULL;

	STAILQ_FOREACH(bank, &bank_list, link)
		if (bank_id == bank->bank_id)
			return bank;

	panic();
}

#ifdef CFG_STM32_RIF
static bool pin_is_accessible(struct stm32_gpio_bank *bank, unsigned int pin)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	bool accessible = false;
	uint32_t cidcfgr = 0;

	if (!bank->rif_cfg)
		return true;

	if (clk_enable(bank->clock))
		panic();

	cidcfgr = io_read32(bank->base + GPIO_CIDCFGR(pin));

	if (!(cidcfgr & _CIDCFGR_CFEN)) {
		/* Resource can be accessed when CID filtering is disabled */
		accessible = true;
	} else if (SCID_OK(cidcfgr, GPIO_CIDCFGR_SCID_MASK, RIF_CID1)) {
		/* Resource can be accessed if CID1 is statically allowed */
		accessible = true;
	} else if (SEM_EN_AND_OK(cidcfgr, RIF_CID1)) {
		/* We must acquire the semaphore to access the resource */
		res = stm32_rif_acquire_semaphore(bank->base + GPIO_SEMCR(pin),
						  GPIO_MAX_CID_SUPPORTED);
		if (res == TEE_SUCCESS)
			accessible = true;
		else
			EMSG("Failed to acquire semaphore on GPIO %c%u",
			     'A' + bank->bank_id, pin);
	}

	clk_disable(bank->clock);

	return accessible;
}
#else
static bool pin_is_accessible(struct stm32_gpio_bank *bank __unused,
			      unsigned int pin __unused)
{
	return true;
}
#endif

static bool pin_is_secure(struct stm32_gpio_bank *bank, unsigned int pin)
{
	bool secure = false;

	if (bank->rif_cfg || bank->sec_support) {
		if (clk_enable(bank->clock))
			panic();

		secure = io_read32(bank->base + GPIO_SECR_OFFSET) & BIT(pin);

		clk_disable(bank->clock);
	}

	return secure;
}

/* Save to output @cfg the current GPIO (@bank_id/@pin) configuration */
static void get_gpio_cfg(uint32_t bank_id, uint32_t pin, struct gpio_cfg *cfg)
{
	struct stm32_gpio_bank *bank = stm32_gpio_get_bank(bank_id);

	if (clk_enable(bank->clock))
		panic();

	/*
	 * Save GPIO configuration bits spread over the few bank registers.
	 * 1bit fields are accessed at bit position being the pin index.
	 * 2bit fields are accessed at bit position being twice the pin index.
	 * 4bit fields are accessed at bit position being fourth the pin index
	 * but accessed from 2 32bit registers at incremental addresses.
	 */
	cfg->mode = (io_read32(bank->base + GPIO_MODER_OFFSET) >> (pin << 1)) &
		    GPIO_MODE_MASK;

	cfg->otype = (io_read32(bank->base + GPIO_OTYPER_OFFSET) >> pin) & 1;

	cfg->ospeed = (io_read32(bank->base +  GPIO_OSPEEDR_OFFSET) >>
		       (pin << 1)) & GPIO_OSPEED_MASK;

	cfg->pupd = (io_read32(bank->base +  GPIO_PUPDR_OFFSET) >> (pin << 1)) &
		    GPIO_PUPD_PULL_MASK;

	cfg->od = (io_read32(bank->base + GPIO_ODR_OFFSET) >> (pin << 1)) & 1;

	if (pin < GPIO_ALT_LOWER_LIMIT)
		cfg->af = (io_read32(bank->base + GPIO_AFRL_OFFSET) >>
			   (pin << 2)) & GPIO_ALTERNATE_MASK;
	else
		cfg->af = (io_read32(bank->base + GPIO_AFRH_OFFSET) >>
			   ((pin - GPIO_ALT_LOWER_LIMIT) << 2)) &
			  GPIO_ALTERNATE_MASK;

	clk_disable(bank->clock);
}

/* Apply GPIO (@bank/@pin) configuration described by @cfg */
static void set_gpio_cfg(uint32_t bank_id, uint32_t pin, struct gpio_cfg *cfg)
{
	struct stm32_gpio_bank *bank = stm32_gpio_get_bank(bank_id);
	uint32_t exceptions = 0;

	if (clk_enable(bank->clock))
		panic();
	exceptions = cpu_spin_lock_xsave(&gpio_lock);

	/* Load GPIO MODE value, 2bit value shifted by twice the pin number */
	io_clrsetbits32(bank->base + GPIO_MODER_OFFSET,
			SHIFT_U32(GPIO_MODE_MASK, pin << 1),
			SHIFT_U32(cfg->mode, pin << 1));

	/* Load GPIO Output TYPE value, 1bit shifted by pin number value */
	io_clrsetbits32(bank->base + GPIO_OTYPER_OFFSET, BIT(pin),
			SHIFT_U32(cfg->otype, pin));

	/* Load GPIO Output Speed confguration, 2bit value */
	io_clrsetbits32(bank->base + GPIO_OSPEEDR_OFFSET,
			SHIFT_U32(GPIO_OSPEED_MASK, pin << 1),
			SHIFT_U32(cfg->ospeed, pin << 1));

	/* Load GPIO pull configuration, 2bit value */
	io_clrsetbits32(bank->base + GPIO_PUPDR_OFFSET, BIT(pin),
			SHIFT_U32(cfg->pupd, pin << 1));

	/* Load pin mux Alternate Function configuration, 4bit value */
	if (pin < GPIO_ALT_LOWER_LIMIT) {
		io_clrsetbits32(bank->base + GPIO_AFRL_OFFSET,
				SHIFT_U32(GPIO_ALTERNATE_MASK, pin << 2),
				SHIFT_U32(cfg->af, pin << 2));
	} else {
		size_t shift = (pin - GPIO_ALT_LOWER_LIMIT) << 2;

		io_clrsetbits32(bank->base + GPIO_AFRH_OFFSET,
				SHIFT_U32(GPIO_ALTERNATE_MASK, shift),
				SHIFT_U32(cfg->af, shift));
	}

	/* Load GPIO Output direction confuguration, 1bit */
	io_clrsetbits32(bank->base + GPIO_ODR_OFFSET, BIT(pin), cfg->od << pin);

	cpu_spin_unlock_xrestore(&gpio_lock, exceptions);
	clk_disable(bank->clock);
}

/* Count pins described in the DT node and get related data if possible */
static int get_pinctrl_from_fdt(const void *fdt, int node,
				int consumer_node __maybe_unused,
				struct stm32_pinctrl *pinctrl, size_t count)
{
	struct stm32_gpio_bank *bank_ref = NULL;
	const fdt32_t *cuint = NULL;
	const fdt32_t *slewrate = NULL;
	int len = 0;
	uint32_t i = 0;
	uint32_t speed = GPIO_OSPEED_LOW;
	uint32_t pull = GPIO_PUPD_NO_PULL;
	size_t found = 0;
	bool do_panic = false;

	cuint = fdt_getprop(fdt, node, "pinmux", &len);
	if (!cuint)
		return -FDT_ERR_NOTFOUND;

	slewrate = fdt_getprop(fdt, node, "slew-rate", NULL);
	if (slewrate)
		speed = fdt32_to_cpu(*slewrate);

	if (fdt_getprop(fdt, node, "bias-pull-up", NULL))
		pull = GPIO_PUPD_PULL_UP;
	if (fdt_getprop(fdt, node, "bias-pull-down", NULL))
		pull = GPIO_PUPD_PULL_DOWN;

	for (i = 0; i < ((uint32_t)len / sizeof(uint32_t)); i++) {
		uint32_t pincfg = 0;
		uint32_t bank = 0;
		uint32_t pin = 0;
		uint32_t mode = 0;
		uint32_t alternate = 0;
		uint32_t odata = 0;
		bool opendrain = false;
		bool pin_non_secure = true;

		pincfg = fdt32_to_cpu(*cuint);
		cuint++;

		bank = (pincfg & DT_GPIO_BANK_MASK) >> DT_GPIO_BANK_SHIFT;

		pin = (pincfg & DT_GPIO_PIN_MASK) >> DT_GPIO_PIN_SHIFT;

		mode = pincfg & DT_GPIO_MODE_MASK;

		pin_non_secure = pincfg & STM32_PIN_NSEC;

		switch (mode) {
		case 0:
			mode = GPIO_MODE_INPUT;
			break;
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
		case 7:
		case 8:
		case 9:
		case 10:
		case 11:
		case 12:
		case 13:
		case 14:
		case 15:
		case 16:
			alternate = mode - 1U;
			mode = GPIO_MODE_ALTERNATE;
			break;
		case 17:
			mode = GPIO_MODE_ANALOG;
			break;
		default:
			mode = GPIO_MODE_OUTPUT;
			break;
		}

		if (fdt_getprop(fdt, node, "drive-open-drain", NULL))
			opendrain = true;

		if (fdt_getprop(fdt, node, "output-high", NULL) &&
		    mode == GPIO_MODE_INPUT) {
			mode = GPIO_MODE_OUTPUT;
			odata = 1;
		}

		if (fdt_getprop(fdt, node, "output-low", NULL) &&
		    mode == GPIO_MODE_INPUT) {
			mode = GPIO_MODE_OUTPUT;
			odata = 0;
		}

		if (found < count) {
			struct stm32_pinctrl *ref = &pinctrl[found];

			ref->bank = (uint8_t)bank;
			ref->pin = (uint8_t)pin;
			ref->cfg.mode = mode;
			if (opendrain)
				ref->cfg.otype = GPIO_OTYPE_OPEN_DRAIN;
			else
				ref->cfg.otype = GPIO_OTYPE_PUSH_PULL;
			ref->cfg.ospeed = speed;
			ref->cfg.pupd = pull;
			ref->cfg.od = odata;
			ref->cfg.af = alternate;
			ref->cfg.nsec = pin_non_secure;

			bank_ref = stm32_gpio_get_bank(bank);

			if (pin >= bank_ref->ngpios) {
				EMSG("node %s requests pin %c%u that does not exist",
				     fdt_get_name(fdt, consumer_node, NULL),
				     bank + 'A', pin);
				do_panic = true;
			} else if (!pin_is_accessible(bank_ref, pin)) {
				EMSG("node %s requests pin %c%u which access is denied",
				     fdt_get_name(fdt, consumer_node, NULL),
				     bank + 'A', pin);
				do_panic = true;
			}
		}

		found++;
	}

	if (do_panic)
		panic();

	return (int)found;
}

static TEE_Result consumed_gpios_pm(enum pm_op op,
				    unsigned int pm_hint __unused,
				    const struct pm_callback_handle *pm_hdl)
{
	struct stm32_gpio_pm_state *handle = pm_hdl->handle;
	unsigned int bank_id = handle->gpio_pinctrl.bank;
	unsigned int pin = handle->gpio_pinctrl.pin;
	struct gpio_chip *chip = &stm32_gpio_get_bank(bank_id)->gpio_chip;

	if (op == PM_OP_RESUME) {
		set_gpio_cfg(bank_id, pin, &handle->gpio_pinctrl.cfg);
		if (handle->gpio_pinctrl.cfg.mode == GPIO_MODE_OUTPUT)
			stm32_gpio_set_level(chip, pin, handle->level);
	} else {
		get_gpio_cfg(bank_id, pin, &handle->gpio_pinctrl.cfg);
		if (handle->gpio_pinctrl.cfg.mode == GPIO_MODE_OUTPUT)
			handle->level = stm32_gpio_get_level(chip, pin);
	}

	return TEE_SUCCESS;
}
DECLARE_KEEP_PAGER_PM(consumed_gpios_pm);

static TEE_Result stm32_gpio_get_dt(struct dt_pargs *pargs, void *data,
				    struct gpio **out_gpio)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	const char *consumer_name __maybe_unused = NULL;
	struct stm32_gpio_pm_state *reg_state = NULL;
	struct stm32_gpio_pm_state *state = NULL;
	struct stm32_gpio_bank *bank = data;
	struct gpio *gpio = NULL;
	unsigned int shift_1b = 0;
	unsigned int shift_2b = 0;
	bool gpio_secure = true;
	uint32_t exceptions = 0;
	uint32_t otype = 0;
	uint32_t pupd = 0;
	uint32_t mode = 0;

	consumer_name = fdt_get_name(pargs->fdt, pargs->consumer_node,
				     NULL);

	res = gpio_dt_alloc_pin(pargs, &gpio);
	if (res)
		return res;

	if (gpio->pin >= bank->ngpios) {
		DMSG("Invalid GPIO reference");
		free(gpio);
		return TEE_ERROR_GENERIC;
	}

	if (gpio->dt_flags & GPIO_STM32_NSEC)
		gpio_secure = false;

	state = calloc(1, sizeof(*state));
	if (!state) {
		free(gpio);
		return TEE_ERROR_OUT_OF_MEMORY;
	}

	SLIST_FOREACH(reg_state, &consumed_gpios_head, link) {
		if (reg_state->gpio_pinctrl.bank == bank->bank_id &&
		    reg_state->gpio_pinctrl.pin == gpio->pin) {
			EMSG("node %s: GPIO %c%u is used by another device",
			     consumer_name, bank->bank_id + 'A', gpio->pin);
			free(state);
			free(gpio);
			return TEE_ERROR_GENERIC;
		}
	}

	if (!pin_is_accessible(bank, gpio->pin)) {
		EMSG("node %s requests pin on GPIO %c%u which access is denied",
		     consumer_name, bank->bank_id + 'A', gpio->pin);
		panic();
	}

	if (gpio_secure && !(bank->rif_cfg || bank->sec_support)) {
		EMSG("node %s requests secure GPIO %c%u that cannot be secured",
		     consumer_name, bank->bank_id + 'A', gpio->pin);
		panic();
	}

	if (gpio_secure != pin_is_secure(bank, gpio->pin)) {
		IMSG("WARNING: node %s requests %s GPIO %c%u but pin is %s. Check st,protreg in GPIO bank node %s",
		     consumer_name, gpio_secure ? "secure" : "non-secure",
		     bank->bank_id + 'A', gpio->pin,
		     pin_is_secure(bank, gpio->pin) ? "secure" : "non-secure",
		     fdt_get_name(pargs->fdt, pargs->phandle_node, NULL));
		if (!IS_ENABLED(CFG_INSECURE))
			panic();
	}

	state->gpio_pinctrl.pin = gpio->pin;
	state->gpio_pinctrl.bank = bank->bank_id;
	SLIST_INSERT_HEAD(&consumed_gpios_head, state, link);

	register_pm_core_service_cb(consumed_gpios_pm, state,
				    "stm32-gpio-state");

	shift_1b = gpio->pin;
	shift_2b = SHIFT_U32(gpio->pin, 1);

	if (gpio->dt_flags & GPIO_PULL_UP)
		pupd = GPIO_PUPD_PULL_UP;
	else if (gpio->dt_flags & GPIO_PULL_DOWN)
		pupd = GPIO_PUPD_PULL_DOWN;
	else
		pupd = GPIO_PUPD_NO_PULL;

	if (gpio->dt_flags & GPIO_LINE_OPEN_DRAIN)
		otype = GPIO_OTYPE_OPEN_DRAIN;
	else
		otype = GPIO_OTYPE_PUSH_PULL;

	if (clk_enable(bank->clock))
		panic();
	exceptions = cpu_spin_lock_xsave(&gpio_lock);

	io_clrsetbits32(bank->base + GPIO_MODER_OFFSET,
			SHIFT_U32(GPIO_MODE_MASK, shift_2b),
			SHIFT_U32(mode, shift_2b));

	io_clrsetbits32(bank->base + GPIO_OTYPER_OFFSET,
			SHIFT_U32(GPIO_OTYPE_OPEN_DRAIN, shift_1b),
			SHIFT_U32(otype, shift_1b));

	io_clrsetbits32(bank->base + GPIO_PUPDR_OFFSET,
			SHIFT_U32(GPIO_PUPD_PULL_MASK, shift_2b),
			SHIFT_U32(pupd, shift_2b));

	cpu_spin_unlock_xrestore(&gpio_lock, exceptions);
	clk_disable(bank->clock);

	gpio->chip = &bank->gpio_chip;

	*out_gpio = gpio;

	return TEE_SUCCESS;
}

/* Get bank ID from bank node property st,bank-name or panic on failure */
static unsigned int dt_get_bank_id(const void *fdt, int node)
{
	const int dt_name_len = strlen(DT_GPIO_BANK_NAME0);
	const fdt32_t *cuint = NULL;
	int len = 0;

	/* Parse "st,bank-name" to get its id (eg: GPIOA -> 0) */
	cuint = fdt_getprop(fdt, node, "st,bank-name", &len);
	if (!cuint || (len != dt_name_len + 1))
		panic("Missing/wrong st,bank-name property");

	if (strncmp((const char *)cuint, DT_GPIO_BANK_NAME0, dt_name_len - 1) ||
	    strcmp((const char *)cuint, DT_GPIO_BANK_NAME0) < 0)
		panic("Wrong st,bank-name property");

	return (unsigned int)strcmp((const char *)cuint, DT_GPIO_BANK_NAME0);
}

/*
 * Return whether or not the GPIO bank related to a DT node is already
 * registered in the GPIO bank link.
 */
static bool bank_is_registered(const void *fdt, int node)
{
	unsigned int bank_id = dt_get_bank_id(fdt, node);
	struct stm32_gpio_bank *bank = NULL;

	STAILQ_FOREACH(bank, &bank_list, link)
		if (bank->bank_id == bank_id)
			return true;

	return false;
}

static TEE_Result apply_rif_config(struct stm32_gpio_bank *bank,
				   uint32_t gpios_mask)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	uint32_t cidcfgr = 0;
	unsigned int i = 0;

	if (!bank->rif_cfg)
		return TEE_SUCCESS;

	if (clk_enable(bank->clock))
		panic();

	for (i = 0; i < bank->ngpios; i++) {
		if (!(BIT(i) & gpios_mask) ||
		    !(BIT(i) & bank->rif_cfg->access_mask[0]))
			continue;

		/*
		 * When TDCID, OP-TEE should be the one to set the CID filtering
		 * configuration. Clearing previous configuration prevents
		 * undesired events during the only legitimate configuration.
		 */
		if (bank->is_tdcid)
			io_clrbits32(bank->base + GPIO_CIDCFGR(i),
				     GPIO_CIDCFGR_CONF_MASK);

		cidcfgr = io_read32(bank->base + GPIO_CIDCFGR(i));

		/* Check if the controller is in semaphore mode */
		if (SEM_MODE_INCORRECT(cidcfgr))
			continue;

		/* If not TDCID, we want to acquire semaphores assigned to us */
		res = stm32_rif_acquire_semaphore(bank->base + GPIO_SEMCR(i),
						  GPIO_MAX_CID_SUPPORTED);
		if (res) {
			EMSG("Could not acquire semaphore for pin %u", i);
			goto out;
		}
	}

	/* Security and privilege RIF configuration */
	io_clrsetbits32(bank->base + GPIO_PRIVCFGR_OFFSET, gpios_mask,
			bank->rif_cfg->priv_conf[0]);
	io_clrsetbits32(bank->base + GPIO_SECR_OFFSET, gpios_mask,
			bank->rif_cfg->sec_conf[0]);

	if (!bank->is_tdcid) {
		res = TEE_SUCCESS;
		goto out;
	}

	for (i = 0; i < bank->ngpios; i++) {
		if (!(BIT(i) & gpios_mask) ||
		    !(BIT(i) & bank->rif_cfg->access_mask[0]))
			continue;

		io_clrsetbits32(bank->base + GPIO_CIDCFGR(i),
				GPIO_CIDCFGR_CONF_MASK,
				bank->rif_cfg->cid_confs[i]);

		cidcfgr = io_read32(bank->base + GPIO_CIDCFGR(i));

		/*
		 * Take semaphore if the resource is in semaphore mode
		 * and secured.
		 */
		if (SEM_MODE_INCORRECT(cidcfgr) ||
		    !(io_read32(bank->base + GPIO_SECR_OFFSET) & BIT(i))) {
			res = stm32_rif_release_semaphore(bank->base +
				GPIO_SEMCR(i),
				GPIO_MAX_CID_SUPPORTED);
			if (res) {
				EMSG("Could not release semaphore for pin%u",
				     i);
				goto out;
			}
		} else {
			res = stm32_rif_acquire_semaphore(bank->base +
				GPIO_SEMCR(i),
				GPIO_MAX_CID_SUPPORTED);
			if (res) {
				EMSG("Could not acquire semaphore for pin%u",
				     i);
				goto out;
			}
		}
	}

	/*
	 * Lock RIF configuration if configured. This cannot be undone until
	 * next reset.
	 */
	io_clrsetbits32(bank->base + GPIO_RCFGLOCKR_OFFSET, gpios_mask,
			bank->rif_cfg->lock_conf[0]);

	if (IS_ENABLED(CFG_TEE_CORE_DEBUG)) {
		/* Check that RIF config are applied, panic otherwise */
		if ((io_read32(bank->base + GPIO_PRIVCFGR_OFFSET) &
		     bank->rif_cfg->access_mask[0] & gpios_mask) !=
		    (bank->rif_cfg->priv_conf[0] & gpios_mask)) {
			EMSG("GPIO bank priv conf (ID: %u) is incorrect",
			     bank->bank_id);
			panic();
		}

		if ((io_read32(bank->base + GPIO_SECR_OFFSET) &
		     bank->rif_cfg->access_mask[0] & gpios_mask) !=
		    (bank->rif_cfg->sec_conf[0] & gpios_mask)) {
			EMSG("GPIO bank sec conf (ID: %u) is incorrect",
			     bank->bank_id);
			panic();
		}
	}

out:
	clk_disable(bank->clock);

	return res;
}

/* Forward reference to stm32_gpio_set_conf_sec() defined below */
static void stm32_gpio_set_conf_sec(struct stm32_gpio_bank *bank);

static TEE_Result stm32_gpio_fw_configure(struct firewall_query *firewall)
{
	struct stm32_gpio_bank *bank = firewall->ctrl->priv;
	uint32_t gpios_mask = 0;
	bool secure = true;
	unsigned int gpio_pos = 0;

	assert(bank->sec_support);

	if (firewall->arg_count != 1)
		return TEE_ERROR_BAD_PARAMETERS;

	if (bank->rif_cfg) {
#ifdef CFG_STM32_RIF
		gpio_pos = (firewall->args[0] & RIF_PER_ID_MASK) >>
			   RIF_PER_ID_SHIFT;
#else
		panic("RIF support expected");
#endif
		gpios_mask = BIT(gpio_pos);

		/* We're about to change a specific GPIO config */
		bank->rif_cfg->access_mask[0] |= gpios_mask;

		/*
		 * Update bank RIF config with firewall configuration data
		 * and apply it.
		 */
		stm32_rif_parse_cfg(firewall->args[0], bank->rif_cfg,
				    GPIO_MAX_CID_SUPPORTED, bank->ngpios);
		return apply_rif_config(bank, gpios_mask);
	}

	/*
	 * Non RIF GPIO banks use TZPROT() bit mask (bits 0 to 15)
	 * and GPIO_STM32_NSEC bit flag in the DT bindings.
	 */
	gpios_mask = firewall->args[0] & GENMASK_32(15, 0);

	secure = !(firewall->args[0] & GPIO_STM32_NSEC);

	if (gpios_mask & ~GENMASK_32(bank->ngpios, 0)) {
		EMSG("Invalid bitmask %#"PRIx32" for GPIO bank %c",
		     gpios_mask, 'A' + bank->bank_id);
		return TEE_ERROR_GENERIC;
	}

	/* Update bank secure register configuration data and apply it */
	if (secure)
		bank->seccfgr |= gpios_mask;
	else
		bank->seccfgr &= ~gpios_mask;

	stm32_gpio_set_conf_sec(bank);

	return TEE_SUCCESS;
}

static const struct firewall_controller_ops stm32_gpio_firewall_ops = {
	.set_conf = stm32_gpio_fw_configure,
};

static void stm32_gpio_save_rif_config(struct stm32_gpio_bank *bank)
{
	size_t i = 0;

	for (i = 0; i < bank->ngpios; i++)
		bank->rif_cfg->cid_confs[i] = io_read32(bank->base +
							 GPIO_CIDCFGR(i));

	bank->rif_cfg->priv_conf[0] = io_read32(bank->base +
						 GPIO_PRIVCFGR_OFFSET);
	bank->rif_cfg->sec_conf[0] = io_read32(bank->base +
						 GPIO_SECR_OFFSET);
	bank->rif_cfg->lock_conf[0] = io_read32(bank->base +
						 GPIO_RCFGLOCKR_OFFSET);
}

static void stm32_parse_gpio_rif_conf(struct stm32_gpio_bank *bank,
				      const void *fdt, int node)
{
	unsigned int i = 0;
	unsigned int nb_rif_conf = 0;
	int lenp = 0;
	const fdt32_t *cuint = NULL;

	cuint = fdt_getprop(fdt, node, "st,protreg", &lenp);
	if (!cuint) {
		DMSG("No RIF configuration available");
		return;
	}

	bank->rif_cfg = calloc(1, sizeof(*bank->rif_cfg));
	if (!bank->rif_cfg)
		panic();

	bank->rif_cfg->sec_conf = calloc(1, sizeof(uint32_t));
	if (!bank->rif_cfg->sec_conf)
		panic();

	nb_rif_conf = MIN((unsigned int)(lenp / sizeof(uint32_t)),
			  bank->ngpios);

	bank->rif_cfg->cid_confs = calloc(bank->ngpios, sizeof(uint32_t));
	bank->rif_cfg->priv_conf = calloc(1, sizeof(uint32_t));
	bank->rif_cfg->lock_conf = calloc(1, sizeof(uint32_t));
	bank->rif_cfg->access_mask = calloc(1, sizeof(uint32_t));
	if (!bank->rif_cfg->cid_confs || !bank->rif_cfg->access_mask ||
	    !bank->rif_cfg->priv_conf || !bank->rif_cfg->lock_conf)
		panic("Missing memory capacity for GPIOS RIF configuration");

	for (i = 0; i < nb_rif_conf; i++)
		stm32_rif_parse_cfg(fdt32_to_cpu(cuint[i]), bank->rif_cfg,
				    GPIO_MAX_CID_SUPPORTED, bank->ngpios);
}

/* Get GPIO bank information from the DT */
static TEE_Result dt_stm32_gpio_bank(const void *fdt, int node,
				     const void *compat_data,
				     int range_offset,
				     struct stm32_gpio_bank **out_bank)
{
	const struct bank_compat *compat = compat_data;
	TEE_Result res = TEE_ERROR_GENERIC;
	struct stm32_gpio_bank *bank = NULL;
	const fdt32_t *cuint = NULL;
	struct io_pa_va pa_va = { };
	struct clk *clk = NULL;
	size_t blen = 0;
	paddr_t pa = 0;
	int len = 0;
	int i = 0;

	assert(out_bank);

	/* Probe deferrable devices first */
	res = clk_dt_get_by_index(fdt, node, 0, &clk);
	if (res)
		return res;

	bank = calloc(1, sizeof(*bank));
	if (!bank)
		return TEE_ERROR_OUT_OF_MEMORY;

	if (compat->secure_extended) {
		res = stm32_rifsc_check_tdcid(&bank->is_tdcid);
		if (res)
			return res;
	}

	/*
	 * Do not rely *only* on the "reg" property to get the address,
	 * but consider also the "ranges" translation property
	 */
	pa = fdt_reg_base_address(fdt, node);
	if (pa == DT_INFO_INVALID_REG)
		panic("missing reg property");

	pa_va.pa = pa + range_offset;

	blen = fdt_reg_size(fdt, node);
	if (blen == DT_INFO_INVALID_REG_SIZE)
		panic("missing reg size property");

	DMSG("Bank name %s", fdt_get_name(fdt, node, NULL));
	bank->base = io_pa_or_va_secure(&pa_va, blen);
	bank->bank_id = dt_get_bank_id(fdt, node);
	bank->clock = clk;
	bank->gpio_chip.ops = &stm32_gpio_ops;
	bank->sec_support = compat->secure_control;

	/* Parse gpio-ranges with its 4 parameters */
	cuint = fdt_getprop(fdt, node, "gpio-ranges", &len);
	len /= sizeof(*cuint);
	if (len % 4)
		panic("wrong gpio-ranges syntax");

	/* Get the last defined gpio line (offset + nb of pins) */
	for (i = 0; i < len / 4; i++) {
		bank->ngpios = MAX(bank->ngpios,
				   (unsigned int)(fdt32_to_cpu(*(cuint + 1)) +
						  fdt32_to_cpu(*(cuint + 3))));
		cuint += 4;
	}

	if (compat->secure_extended) {
		/* RIF configuration */
		bank->base = io_pa_or_va_secure(&pa_va, blen);

		stm32_parse_gpio_rif_conf(bank, fdt, node);
	} else if (bank->sec_support) {
		/* Secure configuration */
		bank->base = io_pa_or_va_secure(&pa_va, blen);
		cuint = fdt_getprop(fdt, node, "st,protreg", NULL);
		if (cuint)
			bank->seccfgr = fdt32_to_cpu(*cuint);
		else
			DMSG("GPIO bank %c assigned to non-secure",
			     bank->bank_id + 'A');
	} else {
		bank->base = io_pa_or_va_nsec(&pa_va, blen);
	}

	if (compat->gpioz)
		stm32mp_register_gpioz_pin_count(bank->ngpios);

	*out_bank = bank;

	return TEE_SUCCESS;
}

static TEE_Result stm32_gpio_firewall_register(const void *fdt, int node,
					       struct stm32_gpio_bank *bank)
{
	struct firewall_controller *controller = NULL;
	TEE_Result res = TEE_ERROR_GENERIC;
	char bank_name[] = "gpio-bank-X";
	char *name = NULL;

	if (!IS_ENABLED(CFG_DRIVERS_FIREWALL) ||
	    !bank->sec_support)
		return TEE_SUCCESS;

	controller = calloc(1, sizeof(*controller));
	if (!controller)
		return TEE_ERROR_OUT_OF_MEMORY;

	bank_name[sizeof(bank_name) - 2] = 'A' + bank->bank_id;
	name = strdup(bank_name);

	controller->name = name;
	controller->priv = bank;
	controller->ops = &stm32_gpio_firewall_ops;

	if (!controller->name)
		EMSG("Warning: out of memory to store bank name");

	res = firewall_dt_controller_register(fdt, node, controller);
	if (res) {
		free(name);
		free(controller);
	}

	return res;
}

/* Parse a pinctrl node to register the GPIO banks it describes */
static TEE_Result dt_stm32_gpio_pinctrl(const void *fdt, int node,
					const void *compat_data)
{
	TEE_Result res = TEE_SUCCESS;
	const fdt32_t *cuint = NULL;
	int range_offs = 0;
	int b_node = 0;
	int len = 0;

	/* Read the ranges property (for regs memory translation) */
	cuint = fdt_getprop(fdt, node, "ranges", &len);
	if (!cuint)
		panic("missing ranges property");

	len /= sizeof(*cuint);
	if (len == 3)
		range_offs = fdt32_to_cpu(*(cuint + 1)) - fdt32_to_cpu(*cuint);

	fdt_for_each_subnode(b_node, fdt, node) {
		cuint = fdt_getprop(fdt, b_node, "gpio-controller", &len);
		if (cuint) {
			/*
			 * We found a property "gpio-controller" in the node:
			 * the node is a GPIO bank description, add it to the
			 * bank list.
			 */
			struct stm32_gpio_bank *bank = NULL;

			if (fdt_get_status(fdt, b_node) == DT_STATUS_DISABLED ||
			    bank_is_registered(fdt, b_node))
				continue;

			res = dt_stm32_gpio_bank(fdt, b_node, compat_data,
						 range_offs, &bank);
			if (res)
				return res;

			/* Registering a provider should not defer probe */
			res = gpio_register_provider(fdt, b_node,
						     stm32_gpio_get_dt, bank);
			if (res)
				panic();

			res = stm32_gpio_firewall_register(fdt, b_node, bank);
			if (res)
				panic();

			STAILQ_INSERT_TAIL(&bank_list, bank, link);
		} else {
			if (len != -FDT_ERR_NOTFOUND)
				panic();
		}
	}

	return TEE_SUCCESS;
}

#ifdef CFG_DRIVERS_PINCTRL
static TEE_Result stm32_pinctrl_conf_apply(struct pinconf *conf)
{
	struct stm32_pinctrl_array *ref = conf->priv;
	struct stm32_pinctrl *p = ref->pinctrl;
	size_t pin_count = ref->count;
	size_t n = 0;
	bool error = false;

	for (n = 0; n < pin_count; n++) {
		struct stm32_gpio_bank *bank = stm32_gpio_get_bank(p[n].bank);

		if (p[n].cfg.nsec == !pin_is_secure(bank, p[n].pin))
			continue;

		if (IS_ENABLED(CFG_INSECURE)) {
			IMSG("WARNING: apply pinctrl for %ssecure pin %c%u that is %ssecure",
			     p[n].cfg.nsec ? "non-" : "",
			     p[n].bank + 'A', p[n].pin,
			     pin_is_secure(bank, p[n].pin) ? "" : "non-");
		} else {
			EMSG("Apply pinctrl for %ssecure pin %c%u that is %ssecure",
			     p[n].cfg.nsec ? "non-" : "",
			     p[n].bank + 'A', p[n].pin,
			     pin_is_secure(bank, p[n].pin) ? "" : "non-");
			error = true;
		}
	}

	if (error)
		return TEE_ERROR_GENERIC;

	for (n = 0; n < pin_count; n++)
		set_gpio_cfg(p[n].bank, p[n].pin, &p[n].cfg);

	return TEE_SUCCESS;
}

static void stm32_pinctrl_conf_free(struct pinconf *conf)
{
	free(conf);
}

static const struct pinctrl_ops stm32_pinctrl_ops = {
	.conf_apply = stm32_pinctrl_conf_apply,
	.conf_free = stm32_pinctrl_conf_free,
};

DECLARE_KEEP_PAGER(stm32_pinctrl_ops);

void stm32_gpio_pinctrl_bank_pin(struct pinctrl_state *pinctrl,
				 unsigned int *bank, unsigned int *pin,
				 unsigned int *count)
{
	size_t conf_index = 0;
	size_t pin_count = 0;
	size_t n = 0;

	assert(count);
	if (!pinctrl)
		goto out;

	for (conf_index = 0; conf_index < pinctrl->conf_count; conf_index++) {
		struct pinconf *pinconf = pinctrl->confs[conf_index];
		struct stm32_pinctrl_array *ref = pinconf->priv;

		/* Consider only the stm32_gpio pins */
		if (pinconf->ops != &stm32_pinctrl_ops)
			continue;

		if (bank || pin) {
			for (n = 0; n < ref->count; n++) {
				if (bank && pin_count < *count)
					bank[pin_count] = ref->pinctrl[n].bank;
				if (pin && pin_count < *count)
					pin[pin_count] = ref->pinctrl[n].pin;
				pin_count++;
			}
		} else {
			pin_count += ref->count;
		}
	}

out:
	*count = pin_count;
}

/* Allocate and return a pinctrl configuration from a DT reference */
static TEE_Result stm32_pinctrl_dt_get(struct dt_pargs *pargs,
				       void *data __unused,
				       struct pinconf **out_pinconf)
{
	struct conf {
		struct pinconf pinconf;
		struct stm32_pinctrl_array array_ref;
	} *loc_conf = NULL;
	struct stm32_pinctrl *pinctrl = NULL;
	struct pinconf *pinconf = NULL;
	const void *fdt = NULL;
	size_t pin_count = 0;
	int pinctrl_node = 0;
	int pinmux_node = 0;
	int count = 0;

	pinctrl_node = pargs->phandle_node;
	fdt = pargs->fdt;
	assert(fdt && pinctrl_node);

	fdt_for_each_subnode(pinmux_node, fdt, pinctrl_node) {
		if (fdt_getprop(fdt, pinmux_node, "pinmux", &count))
			pin_count += (size_t)count / sizeof(uint32_t);
		else if (count != -FDT_ERR_NOTFOUND)
			panic();
	}

	loc_conf = calloc(1, sizeof(*loc_conf) + sizeof(*pinctrl) * pin_count);
	if (!loc_conf)
		return TEE_ERROR_OUT_OF_MEMORY;

	pinconf = &loc_conf->pinconf;
	pinconf->ops = &stm32_pinctrl_ops;
	pinconf->priv = &loc_conf->array_ref;

	loc_conf->array_ref.count = pin_count;
	pinctrl = loc_conf->array_ref.pinctrl;

	count = 0;
	fdt_for_each_subnode(pinmux_node, fdt, pinctrl_node) {
		int found = 0;

		found = get_pinctrl_from_fdt(fdt, pinmux_node,
					     pargs->consumer_node,
					     pinctrl + count,
					     pin_count - count);
		if (found <= 0 && found > ((int)pin_count - count)) {
			/* We can't recover from an error here so let's panic */
			panic();
		}

		count += found;
	}

	*out_pinconf = pinconf;

	return TEE_SUCCESS;
}
#endif /*CFG_DRIVERS_PINCTRL*/

static void stm32_gpio_get_conf_sec(struct stm32_gpio_bank *bank)
{
	if (bank->sec_support) {
		clk_enable(bank->clock);
		bank->seccfgr = io_read32(bank->base + GPIO_SECR_OFFSET);
		clk_disable(bank->clock);
	}
}

static void stm32_gpio_set_conf_sec(struct stm32_gpio_bank *bank)
{
	if (bank->sec_support) {
		clk_enable(bank->clock);
		io_write32(bank->base + GPIO_SECR_OFFSET, bank->seccfgr);
		clk_disable(bank->clock);
	}
}

static TEE_Result stm32_gpio_sec_config_resume(void)
{
	struct stm32_gpio_bank *bank = NULL;

	STAILQ_FOREACH(bank, &bank_list, link) {
		if (bank->rif_cfg) {
			if (!bank->is_tdcid)
				continue;

			bank->rif_cfg->access_mask[0] = GENMASK_32(bank->ngpios,
								   0);

			apply_rif_config(bank, bank->rif_cfg->access_mask[0]);
		} else {
			stm32_gpio_set_conf_sec(bank);
		}
	}

	return TEE_SUCCESS;
}

static TEE_Result stm32_gpio_sec_config_suspend(void)
{
	struct stm32_gpio_bank *bank = NULL;

	STAILQ_FOREACH(bank, &bank_list, link) {
		if (bank->rif_cfg) {
			if (bank->is_tdcid)
				stm32_gpio_save_rif_config(bank);
		} else {
			stm32_gpio_get_conf_sec(bank);
		}
	}

	return TEE_SUCCESS;
}

static TEE_Result
stm32_gpio_sec_config_pm(enum pm_op op, unsigned int pm_hint,
			 const struct pm_callback_handle *hdl __unused)
{
	TEE_Result ret = TEE_ERROR_GENERIC;

	if (!PM_HINT_IS_STATE(pm_hint, CONTEXT))
		return TEE_SUCCESS;

	if (op == PM_OP_RESUME)
		ret = stm32_gpio_sec_config_resume();
	else
		ret = stm32_gpio_sec_config_suspend();

	return ret;
}
DECLARE_KEEP_PAGER_PM(stm32_gpio_sec_config_pm);

/*
 * Several pinctrl nodes can be probed. Their bank will be put in the unique
 * bank_list. To avoid multiple configuration set for a bank when looping
 * over each bank in the bank list, ready is set to true when a bank is
 * configured. Therefore, during other bank probes, the configuration won't
 * be set again.
 */
static TEE_Result apply_sec_cfg(void)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	struct stm32_gpio_bank *bank = NULL;

	STAILQ_FOREACH(bank, &bank_list, link) {
		if (bank->ready)
			continue;

		if (bank->rif_cfg) {
			res = apply_rif_config(bank,
					       bank->rif_cfg->access_mask[0]);
			if (res) {
				EMSG("Failed to set GPIO RIF, bank %u",
				     bank->bank_id);
				STAILQ_REMOVE(&bank_list, bank, stm32_gpio_bank,
					      link);
				free(bank);
				return res;
			}
		} else {
			stm32_gpio_set_conf_sec(bank);
		}

		bank->ready = true;
	}

	return TEE_SUCCESS;
}

static TEE_Result stm32_pinctrl_probe(const void *fdt, int node,
				      const void *compat_data)
{
	static bool pm_register;
	TEE_Result res = TEE_ERROR_GENERIC;

	/* Register GPIO banks described in this pin control node */
	res = dt_stm32_gpio_pinctrl(fdt, node, compat_data);
	if (res)
		return res;

	if (STAILQ_EMPTY(&bank_list))
		DMSG("no gpio bank for that driver");
	else if (apply_sec_cfg())
		panic();

	if (!pm_register) {
		/*
		 * Register to PM once for all probed banks to restore
		 * their secure configuration.
		 */
		register_pm_core_service_cb(stm32_gpio_sec_config_pm, NULL,
					    "stm32-gpio-secure-config");
		pm_register = true;
	}

#ifdef CFG_DRIVERS_PINCTRL
	res = pinctrl_register_provider(fdt, node, stm32_pinctrl_dt_get,
					(void *)compat_data);
	if (res)
		panic();
#endif

	return TEE_SUCCESS;
}

static const struct dt_device_match stm32_pinctrl_match_table[] = {
	{
		.compatible = "st,stm32mp135-pinctrl",
		.compat_data = &(struct bank_compat){
			.secure_control = true,
			.secure_extended = false,
		},
	},
	{
		.compatible = "st,stm32mp157-pinctrl",
		.compat_data = &(struct bank_compat){
			.secure_control = false,
			.secure_extended = false,
		},
	},
	{
		.compatible = "st,stm32mp157-z-pinctrl",
		.compat_data = &(struct bank_compat){
			.gpioz = true,
			.secure_control = true,
			.secure_extended = false,
		},
	},
	{
		.compatible = "st,stm32mp257-pinctrl",
		.compat_data = &(struct bank_compat){
			.secure_control = true,
			.secure_extended = true,
		},
	},
	{
		.compatible = "st,stm32mp257-z-pinctrl",
		.compat_data = &(struct bank_compat){
			.gpioz = true,
			.secure_control = true,
			.secure_extended = true,
		},
	},
	{
		.compatible = "st,stm32mp215-pinctrl",
		.compat_data = &(struct bank_compat){
			.secure_control = true,
			.secure_extended = true,
		},
	},
	{
		.compatible = "st,stm32mp215-z-pinctrl",
		.compat_data = &(struct bank_compat){
			.gpioz = true,
			.secure_control = true,
			.secure_extended = true,
		},
	},
	{ }
};

DEFINE_DT_DRIVER(stm32_pinctrl_dt_driver) = {
	.name = "stm32_gpio-pinctrl",
	.type = DT_DRIVER_PINCTRL,
	.match_table = stm32_pinctrl_match_table,
	.probe = stm32_pinctrl_probe,
};
