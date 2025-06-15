/*
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hardware/flash.h"
#include "pico/bootrom.h"

#include "hardware/structs/io_qspi.h"
#if PICO_RP2040
#include "hardware/structs/ssi.h"
#else
#include "hardware/structs/qmi.h"
#include "hardware/regs/otp_data.h"
#endif
#include "hardware/xip_cache.h"

#define FLASH_BLOCK_ERASE_CMD 0xd8

// Standard RUID instruction: 4Bh command prefix, 32 dummy bits, 64 data bits.
#define FLASH_RUID_CMD 0x4b
#define FLASH_RUID_DUMMY_BYTES 4
#define FLASH_RUID_DATA_BYTES FLASH_UNIQUE_ID_SIZE_BYTES
#define FLASH_RUID_TOTAL_BYTES (1 + FLASH_RUID_DUMMY_BYTES + FLASH_RUID_DATA_BYTES)

void __no_inline_not_in_flash_func(flash_write_partial_internal)(uint32_t addr, const uint8_t *data, size_t size);

//-----------------------------------------------------------------------------
// Infrastructure for reentering XIP mode after exiting for programming (take
// a copy of boot2 before XIP exit). Calling boot2 as a function works because
// it accepts a return vector in LR (and doesn't trash r4-r7). Bootrom passes
// NULL in LR, instructing boot2 to enter flash vector table's reset handler.

#if !PICO_NO_FLASH

#define FLASHCMD_PAGE_PROGRAM 0x02
#define FLASHCMD_READ_STATUS 0x05
#define FLASHCMD_WRITE_ENABLE 0x06
#define BOOT2_SIZE_WORDS 64

enum outover {
	OUTOVER_NORMAL = 0,
	OUTOVER_INVERT,
	OUTOVER_LOW,
	OUTOVER_HIGH
};

static uint32_t boot2_copyout[BOOT2_SIZE_WORDS];
static bool boot2_copyout_valid = false;

static void __no_inline_not_in_flash_func(flash_init_boot2_copyout)(void) {
    if (boot2_copyout_valid)
        return;
    // todo we may want the option of boot2 just being a free function in
    //      user RAM, e.g. if it is larger than 256 bytes
#if PICO_RP2040
    const volatile uint32_t *copy_from = (uint32_t *)XIP_BASE;
#else
    const volatile uint32_t *copy_from = (uint32_t *)BOOTRAM_BASE;
#endif
    for (int i = 0; i < BOOT2_SIZE_WORDS; ++i)
        boot2_copyout[i] = copy_from[i];
    __compiler_memory_barrier();
    boot2_copyout_valid = true;
}


static void __no_inline_not_in_flash_func(flash_enable_xip_via_boot2)(void) {
    ((void (*)(void))((intptr_t)boot2_copyout+1))();
}

#else

static void __no_inline_not_in_flash_func(flash_init_boot2_copyout)(void) {}

static void __no_inline_not_in_flash_func(flash_enable_xip_via_boot2)(void) {
    // Set up XIP for 03h read on bus access (slow but generic)
    rom_flash_enter_cmd_xip_fn flash_enter_cmd_xip_func = (rom_flash_enter_cmd_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_ENTER_CMD_XIP);
    assert(flash_enter_cmd_xip_func);
    flash_enter_cmd_xip_func();
}

#endif

#if PICO_RP2350
// This is specifically for saving/restoring the registers modified by RP2350
// flash_exit_xip() ROM func, not the entirety of the QMI window state.
typedef struct flash_rp2350_qmi_save_state {
    uint32_t timing;
    uint32_t rcmd;
    uint32_t rfmt;
} flash_rp2350_qmi_save_state_t;

static void __no_inline_not_in_flash_func(flash_rp2350_save_qmi_cs1)(flash_rp2350_qmi_save_state_t *state) {
    state->timing = qmi_hw->m[1].timing;
    state->rcmd = qmi_hw->m[1].rcmd;
    state->rfmt = qmi_hw->m[1].rfmt;
}

static void __no_inline_not_in_flash_func(flash_rp2350_restore_qmi_cs1)(const flash_rp2350_qmi_save_state_t *state) {
    if (flash_devinfo_get_cs_size(1) == FLASH_DEVINFO_SIZE_NONE) {
        // Case 1: The RP2350 ROM sets QMI to a clean (03h read) configuration
        // during flash_exit_xip(), even though when CS1 is not enabled via
        // FLASH_DEVINFO it does not issue an XIP exit sequence to CS1. In
        // this case, restore the original register config for CS1 as it is
        // still the correct config.
        qmi_hw->m[1].timing = state->timing;
        qmi_hw->m[1].rcmd = state->rcmd;
        qmi_hw->m[1].rfmt = state->rfmt;
    } else {
        // Case 2: If RAM is attached to CS1, and the ROM has issued an XIP
        // exit sequence to it, then the ROM re-initialisation of the QMI
        // registers has actually not gone far enough. The old XIP write mode
        // is no longer valid when the QSPI RAM is returned to a serial
        // command state. Restore the default 02h serial write command config.
        qmi_hw->m[1].wfmt = QMI_M1_WFMT_RESET;
        qmi_hw->m[1].wcmd = QMI_M1_WCMD_RESET;
    }
}
#endif

//-----------------------------------------------------------------------------
// Actual flash programming shims (work whether or not PICO_NO_FLASH==1)

void __no_inline_not_in_flash_func(flash_range_erase)(uint32_t flash_offs, size_t count) {
#ifdef PICO_FLASH_SIZE_BYTES
    hard_assert(flash_offs + count <= PICO_FLASH_SIZE_BYTES);
#endif
    invalid_params_if(HARDWARE_FLASH, flash_offs & (FLASH_SECTOR_SIZE - 1));
    invalid_params_if(HARDWARE_FLASH, count & (FLASH_SECTOR_SIZE - 1));
    rom_connect_internal_flash_fn connect_internal_flash_func = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn flash_exit_xip_func = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_range_erase_fn flash_range_erase_func = (rom_flash_range_erase_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_ERASE);
    rom_flash_flush_cache_fn flash_flush_cache_func = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    assert(connect_internal_flash_func && flash_exit_xip_func && flash_range_erase_func && flash_flush_cache_func);
    flash_init_boot2_copyout();
    // Commit any pending writes to external RAM, to avoid losing them in the subsequent flush:
    xip_cache_clean_all();
#if PICO_RP2350
    flash_rp2350_qmi_save_state_t qmi_save;
    flash_rp2350_save_qmi_cs1(&qmi_save);
#endif

    // No flash accesses after this point
    __compiler_memory_barrier();

    connect_internal_flash_func();
    flash_exit_xip_func();
    flash_range_erase_func(flash_offs, count, FLASH_BLOCK_SIZE, FLASH_BLOCK_ERASE_CMD);
    flash_flush_cache_func(); // Note this is needed to remove CSn IO force as well as cache flushing
    flash_enable_xip_via_boot2();
#if PICO_RP2350
    flash_rp2350_restore_qmi_cs1(&qmi_save);
#endif
}

void __no_inline_not_in_flash_func(flash_flush_cache)(void) {
    rom_flash_flush_cache_fn flash_flush_cache_func = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    flash_flush_cache_func();
}

void __no_inline_not_in_flash_func(flash_range_program)(uint32_t flash_offs, const uint8_t *data, size_t count) {
#ifdef PICO_FLASH_SIZE_BYTES
    hard_assert(flash_offs + count <= PICO_FLASH_SIZE_BYTES);
#endif
    invalid_params_if(HARDWARE_FLASH, flash_offs & (FLASH_PAGE_SIZE - 1));
    invalid_params_if(HARDWARE_FLASH, count & (FLASH_PAGE_SIZE - 1));
    rom_connect_internal_flash_fn connect_internal_flash_func = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn flash_exit_xip_func = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_range_program_fn flash_range_program_func = (rom_flash_range_program_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_PROGRAM);
    rom_flash_flush_cache_fn flash_flush_cache_func = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    assert(connect_internal_flash_func && flash_exit_xip_func && flash_range_program_func && flash_flush_cache_func);
    flash_init_boot2_copyout();
    xip_cache_clean_all();
#if PICO_RP2350
    flash_rp2350_qmi_save_state_t qmi_save;
    flash_rp2350_save_qmi_cs1(&qmi_save);
#endif

    __compiler_memory_barrier();

    connect_internal_flash_func();
    flash_exit_xip_func();
    flash_range_program_func(flash_offs, data, count);
    flash_flush_cache_func(); // Note this is needed to remove CSn IO force as well as cache flushing
    flash_enable_xip_via_boot2();
#if PICO_RP2350
    flash_rp2350_restore_qmi_cs1(&qmi_save);
#endif
}

void __no_inline_not_in_flash_func(flash_write_partial)(uint32_t flash_offs, const uint8_t *data, size_t count) {
    rom_connect_internal_flash_fn connect_internal_flash_func = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn flash_exit_xip_func = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_flush_cache_fn flash_flush_cache_func = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    assert(connect_internal_flash_func && flash_exit_xip_func && flash_flush_cache_func);
    flash_init_boot2_copyout();
    xip_cache_clean_all();
#if PICO_RP2350
    flash_rp2350_qmi_save_state_t qmi_save;
    flash_rp2350_save_qmi_cs1(&qmi_save);
#endif

    __compiler_memory_barrier();

    connect_internal_flash_func();
    flash_exit_xip_func();
    flash_write_partial_internal(flash_offs, data, count);
    flash_flush_cache_func(); // Note this is needed to remove CSn IO force as well as cache flushing
    flash_enable_xip_via_boot2();
#if PICO_RP2350
    flash_rp2350_restore_qmi_cs1(&qmi_save);
#endif
}

//-----------------------------------------------------------------------------
// Lower-level flash access functions

#if !PICO_NO_FLASH
// Bitbanging the chip select using IO overrides, in case RAM-resident IRQs
// are still running, and the FIFO bottoms out. (the bootrom does the same)
static void __no_inline_not_in_flash_func(flash_cs_force)(bool high) {
#if PICO_RP2040
    uint32_t field_val = high ?
        IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_VALUE_HIGH :
        IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_VALUE_LOW;
    hw_write_masked(&io_qspi_hw->io[1].ctrl,
        field_val << IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_LSB,
        IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_BITS
    );
#else
    if (high) {
        hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS0N_BITS);
    } else {
        hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS0N_BITS);
    }
#endif
}

void __no_inline_not_in_flash_func(flash_do_cmd)(const uint8_t *txbuf, uint8_t *rxbuf, size_t count) {
    rom_connect_internal_flash_fn connect_internal_flash_func = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn flash_exit_xip_func = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_flush_cache_fn flash_flush_cache_func = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    assert(connect_internal_flash_func && flash_exit_xip_func && flash_flush_cache_func);
    flash_init_boot2_copyout();
    xip_cache_clean_all();
#if PICO_RP2350
    flash_rp2350_qmi_save_state_t qmi_save;
    flash_rp2350_save_qmi_cs1(&qmi_save);
#endif

    __compiler_memory_barrier();
    connect_internal_flash_func();
    flash_exit_xip_func();

    flash_cs_force(0);
    size_t tx_remaining = count;
    size_t rx_remaining = count;
#if PICO_RP2040
    // Synopsys SSI version
    // We may be interrupted -- don't want FIFO to overflow if we're distracted.
    const size_t max_in_flight = 16 - 2;
    while (tx_remaining || rx_remaining) {
        uint32_t flags = ssi_hw->sr;
        bool can_put = flags & SSI_SR_TFNF_BITS;
        bool can_get = flags & SSI_SR_RFNE_BITS;
        if (can_put && tx_remaining && rx_remaining - tx_remaining < max_in_flight) {
            ssi_hw->dr0 = *txbuf++;
            --tx_remaining;
        }
        if (can_get && rx_remaining) {
            *rxbuf++ = (uint8_t)ssi_hw->dr0;
            --rx_remaining;
        }
    }
#else
    // QMI version -- no need to bound FIFO contents as QMI stalls on full DIRECT_RX.
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    while (tx_remaining || rx_remaining) {
        uint32_t flags = qmi_hw->direct_csr;
        bool can_put = !(flags & QMI_DIRECT_CSR_TXFULL_BITS);
        bool can_get = !(flags & QMI_DIRECT_CSR_RXEMPTY_BITS);
        if (can_put && tx_remaining) {
            qmi_hw->direct_tx = *txbuf++;
            --tx_remaining;
        }
        if (can_get && rx_remaining) {
            *rxbuf++ = (uint8_t)qmi_hw->direct_rx;
            --rx_remaining;
        }
    }
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
#endif
    flash_cs_force(1);

    flash_flush_cache_func();
    flash_enable_xip_via_boot2();
#if PICO_RP2350
    flash_rp2350_restore_qmi_cs1(&qmi_save);
#endif
}
#endif

// Use standard RUID command to get a unique identifier for the flash (and
// hence the board)

static_assert(FLASH_UNIQUE_ID_SIZE_BYTES == FLASH_RUID_DATA_BYTES, "");

void flash_get_unique_id(uint8_t *id_out) {
#if PICO_NO_FLASH
    __unused uint8_t *ignore = id_out;
    panic_unsupported();
#else
    uint8_t txbuf[FLASH_RUID_TOTAL_BYTES] = {0};
    uint8_t rxbuf[FLASH_RUID_TOTAL_BYTES] = {0};
    txbuf[0] = FLASH_RUID_CMD;
    flash_do_cmd(txbuf, rxbuf, FLASH_RUID_TOTAL_BYTES);
    for (int i = 0; i < FLASH_RUID_DATA_BYTES; i++)
        id_out[i] = rxbuf[i + 1 + FLASH_RUID_DUMMY_BYTES];
#endif
}

#if !PICO_RP2040
// This is a static symbol because the layout of FLASH_DEVINFO is liable to change from device to
// device, so fields must have getters/setters.
static io_rw_16 * flash_devinfo_ptr(void) {
    // Note the lookup returns a pointer to a 32-bit pointer literal in the ROM
    io_rw_16 **p = (io_rw_16 **) rom_data_lookup(ROM_DATA_FLASH_DEVINFO16_PTR);
    assert(p);
    return *p;
}

static void flash_devinfo_update_field(uint16_t wdata, uint16_t mask) {
    // Boot RAM does not support exclusives, but does support RWTYPE SET/CLR/XOR (with byte
    // strobes). Can't use hw_write_masked because it performs a 32-bit write.
    io_rw_16 *devinfo = flash_devinfo_ptr();
    *hw_xor_alias(devinfo) = (*devinfo ^ wdata) & mask;
}

// This is a RAM function because may be called during flash programming to enable save/restore of
// QMI window 1 registers on RP2350:
flash_devinfo_size_t __no_inline_not_in_flash_func(flash_devinfo_get_cs_size)(uint cs) {
    invalid_params_if(HARDWARE_FLASH, cs > 1);
    io_ro_16 *devinfo = (io_ro_16 *) flash_devinfo_ptr();
    if (cs == 0u) {
#ifdef PICO_FLASH_SIZE_BYTES
        // A flash size explicitly specified for the build (e.g. from the board header) takes
        // precedence over whatever was found in OTP. Not using flash_devinfo_bytes_to_size() as
        // the call could be outlined, and this code must be in RAM.
        if (PICO_FLASH_SIZE_BYTES == 0) {
            return FLASH_DEVINFO_SIZE_NONE;
        } else {
            return (flash_devinfo_size_t) (
                __builtin_ctz(PICO_FLASH_SIZE_BYTES / 8192u) + (uint)FLASH_DEVINFO_SIZE_8K
            );
        }
#else
        return (flash_devinfo_size_t) (
            (*devinfo & OTP_DATA_FLASH_DEVINFO_CS0_SIZE_BITS) >> OTP_DATA_FLASH_DEVINFO_CS0_SIZE_LSB
        );
#endif
    } else {
        return (flash_devinfo_size_t) (
            (*devinfo & OTP_DATA_FLASH_DEVINFO_CS1_SIZE_BITS) >> OTP_DATA_FLASH_DEVINFO_CS1_SIZE_LSB
        );
    }
}

void flash_devinfo_set_cs_size(uint cs, flash_devinfo_size_t size) {
    invalid_params_if(HARDWARE_FLASH, cs > 1);
    invalid_params_if(HARDWARE_FLASH, (uint)size > (uint)FLASH_DEVINFO_SIZE_MAX);
    uint cs_shift = cs == 0u ? OTP_DATA_FLASH_DEVINFO_CS0_SIZE_LSB : OTP_DATA_FLASH_DEVINFO_CS1_SIZE_LSB;
    uint16_t cs_mask = OTP_DATA_FLASH_DEVINFO_CS0_SIZE_BITS >> OTP_DATA_FLASH_DEVINFO_CS0_SIZE_LSB;
    flash_devinfo_update_field(
        (uint16_t)size << cs_shift,
        cs_mask << cs_shift
    );
}

bool flash_devinfo_get_d8h_erase_supported(void) {
    return *flash_devinfo_ptr() & OTP_DATA_FLASH_DEVINFO_D8H_ERASE_SUPPORTED_BITS;
}

void flash_devinfo_set_d8h_erase_supported(bool supported) {
    flash_devinfo_update_field(
        (uint)supported << OTP_DATA_FLASH_DEVINFO_D8H_ERASE_SUPPORTED_LSB,
        OTP_DATA_FLASH_DEVINFO_D8H_ERASE_SUPPORTED_BITS
    );
}

uint flash_devinfo_get_cs_gpio(uint cs) {
    invalid_params_if(HARDWARE_FLASH, cs != 1);
    (void)cs;
    return (*flash_devinfo_ptr() & OTP_DATA_FLASH_DEVINFO_CS1_GPIO_BITS) >> OTP_DATA_FLASH_DEVINFO_CS1_GPIO_LSB;
}

void flash_devinfo_set_cs_gpio(uint cs, uint gpio) {
    invalid_params_if(HARDWARE_FLASH, cs != 1);
    invalid_params_if(HARDWARE_FLASH, gpio >= NUM_BANK0_GPIOS);
    (void)cs;
    flash_devinfo_update_field(
        ((uint16_t)gpio) << OTP_DATA_FLASH_DEVINFO_CS1_GPIO_LSB,
        OTP_DATA_FLASH_DEVINFO_CS1_GPIO_BITS
    );
}

#endif // !PICO_RP2040
//-----------------------------------------------------------------------------
/**
 * Low level flash functions are based on:
 * github.com/raspberrypi/pico-bootrom-rp2040/blob/master/bootrom/program_flash_generic.c,
 * github.com/raspberrypi/pico-bootrom-rp2350/blob/master/src/main/arm/varm_generic_flash.c
 */
static int __no_inline_not_in_flash_func(flash_was_aborted)()
{
	return *(io_rw_32 *)(IO_QSPI_BASE + IO_QSPI_GPIO_QSPI_SD1_CTRL_OFFSET) &
	       IO_QSPI_GPIO_QSPI_SD1_CTRL_INOVER_BITS;
}

#if PICO_RP2350
static uint __no_inline_not_in_flash_func(flash_put_get)(uint cs, const uint8_t *tx, uint8_t *rx, size_t count)
{
	/* Assert chip select, and enable direct mode. Anything queued in TX FIFO will start now. */
	uint32_t csr_toggle_mask = (QMI_DIRECT_CSR_ASSERT_CS0N_BITS << cs) | QMI_DIRECT_CSR_EN_BITS;

	hw_xor_bits(&qmi_hw->direct_csr, csr_toggle_mask);

	size_t tx_count = count;
	size_t rx_count = count;

	while (tx_count || rx_count) {
		uint32_t status = qmi_hw->direct_csr;

		if (tx_count && !(status & QMI_DIRECT_CSR_TXFULL_BITS)) {
			qmi_hw->direct_tx = (uint32_t)(tx ? *tx++ : 0);
			--tx_count;
		}
		if (rx_count && !(status & QMI_DIRECT_CSR_RXEMPTY_BITS)) {
			uint8_t rxbyte = (uint8_t)qmi_hw->direct_rx;

			if (rx) {
				*rx++ = rxbyte;
			}
			--rx_count;
		}
	}

	/* Wait for BUSY as there may be no RX data at all, e.g. for single-byte SPI commands */
	while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
	}

	/* Disable direct-mode interface and deassert chip select */
	hw_xor_bits(&qmi_hw->direct_csr, csr_toggle_mask);
	return cs;
}
#else
static void __no_inline_not_in_flash_func(flash_put_get)(const uint8_t *tx, uint8_t *rx, size_t count, size_t rx_skip)
{
	const uint max_in_flight = 16 - 2;
	size_t tx_count = count;
	size_t rx_count = count;
	bool did_something;
	uint32_t tx_level;
	uint32_t rx_level;
	uint8_t rxbyte;

	while (tx_count || rx_skip || rx_count) {
		tx_level = ssi_hw->txflr;
		rx_level = ssi_hw->rxflr;
		did_something = false;
		if (tx_count && tx_level + rx_level < max_in_flight) {
			ssi_hw->dr0 = (uint32_t)(tx ? *tx++ : 0);
			--tx_count;
			did_something = true;
		}
		if (rx_level) {
			rxbyte = ssi_hw->dr0;
			did_something = true;
			if (rx_skip) {
				--rx_skip;
			} else {
				if (rx) {
					*rx++ = rxbyte;
				}
				--rx_count;
			}
		}

		if (!did_something && __builtin_expect(flash_was_aborted(), 0)) {
			break;
		}
	}
	flash_cs_force(OUTOVER_HIGH);
}
#endif

static inline void flash_wait_ready(uint cs)
{
	uint8_t status_reg;
#if PICO_RP2040
	__unused uint ignore = cs;
#endif

	do {
#if PICO_RP2350
		qmi_hw->direct_tx = FLASHCMD_READ_STATUS | QMI_DIRECT_TX_NOPUSH_BITS;
		cs = flash_put_get(cs, NULL, &status_reg, 1);
#else
		flash_cs_force(OUTOVER_LOW);
		ssi_hw->dr0 = FLASHCMD_READ_STATUS;
		flash_put_get(NULL, &status_reg, 1, 1);
#endif
	} while (status_reg & 0x1 && !flash_was_aborted());
}

static inline void flash_enable_write(uint cs)
{
#if PICO_RP2350
	qmi_hw->direct_tx = FLASHCMD_WRITE_ENABLE | QMI_DIRECT_TX_NOPUSH_BITS;
	flash_put_get(cs, NULL, NULL, 0);
#else
	__unused uint ignore = cs;
	flash_cs_force(OUTOVER_LOW);
	ssi_hw->dr0 = FLASHCMD_WRITE_ENABLE;
	flash_put_get(NULL, NULL, 0, 1);
#endif
}

static inline void flash_put_cmd_addr(uint8_t cmd, uint32_t addr)
{
#if PICO_RP2350
	addr = __builtin_bswap32(addr & ((1u << 24) - 1));
	addr |= cmd;
	qmi_hw->direct_tx =
		((addr << 16) >> 16) | QMI_DIRECT_TX_NOPUSH_BITS | QMI_DIRECT_TX_DWIDTH_BITS;
	qmi_hw->direct_tx = (addr >> 16) | QMI_DIRECT_TX_NOPUSH_BITS | QMI_DIRECT_TX_DWIDTH_BITS;
#else
	flash_cs_force(OUTOVER_LOW);
	addr |= cmd << 24;
	for (int i = 0; i < 4; ++i) {
		ssi_hw->dr0 = addr >> 24;
		addr <<= 8;
	}
#endif
}

void __no_inline_not_in_flash_func(flash_write_partial_internal)(uint32_t addr, const uint8_t *data, size_t size)
{
	uint cs = (addr >> 24) & 0x1u;

	flash_enable_write(cs);
	flash_put_cmd_addr(FLASHCMD_PAGE_PROGRAM, addr);
#if PICO_RP2350
	flash_put_get(cs, data, NULL, size);
#else
	flash_put_get(data, NULL, size, 4);
#endif
	flash_wait_ready(cs);
}
