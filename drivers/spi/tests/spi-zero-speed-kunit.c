// SPDX-License-Identifier: GPL-2.0
// Proof-of-concept KUnit test, not for upstream submission.
//
// Shows that a transfer on a device without a speed reaches the
// controller's transfer_one() with xfer->speed_hz == 0 when the controller
// (like spi-atmel) leaves ctlr->max_speed_hz unset, and what spi-atmel's
// divider computation does with that value.

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/compiler.h>
#include <linux/math.h>
#include <linux/spi/spi.h>

#define BUS_HZ		132000000UL	/* typical AT91 peripheral clock */
#define SPI_SCBR_SIZE	8		/* as in spi-atmel.c */

static u32 seen_speed_hz;
static int seen_scbr;
static bool use_fixed_formula;

/* The divider computation from atmel_spi_set_xfer_speed(). */
static noinline u32 atmel_scbr_unfixed(unsigned long bus_hz, u32 speed_hz)
{
	OPTIMIZER_HIDE_VAR(speed_hz);
	return DIV_ROUND_UP(bus_hz, speed_hz);
}

/* The same computation with the proposed fix applied. */
static noinline u32 atmel_scbr_fixed(unsigned long bus_hz, u32 speed_hz)
{
	OPTIMIZER_HIDE_VAR(speed_hz);
	if (speed_hz)
		return DIV_ROUND_UP(bus_hz, speed_hz);
	return 0xff;
}

static int zero_speed_transfer_one(struct spi_controller *ctlr,
				   struct spi_device *spi,
				   struct spi_transfer *xfer)
{
	seen_speed_hz = xfer->speed_hz;
	seen_scbr = -1;

	if (use_fixed_formula)
		seen_scbr = atmel_scbr_fixed(BUS_HZ, xfer->speed_hz);
	else if (IS_ENABLED(CONFIG_ARM))
		/* ARM has no divide trap; x86 would oops here, so skip it. */
		seen_scbr = atmel_scbr_unfixed(BUS_HZ, xfer->speed_hz);

	return 0;	/* transfer completed synchronously */
}

static void run_one_transfer(struct kunit *test, u32 ctlr_max_speed_hz,
			     u32 dev_max_speed_hz)
{
	struct spi_board_info info = {
		.modalias	= "kunit-zero-speed",
		.max_speed_hz	= dev_max_speed_hz,
		.chip_select	= 0,
	};
	struct spi_transfer xfer = {
		.len		= 4,
		/* .speed_hz left 0: "use the device default", as most callers do */
	};
	static u8 tx[4];
	struct spi_controller *ctlr;
	struct spi_device *spi;
	struct device *parent;
	int ret;

	xfer.tx_buf = tx;

	parent = kunit_device_register(test, "spi-zero-speed-parent");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, parent);

	ctlr = spi_alloc_host(parent, 0);
	KUNIT_ASSERT_NOT_NULL(test, ctlr);
	ctlr->bus_num = -1;
	ctlr->num_chipselect = 1;
	ctlr->transfer_one = zero_speed_transfer_one;
	ctlr->max_speed_hz = ctlr_max_speed_hz;	/* spi-atmel: never set */

	ret = spi_register_controller(ctlr);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* Same path as sysfs new_device / a DT node without spi-max-frequency */
	spi = spi_new_device(ctlr, &info);
	KUNIT_ASSERT_NOT_NULL(test, spi);
	kunit_info(test, "after spi_setup(): spi->max_speed_hz = %u\n",
		   spi->max_speed_hz);

	seen_speed_hz = U32_MAX;
	ret = spi_sync_transfer(spi, &xfer, 1);
	kunit_info(test, "spi_sync() = %d, transfer_one saw speed_hz = %u, scbr = %d\n",
		   ret, seen_speed_hz, seen_scbr);
	KUNIT_EXPECT_EQ(test, ret, 0);

	spi_unregister_device(spi);
	spi_unregister_controller(ctlr);
}

/* Controller without max_speed_hz (spi-atmel's situation). */
static void zero_speed_reaches_driver(struct kunit *test)
{
	use_fixed_formula = false;
	run_one_transfer(test, 0, 0);

	KUNIT_EXPECT_EQ_MSG(test, seen_speed_hz, 0U,
			    "a zero speed reached transfer_one(): the zero-speed branch is not dead code");
	if (IS_ENABLED(CONFIG_ARM))
		KUNIT_EXPECT_EQ_MSG(test, seen_scbr, 0,
				    "unfixed atmel divider computes scbr = 0 (\"too high\" error path)");
}

/* The same transfer with the proposed fix: slowest divider. */
static void zero_speed_fixed_uses_slowest_divider(struct kunit *test)
{
	use_fixed_formula = true;
	run_one_transfer(test, 0, 0);

	KUNIT_EXPECT_EQ(test, seen_speed_hz, 0U);
	KUNIT_EXPECT_EQ(test, seen_scbr, 0xff);
	KUNIT_EXPECT_LT(test, seen_scbr, 1 << SPI_SCBR_SIZE);
}

/* Control: a controller that sets max_speed_hz gets a real speed. */
static void zero_speed_clamped_when_ctlr_sets_max(struct kunit *test)
{
	use_fixed_formula = true;
	run_one_transfer(test, 1000000, 0);

	KUNIT_EXPECT_EQ(test, seen_speed_hz, 1000000U);
}

static struct kunit_case zero_speed_cases[] = {
	KUNIT_CASE(zero_speed_reaches_driver),
	KUNIT_CASE(zero_speed_fixed_uses_slowest_divider),
	KUNIT_CASE(zero_speed_clamped_when_ctlr_sets_max),
	{}
};

static struct kunit_suite zero_speed_suite = {
	.name = "spi-zero-speed",
	.test_cases = zero_speed_cases,
};
kunit_test_suite(zero_speed_suite);

MODULE_DESCRIPTION("Proof: zero SPI speed reaches controllers without max_speed_hz");
MODULE_LICENSE("GPL");
