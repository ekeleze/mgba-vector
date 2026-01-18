//
// Created by ekeleze on 1/17/26.
//

#include "video.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <mgba/core/core.h>

#define GPIO_LCD_WRX 110
#define GPIO_LCD_RESET_MIDAS 96
#define GPIO_LCD_RESET_SANTEK 55
#define RSHIFT 0x1C
#define XSHIFT 0x0
#define YSHIFT 0x18

#define GBA_WIDTH 240
#define GBA_HEIGHT 160

typedef uint16_t color_t;

static int lcd_fd = -1;
static int MAX_TRANSFER = 0x1000;

static int screen_width = 184;
static int screen_height = 96;
static int framebuffer_size = 184 * 96;

static bool isScaled = false;

static bool v1 = true;

static void gpio_export(int pin) {
	int fd = open("/sys/class/gpio/export", O_WRONLY);
	if (fd < 0) return;
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", pin);
	write(fd, buf, strlen(buf));
	close(fd);
}

static void gpio_set_direction(int pin, const char* dir) {
	char path[64];
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
	int fd = open(path, O_WRONLY);
	if (fd < 0) return;
	write(fd, dir, strlen(dir));
	close(fd);
}

static void gpio_write(int pin, int value) {
	char path[64];
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
	int fd = open(path, O_WRONLY);
	if (fd < 0) return;
	write(fd, value ? "1" : "0", 1);
	close(fd);
}

static void lcd_spi_transfer(int is_cmd, int bytes, const void* data) {
	const uint8_t* tx_buf = data;

	gpio_write(GPIO_LCD_WRX, is_cmd ? 0 : 1);

	while (bytes > 0) {
		size_t count = bytes > MAX_TRANSFER ? MAX_TRANSFER : bytes;
		write(lcd_fd, tx_buf, count);
		bytes -= count;
		tx_buf += count;
	}
}

bool IsVector1() {
	FILE *fp;
	char buffer[128];
	int result;

	fp = popen("emr-cat v", "r");
	if (fp == NULL) {
		perror("popen failed");
		return true;
	}

	if (fgets(buffer, sizeof(buffer), fp) != NULL) {
		if (sscanf(buffer, "%x", &result) == 1) {
			pclose(fp);
			if (result == 0x6) return true;
			if (result == 0x20)
				return false;
			fprintf(stderr, "Unexpected output: %s\n", buffer);
			return true;
		}
	}

	pclose(fp);
	fprintf(stderr, "Failed to read output.\n");
	return true;
}

void SANTEK_Init() {
	printf("Initializing SANTEK LCD (184x96)...\n");

	framebuffer_size = 184 * 96;
	screen_height = 96;
	screen_width = 184;

	gpio_export(GPIO_LCD_WRX);
	gpio_export(GPIO_LCD_RESET_SANTEK);
	gpio_set_direction(GPIO_LCD_WRX, "out");
	gpio_set_direction(GPIO_LCD_RESET_SANTEK, "out");

	gpio_write(GPIO_LCD_RESET_SANTEK, 0);
    usleep(50000);
    gpio_write(GPIO_LCD_RESET_SANTEK, 1);
    usleep(120000);

	lcd_fd = open("/dev/spidev1.0", O_RDWR);
    if (lcd_fd < 0) {
        fprintf(stderr, "Can't open SPI: %d\n", errno);
        exit(1);
    }

	uint8_t mode = 0;
    ioctl(lcd_fd, SPI_IOC_RD_MODE, &mode);

	int bufsiz_fd = open("/sys/module/spidev/parameters/bufsiz", O_RDONLY);
    if (bufsiz_fd >= 0) {
        char buf[32] = {0};
        read(bufsiz_fd, buf, sizeof(buf));
        MAX_TRANSFER = atoi(buf);
        close(bufsiz_fd);
        printf("SPI max transfer: %d bytes\n", MAX_TRANSFER);
    }

	struct {
        uint8_t cmd;
        uint8_t len;
        uint8_t data[16];
        uint32_t delay;
    } init[] = {
        { 0x10, 1, { 0x00 }, 120}, // Sleep in
  		{ 0x2A, 4, { 0x00, RSHIFT, (184 + RSHIFT - 1) >> 8, (184 + RSHIFT - 1) & 0xFF } }, // Column address set
  		{ 0x2B, 4, { 0x00, 0x00, (96 -1) >> 8, (96 -1) & 0xFF } }, // Row address set
  		{ 0x36, 1, { 0x00 }, 0 }, // Memory data access control
  		{ 0x3A, 1, { 0x55 }, 0 }, // Interface pixel format (16 bit/pixel 65k RGB data)
  		{ 0xB0, 2, { 0x00, 0x08 } }, // RAM control (LSB first)
  		{ 0xB2, 5, { 0x0C, 0x0C, 0x00, 0x33, 0x33 }, 0 }, // Porch setting
  		{ 0xB7, 1, { 0x72 }, 0 }, // Gate control (VGH 14.97v, VGL -8.23v)
  		{ 0xBB, 1, { 0x3B }, 0 }, // VCOMS setting (1.575v)
  		{ 0xC0, 1, { 0x2C }, 0 }, // LCM control
  		{ 0xC2, 1, { 0x01 }, 0 }, // VDV and VRH command enable
  		{ 0xC3, 1, { 0x14 }, 0 }, // VRH set
  		{ 0xC4, 1, { 0x20 }, 0 }, // VDV set
  		{ 0xC6, 1, { 0x0F }, 0 }, // Frame rate control in normal mode (60hz)
  		{ 0xD0, 2, { 0xA4, 0xA1 }, 0 }, // Power control 1
  		{ 0xE0, 14, { 0xD0, 0x10, 0x16, 0x0A, 0x0A, 0x26, 0x3C, 0x53, 0x53, 0x18, 0x15, 0x12, 0x36, 0x3C }, 0 }, // Positive voltage gamma control
  		{ 0xE1, 14, { 0xD0, 0x11, 0x19, 0x0A, 0x09, 0x25, 0x3D, 0x35, 0x54, 0x17, 0x15, 0x12, 0x36, 0x3C }, 0 }, // Negative voltage gamma control
  		{ 0xE9, 3, { 0x05, 0x05, 0x01 }, 0 }, // Equalize time control
  		{ 0x21, 1, { 0x00 }, 0 }, // Display inversion on
		{ 0x11, 1, { 0x00 }, 120 }, // Sleep out
  		{ 0x29, 1, { 0x00 }, 120 }, // Display on
  		{ 0 }
    };

	for (int i = 0; init[i].cmd; i++) {
        lcd_spi_transfer(1, 1, &init[i].cmd);
        if (init[i].len) {
            lcd_spi_transfer(0, init[i].len, init[i].data);
        }
        if (init[i].delay) {
            usleep(init[i].delay * 1000);
        }
    }

	printf("SANTEK LCD initialized!\n");
}

void MIDAS_Init() {
	printf("Initializing MIDAS LCD (160x80)...\n");

	framebuffer_size = 160 * 80;
	screen_height = 80;
	screen_width = 160;

    gpio_export(GPIO_LCD_WRX);
    gpio_export(GPIO_LCD_RESET_MIDAS);
    gpio_set_direction(GPIO_LCD_WRX, "out");
    gpio_set_direction(GPIO_LCD_RESET_MIDAS, "out");

    gpio_write(GPIO_LCD_RESET_MIDAS, 0);
    usleep(50000);
    gpio_write(GPIO_LCD_RESET_MIDAS, 1);
    usleep(120000);

    lcd_fd = open("/dev/spidev1.0", O_RDWR);
    if (lcd_fd < 0) {
        fprintf(stderr, "Can't open SPI: %d\n", errno);
        exit(1);
    }

    uint8_t mode = 0;
    ioctl(lcd_fd, SPI_IOC_RD_MODE, &mode);

    int bufsiz_fd = open("/sys/module/spidev/parameters/bufsiz", O_RDONLY);
    if (bufsiz_fd >= 0) {
        char buf[32] = {0};
        read(bufsiz_fd, buf, sizeof(buf));
        MAX_TRANSFER = atoi(buf);
        close(bufsiz_fd);
        printf("SPI max transfer: %d bytes\n", MAX_TRANSFER);
    }

    struct {
        uint8_t cmd;
        uint8_t len;
        uint8_t data[16];
        uint32_t delay;
    } init[] = {
        {0x01, 0, {0}, 150},        // Software reset
        {0x11, 0, {0}, 500},        // Sleep out
        {0x20, 0, {0}, 0},          // Display inversion off
        {0x36, 1, {0xA8}, 0},       // Memory access control
        {0x3A, 1, {0x05}, 0},       // 16-bit RGB565

        {0xE0, 16, {0x07, 0x0e, 0x08, 0x07, 0x10, 0x07, 0x02, 0x07,
                    0x09, 0x0f, 0x25, 0x36, 0x00, 0x08, 0x04, 0x10}, 0},
        {0xE1, 16, {0x0a, 0x0d, 0x08, 0x07, 0x0f, 0x07, 0x02, 0x07,
                    0x09, 0x0f, 0x25, 0x35, 0x00, 0x09, 0x04, 0x10}, 0},

        {0xFC, 1, {128+64}, 0},
        {0x13, 0, {0}, 100},        // Normal display mode
        {0x26, 1, {0x02}, 10},      // Gamma set
        {0x29, 0, {0}, 10},         // Display on

        {0x2A, 4, {(XSHIFT >> 8) & 0xFF, XSHIFT & 0xFF,
                   ((160 + XSHIFT - 1) >> 8) & 0xFF, (160 + XSHIFT - 1) & 0xFF}, 0},
        {0x2B, 4, {(YSHIFT >> 8) & 0xFF, YSHIFT & 0xFF,
                   ((80 + YSHIFT - 1) >> 8) & 0xFF, (80 + YSHIFT - 1) & 0xFF}, 0},

        {0, 0, {0}, 0}
    };

    for (int i = 0; init[i].cmd; i++) {
        lcd_spi_transfer(1, 1, &init[i].cmd);
        if (init[i].len) {
            lcd_spi_transfer(0, init[i].len, init[i].data);
        }
        if (init[i].delay) {
            usleep(init[i].delay * 1000);
        }
    }

    printf("MIDAS LCD initialized!\n");
}

void video_init(bool scaled) {
	isScaled = scaled;
	v1 = IsVector1();

	if (v1) {
		SANTEK_Init();
	} else {
		MIDAS_Init();
	}
}

void render_crop(const color_t* src, color_t* dst) {
	int offset_x = (GBA_WIDTH - screen_width) / 2;
	int offset_y = (GBA_HEIGHT - screen_height) / 2;

	for (int y = 0; y < screen_height; y++) {
		memcpy(&dst[y * screen_width],
			   &src[(y + offset_y) * GBA_WIDTH + offset_x],
			   screen_width * sizeof(color_t));
	}
}

void render_scaled(const color_t* src, color_t* dst) {
	float x_ratio = (float)GBA_WIDTH / screen_width;
	float y_ratio = (float)GBA_HEIGHT / screen_height;

	for (int y = 0; y < screen_height; y++) {
		for (int x = 0; x < screen_width; x++) {
			int src_x = (int)(x * x_ratio);
			int src_y = (int)(y * y_ratio);
			dst[y * screen_width + x] = src[src_y * GBA_WIDTH + src_x];
		}
	}
}

void render_integer2x(const color_t* src, color_t* dst) {
	memset(dst, 0, screen_width * screen_height * sizeof(color_t));

	int scaled_width = GBA_WIDTH / 2;   // 120
	int scaled_height = GBA_HEIGHT / 2; // 80
	int offset_x = (screen_width - scaled_width) / 2;

	for (int y = 0; y < scaled_height; y++) {
		for (int x = 0; x < scaled_width; x++) {
			int src_x = x * 2;
			int src_y = y * 2;

			color_t c = src[src_y * GBA_WIDTH + src_x];

			dst[y * screen_width + x + offset_x] = (c >> 8) | (c << 8);
		}
	}
}

void video_draw(struct mCore* core) {
	color_t vFrame[framebuffer_size];
	memset(vFrame, 0, sizeof(vFrame));

	const void* buffer;
	size_t stride;

	core->getPixels(core, &buffer, &stride);

	const uint32_t* frame32 = buffer;

	color_t frame[GBA_WIDTH * GBA_HEIGHT];

	for (int i = 0; i < GBA_WIDTH * GBA_HEIGHT; i++) {
		uint32_t pixel = frame32[i];
		uint8_t r = (pixel >> 19) & 0x1F;
		uint8_t g = (pixel >> 10) & 0x3F;
		uint8_t b = (pixel >> 3) & 0x1F;
		frame[i] = (r << 11) | (g << 5) | b;
	}

	if (!v1)
		render_integer2x(frame, vFrame);
	else if (isScaled)
		render_scaled(frame, vFrame);
	else
		render_crop(frame, vFrame);

	static const uint8_t WRITE_RAM = 0x2C;
	lcd_spi_transfer(1, 1, &WRITE_RAM);
	lcd_spi_transfer(0, sizeof(vFrame), vFrame);
}