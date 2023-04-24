/*
 * Copyright (c) 2020-2021, STMicroelectronics - All Rights Reserved
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <platform_def.h>

#include <arch_helpers.h>
#include <common/debug.h>
#include <drivers/delay_timer.h>
#include <drivers/st/stm32_iwdg.h>
#include <drivers/st/stm32_uart.h>
#include <drivers/st/stm32_uart_regs.h>
#include <lib/mmio.h>
#include <tools_share/firmware_image_package.h>

#include <stm32cubeprogrammer.h>

#define PROGRAMMER_TIMEOUT_US	20000U

/* USART bootloader protocol version V4.0 */
#define USART_BL_VERSION	0x40
#define UNDEFINED_DOWN_ADDR	0xFFFFFFFF

static const uint8_t command_tab[] = {
	GET_CMD_COMMAND,
	GET_VER_COMMAND,
	GET_ID_COMMAND,
	PHASE_COMMAND,
#if STM32MP_SSP
	READ_PART_COMMAND,
#endif
	START_COMMAND,
	DOWNLOAD_COMMAND
};

/* STM32CubeProgrammer over UART handle */
struct stm32prog_uart_handle_s {
	struct stm32_uart_handle_s uart;
	uint32_t packet;
	uint8_t *addr;
	uint32_t len;
	uint8_t phase;
#if STM32MP_SSP
	uintptr_t cert_base;
	size_t cert_len;
#endif
	/* error msg buffer: max 255 in UART protocol, reduced in TF-A */
	uint8_t error[64];
} handle;

/* Trace and handle unrecoverable UART protocol error */
#define STM32PROG_ERROR(...) \
	{ \
		ERROR(__VA_ARGS__); \
		if (handle.phase != PHASE_RESET) { \
			snprintf((char *)&handle.error, sizeof(handle.error), __VA_ARGS__); \
			handle.phase = PHASE_RESET; \
			handle.addr = (uint8_t *)UNDEFINED_DOWN_ADDR; \
			handle.len = 0U; \
			handle.packet = 0U; \
		} \
	}

static int uart_write(const uint8_t *addr, uint16_t size)
{
	while (size) {
		if (stm32_uart_putc(&handle.uart, *addr) != 0) {
			return -EIO;
		}

		size--;
		addr++;
	}

	return 0;
}

static int uart_write_8(uint8_t byte)
{
	return stm32_uart_putc(&handle.uart, byte);
}

static int uart_write_32(uint32_t value)
{
	return uart_write((uint8_t *)&value, 4U);
}

static int uart_read_8(uint8_t *byte)
{
	int ret;
	uint64_t timeout_ref = timeout_init_us(PROGRAMMER_TIMEOUT_US);

	do {
		ret = stm32_uart_getc(&handle.uart);
		if (ret == -EAGAIN) {
			if (timeout_elapsed(timeout_ref)) {
				return -ETIMEDOUT;
			}
		} else if (ret < 0) {
			return ret;
		}
	} while (ret == -EAGAIN);

	*byte = (uint8_t)ret;

	return 0;
}

static int uart_flush_and_nack(void)
{
	int ret;

	/* read all pending data */
	do {
		ret = stm32_uart_getc(&handle.uart);
	} while (ret >= 0);

	return uart_write_8(NACK_BYTE);
}

static inline int is_valid_header(fip_toc_header_t *header)
{
	if ((header->name == TOC_HEADER_NAME) && (header->serial_number != 0)) {
		return 1;
	} else {
		return 0;
	}
}

static int uart_receive_command(uint8_t *command)
{
	uint8_t byte = 0U;
	uint8_t xor = 0U;
	unsigned int count;
	bool found = false;
	int ret;

	ret = uart_read_8(&byte);
	if (ret != 0) {
		return ret;
	}

	/* handle reconnection request */
	if (byte == INIT_BYTE) {
		*command = byte;
		return 0;
	}

	for (count = 0U; count < ARRAY_SIZE(command_tab); count++) {
		if (command_tab[count] == byte) {
			found = true;
			break;
		}
	}

	if (!found) {
		VERBOSE("UART: Command unknown (byte=0x%x)\n", byte);
		return -EPROTO;
	}

	ret = uart_read_8(&xor);
	if (ret != 0) {
		return ret;
	}

	if ((byte ^ xor) != 0xFF) {
		VERBOSE("UART: Command XOR check fail (byte=0x%x, xor=0x%x)\n",
			byte, xor);
		return -EPROTO;
	}

	*command = byte;

	return 0;
}

static int get_cmd_command(void)
{
	int ret;
	const uint8_t msg[2] = {
		sizeof(command_tab), /* Length of data - 1 */
		USART_BL_VERSION
	};

	ret = uart_write(msg, sizeof(msg));
	if (ret != 0) {
		return ret;
	}

	return uart_write(command_tab, sizeof(command_tab));
}

static int get_version_command(void)
{
	return uart_write_8(STM32_TF_VERSION);
}

static int get_id_command(void)
{
	uint32_t id = stm32mp_get_chip_dev_id();
	int ret;

	ret = uart_write_8(2U); /* Length of command */
	if (ret != 0) {
		return ret;
	}

	ret = uart_write_8((id & 0xFF00) >> 8); /* device ID byte 1 */
	if (ret != 0) {
		return ret;
	}

	return uart_write_8(id & 0xFF); /* device ID byte 2 */
}

static int uart_send_phase(uint32_t address)
{
	int ret;
	uint8_t msg_size = 5U; /* Length of data - 1 */
	uint8_t error_size = 0U;

	/* additionnal information only for RESET phase */
	if (handle.phase == PHASE_RESET) {
		error_size = strnlen((char *)&handle.error, sizeof(handle.error));
	}

	ret = uart_write_8(msg_size + error_size);
	if (ret != 0) {
		return ret;
	}

	/* Send the ID of next partition */
	ret = uart_write_8(handle.phase);
	if (ret != 0) {
		return ret;
	}

	/* Destination address */
	ret = uart_write_32(address);
	if (ret != 0) {
		return ret;
	}

	ret = uart_write_8(error_size);
	if (ret != 0) {
		return ret;
	}

	/* Additional information: message error */
	if (error_size > 0U) {
		ret = uart_write(handle.error, error_size);
	}

	return ret;
}

static int uart_download_part(void)
{
	uint8_t operation = 0U;
	uint8_t xor;
	uint8_t byte = 0U;
	uint32_t packet_number = 0U;
	uint32_t packet_size = 0U;
	uint32_t i = 0;
	int ret;

	/* Get operation number */
	ret = uart_read_8(&operation);
	if (ret != 0) {
		return ret;
	}

	xor = operation;

	/* Get packet Number */
	for (i = 3U; i > 0U; i--) {
		ret = uart_read_8(&byte);
		if (ret != 0) {
			return ret;
		}

		xor ^= byte;
		packet_number = (packet_number << 8) | byte;
	}

	if (packet_number != handle.packet) {
		WARN("UART: Bad packet number receive: %i, expected %i\n",
		     packet_number, handle.packet);
		return -EPROTO;
	}

	/* Checksum */
	ret = uart_read_8(&byte);
	if (ret != 0) {
		return ret;
	}

	if (xor != byte) {
		VERBOSE("UART: Download Command checksum xor: %x, received %x\n",
			xor, byte);
		return -EPROTO;
	}

	ret = uart_write_8(ACK_BYTE);
	if (ret != 0) {
		return ret;
	}

	ret = uart_read_8(&byte);
	if (ret != 0) {
		return ret;
	}

	xor = byte;
	packet_size = byte + 1U;
	if (handle.len < packet_size) {
		STM32PROG_ERROR("Download overflow at %p\n", handle.addr + packet_size);
		return 0;
	}

	for (i = 0U; i < packet_size; i++) {
		ret = uart_read_8(&byte);
		if (ret != 0) {
			return ret;
		}

		*(handle.addr + i) = byte;
		xor ^= byte;
	}

	/* Checksum */
	ret = uart_read_8(&byte) != 0;
	if (ret != 0) {
		return ret;
	}

	if (xor != byte) {
		VERBOSE("UART: Download Data checksum xor: %x, received %x\n",
			xor, byte);
		return -EPROTO;
	}

	/* packet treated */
	handle.packet++;
	handle.addr += packet_size;
	handle.len -= packet_size;

	return 0;
}

static int uart_start_cmd(unsigned int image_id, uintptr_t buffer)
{
	uint8_t byte = 0U;
	uint8_t xor = 0U;
	int8_t i;
	uint32_t start_address = 0U;
	int ret;

	/* Get address */
	for (i = 4; i > 0; i--) {
		ret = uart_read_8(&byte);
		if (ret != 0) {
			return ret;
		}

		xor ^= byte;
		start_address = (start_address << 8) | byte;
	}

	/* Checksum */
	ret = uart_read_8(&byte);
	if (ret != 0) {
		return ret;
	}

	if (xor != byte) {
		VERBOSE("UART: Start Command checksum xor: %x, received %x\n",
			xor, byte);
		return -EPROTO;
	}

	if (start_address != UNDEFINED_DOWN_ADDR) {
		STM32PROG_ERROR("Invalid start at %x, for phase %d\n",
				start_address, handle.phase);
		return 0;
	}

#if !STM32MP_USE_STM32IMAGE
	if (image_id == FIP_IMAGE_ID) {
		if (!is_valid_header((fip_toc_header_t *)buffer)) {
			STM32PROG_ERROR("FIP Header check failed at phase %d\n",
					(uint32_t)buffer);
			return -EIO;
		}

		VERBOSE("FIP header looks OK.\n");
	}
#else
	if (image_id == STM32_IMAGE_ID) {
		/* Verify header and checksum payload */
		ret = stm32mp_check_header((boot_api_image_header_t *)buffer,
					   buffer + sizeof(boot_api_image_header_t));
		if (ret != 0U) {
			STM32PROG_ERROR("STM32IMAGE check error at %x\n",
					(uint32_t)buffer);
			return -EIO;
		}

		VERBOSE("STM32 header looks OK.\n");
	}
#endif

	return 0;
}

#if STM32MP_SSP
static int uart_read_part(void)
{
	uint8_t byte = 0U;
	uint8_t xor = 0U;
	uint8_t partid = 0U;
	uint16_t size = 0U;
	uint32_t start_address = 0U;
	uint32_t i;
	size_t length;
	uint8_t *buffer;

	/* Get partition id */
	if (uart_read_8(&partid) != 0) {
		return -EIO;
	}

	if ((partid != PHASE_FLASHLAYOUT) && (partid != PHASE_SSP)) {
		return -EPERM;
	}

	xor = partid;

	/* Get address */
	for (i = 4U; i > 0U; i--) {
		if (uart_read_8(&byte) != 0) {
			return -EIO;
		}

		xor ^= byte;
		start_address = (start_address << 8) | byte;
	}

	/* Checksum */
	if (uart_read_8(&byte) != 0) {
		return -EIO;
	}

	if (xor != byte) {
		WARN("UART: Start cmd: address checksum: %x != %x\n",
		     xor, byte);
		return -EPROTO;
	}
	/* OFFSET != 0 not supported */
	if (start_address != 0U) {
		return -EIO;
	}

	uart_write_8(ACK_BYTE);

	/* Get number of bytes to send */
	if (uart_read_8(&byte) != 0) {
		return -EIO;
	}

	xor = byte;

	/* Send Size + 1 */
	size = byte++;

	/* Checksum */
	if (uart_read_8(&byte) != 0) {
		return -EIO;
	}

	if ((xor ^ byte) != 0xFF) {
		WARN("UART: Start cmd: length checksum: %x != %x\n", xor, byte);
		return -EPROTO;
	}

	uart_write_8(ACK_BYTE);

	if (partid != PHASE_SSP) {
		WARN("Not supported\n");
		return -EPROTO;
	}

	length = handle.cert_len;
	buffer = (uint8_t *)handle.cert_base;

	for (i = 0U; i < length; i++, buffer++) {
		uart_write_8(*buffer);
	}
	for (; i < size; i++) {
		uart_write_8(0x0);
	}

	return 0;
}
#endif /* STM32MP_SSP */

static int uart_read(unsigned int image_id, uint8_t id, uintptr_t buffer, size_t length)
{
	bool start_done = false;
	int ret;
	uint8_t command = 0U;

	handle.phase = id;
	handle.packet = 0U;
	handle.addr = (uint8_t *)buffer;
	handle.len = length;

	INFO("UART: read phase %i at 0x%lx size 0x%x\n",
	     id, buffer, length);
	while (!start_done) {

		stm32_iwdg_refresh();

		ret = uart_receive_command(&command);
		if (ret != 0U) {
			/* delay to wait STM32CubeProgrammer end of transmission */
			mdelay(2);

			ret = uart_flush_and_nack();
			if (ret != 0U) {
				return ret;
			}

			continue;
		}

		uart_write_8(ACK_BYTE);

		switch (command) {
		case INIT_BYTE:
			INFO("UART: Connected\n");
			/* Nothing to do */
			continue;

		case GET_CMD_COMMAND:
			ret = get_cmd_command();
			break;

		case GET_VER_COMMAND:
			ret = get_version_command();
			break;

		case GET_ID_COMMAND:
			ret = get_id_command();
			break;

		case PHASE_COMMAND:
			ret = uart_send_phase((uint32_t)buffer);
			if ((ret == 0U) && (handle.phase == PHASE_RESET)) {
				start_done = true;
				INFO("UART: Reset\n");
			}
			break;

		case DOWNLOAD_COMMAND:
			ret = uart_download_part();
			break;
#if STM32MP_SSP
		case READ_PART_COMMAND:
			ret = uart_read_part();
			break;
#endif
		case START_COMMAND:
			ret = uart_start_cmd(image_id, buffer);
			if ((ret == 0U) && (handle.phase == id)) {
				INFO("UART: Start phase %d\n", handle.phase);
#if STM32MP_SSP
				if (handle.phase == PHASE_SSP) {
					handle.phase = PHASE_RESET;
					break;
				}
#endif
				start_done = true;
			}
			break;

		default:
			/* Not supported command  */
			WARN("UART: Unknown command\n");
			ret = -EINVAL;
			break;
		}

		if (ret == 0U) {
			ret = uart_write_8(ACK_BYTE);
		} else {
			ret = uart_flush_and_nack();
		}

		if (ret != 0U) {
			return ret;
		}
	}

	return 0;
}

/* Init UART: 115200, 8bit 1stop parity even and enable FIFO mode */
const struct stm32_uart_init_s init = {
	.baud_rate = U(115200),
	.word_length = STM32_UART_WORDLENGTH_9B,
	.stop_bits = STM32_UART_STOPBITS_1,
	.parity = STM32_UART_PARITY_EVEN,
	.hw_flow_control = STM32_UART_HWCONTROL_NONE,
	.mode = STM32_UART_MODE_TX_RX,
	.over_sampling = STM32_UART_OVERSAMPLING_16,
	.fifo_mode = STM32_UART_FIFOMODE_EN,
};

#if STM32MP_SSP
int stm32cubeprog_uart_ssp(uintptr_t instance,
			   uintptr_t cert_base,
			   size_t cert_len,
			   uintptr_t ssp_base,
			   size_t ssp_len)
{
	int ret;

	if (stm32_uart_init(&handle.uart, instance, &init) != 0U) {
		return -EIO;
	}

	/* NACK to synchronize STM32CubeProgrammer */
	ret = uart_flush_and_nack();
	if (ret != 0) {
		return ret;
	}

	if (cert_base == UNDEFINED_DOWN_ADDR) {
		/* Send Provisioning message to programmer for reboot */
		STM32PROG_ERROR("Provisioning\n");
	} else {
		handle.cert_base = cert_base;
		handle.cert_len  = cert_len;
		handle.phase = PHASE_SSP;
	}

	return uart_read(MAX_IMAGE_IDS, handle.phase, ssp_base, ssp_len);

}
#endif

int stm32cubeprog_uart_load(unsigned int image_id,
			    uintptr_t instance,
			    uintptr_t flashlayout_base,
			    size_t flashlayout_len,
			    uintptr_t ssbl_base,
			    size_t ssbl_len)
{
	int ret;

	if (stm32_uart_init(&handle.uart, instance, &init) != 0U) {
		return -EIO;
	}

	/*
	 * The following NACK_BYTE is written because STM32CubeProgrammer has
	 * already sent its command before TF-A has reached this point, and
	 * because FIFO was not configured by BootROM.
	 * The byte in the UART_RX register is then the checksum and not the
	 * command. NACK_BYTE has to be written, so that the programmer will
	 * re-send the good command.
	 */
	ret = uart_flush_and_nack();
	if (ret != 0) {
		return ret;
	}

	/* Get FlashLayout with PhaseId=0 */
	if (flashlayout_len > 0U) {
		ret = uart_read(STM32_IMAGE_ID, PHASE_FLASHLAYOUT,
				flashlayout_base, flashlayout_len);
		if (ret != 0U) {
			return ret;
		}
	}

	ret = uart_read(image_id, PHASE_SSBL, ssbl_base, ssbl_len);

	return ret;
}
