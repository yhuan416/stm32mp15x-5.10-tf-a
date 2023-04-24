/*
 * Copyright (C) 2018-2021, STMicroelectronics - All Rights Reserved
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef STM32MP_COMMON_H
#define STM32MP_COMMON_H

#include <stdbool.h>

#include <platform_def.h>

#define JEDEC_ST_BKID U(0x0)
#define JEDEC_ST_MFID U(0x20)

/* Functions to save and get boot context address given by ROM code */
void stm32mp_save_boot_ctx_address(uintptr_t address);
uintptr_t stm32mp_get_boot_ctx_address(void);
uint16_t stm32mp_get_boot_itf_selected(void);
uint32_t stm32mp_get_boot_action(void);

bool stm32mp_is_single_core(void);
bool stm32mp_is_closed_device(void);
bool stm32mp_is_auth_supported(void);

/* Return the base address of the DDR controller */
uintptr_t stm32mp_ddrctrl_base(void);

/* Return the base address of the DDR PHY */
uintptr_t stm32mp_ddrphyc_base(void);

/* Return the base address of the PWR peripheral */
uintptr_t stm32mp_pwr_base(void);

/* Return the base address of the RCC peripheral */
uintptr_t stm32mp_rcc_base(void);

void stm32_gic_pcpu_init(void);
void stm32_gic_init(void);
int stm32_gic_enable_spi(int node, const char *name);

/* Check MMU status to allow spinlock use */
bool stm32mp_lock_available(void);

/* SMP protection on PWR registers access */
void stm32mp_pwr_regs_lock(void);
void stm32mp_pwr_regs_unlock(void);

int stm32_get_otp_index(const char *otp_name, uint32_t *otp_idx,
			uint32_t *otp_len);
int stm32_get_otp_value(const char *otp_name, uint32_t *otp_val);
int stm32_get_otp_value_from_idx(const uint32_t otp_idx, uint32_t *otp_val);

/* Get IWDG platform instance ID from peripheral IO memory base address */
uint32_t stm32_iwdg_get_instance(uintptr_t base);

/* Return bitflag mask for expected IWDG configuration from OTP content */
uint32_t stm32_iwdg_get_otp_config(uint32_t iwdg_inst);

#if defined(IMAGE_BL2)
/* Update OTP shadow registers with IWDG configuration from device tree */
uint32_t stm32_iwdg_shadow_update(uint32_t iwdg_inst, uint32_t flags);
#endif

#if STM32MP_UART_PROGRAMMER || defined(IMAGE_BL32)
/* Get the UART address from its instance number */
uintptr_t get_uart_address(uint32_t instance_nb);
#endif

/*
 * Platform util functions for the GPIO driver
 * @bank: Target GPIO bank ID as per DT bindings
 *
 * Platform shall implement these functions to provide to stm32_gpio
 * driver the resource reference for a target GPIO bank. That are
 * memory mapped interface base address, interface offset (see below)
 * and clock identifier.
 *
 * stm32_get_gpio_bank_offset() returns a bank offset that is used to
 * check DT configuration matches platform implementation of the banks
 * description.
 */
uintptr_t stm32_get_gpio_bank_base(unsigned int bank);
unsigned long stm32_get_gpio_bank_clock(unsigned int bank);
uint32_t stm32_get_gpio_bank_offset(unsigned int bank);
bool stm32_gpio_is_secure_at_reset(unsigned int bank);

/* Return node offset for target GPIO bank ID @bank or a FDT error code */
int stm32_get_gpio_bank_pinctrl_node(void *fdt, unsigned int bank);

/* Get the chip revision */
uint32_t stm32mp_get_chip_version(void);
/* Get the chip device ID */
uint32_t stm32mp_get_chip_dev_id(void);

/* Get SOC name */
#define STM32_SOC_NAME_SIZE 20
void stm32mp_get_soc_name(char name[STM32_SOC_NAME_SIZE]);

/* Print CPU information */
void stm32mp_print_cpuinfo(void);

/* Print board information */
void stm32mp_print_boardinfo(void);

/* Check HW CPU OPP support */
bool stm32mp_supports_cpu_opp(uint32_t opp_id);

/* Initialise the IO layer and register platform IO devices */
void stm32mp_io_setup(void);

#if STM32MP_USE_STM32IMAGE
/*
 * Check that the STM32 header of a .stm32 binary image is valid
 * @param header: pointer to the stm32 image header
 * @param buffer: address of the binary image (payload)
 * @return: 0 on success, negative value in case of error
 */
int stm32mp_check_header(boot_api_image_header_t *header, uintptr_t buffer);
#endif

#if TRUSTED_BOARD_BOOT
void stm32mp_save_loaded_header(void *header);
void stm32mp_delete_loaded_header(void);
boot_api_image_header_t *stm32mp_get_loaded_header(void);
#endif

/* Functions to map DDR in MMU with non-cacheable attribute, and unmap it */
int stm32mp_map_ddr_non_cacheable(void);
int stm32mp_unmap_ddr(void);

#endif /* STM32MP_COMMON_H */
