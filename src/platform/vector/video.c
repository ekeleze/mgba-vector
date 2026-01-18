//
// Created by ekeleze on 1/18/26.
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
#include <arm_neon.h>

#include <mgba/core/core.h>

#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>

#define GPIO_LCD_WRX 110
#define GPIO_LCD_RESET_MIDAS 96
#define GPIO_LCD_RESET_SANTEK 55
#define RSHIFT 0x1C
#define XSHIFT 0x0
#define YSHIFT 0x18

#define GBA_WIDTH 240
#define GBA_HEIGHT 160

#define FRAME_PIXELS (160 * 80)

static pthread_t video_thread;
static atomic_bool video_running = true;
static atomic_bool frame_pending = false;

static const uint32_t* pending_src;
static size_t pending_stride;

static pthread_t video_thread;

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

	gpio_write(GPIO_LCD_WRX, is_cmd ? 0 : 1);

	const uint8_t* ptr = data;

	while (bytes) {

		int chunk = bytes > MAX_TRANSFER ? MAX_TRANSFER : bytes;

		ssize_t ret = write(lcd_fd, ptr, chunk);

		if (ret <= 0) break;

		ptr += ret;
		bytes -= ret;
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

void zero_buffer(void* ptr, size_t size) {
	uint8_t* p = (uint8_t*)ptr;
	size_t i = 0;

	uint8x16_t zero = vdupq_n_u8(0);

	// 64 bytes at a time
	for (; i + 64 <= size; i += 64) {
		vst1q_u8(p + i, zero);
		vst1q_u8(p + i + 16, zero);
		vst1q_u8(p + i + 32, zero);
		vst1q_u8(p + i + 48, zero);
	}

	// Cleanup
	memset(p + i, 0, size - i);
}

static uint16x4_t convert_color(uint32x4_t pixels) {
    uint32x4_t b = vshrq_n_u32(pixels, 19);
    uint32x4_t g = vshrq_n_u32(pixels, 10);
    uint32x4_t r = vshrq_n_u32(pixels, 3);

    r = vandq_u32(r, vdupq_n_u32(0x1F));
    g = vandq_u32(g, vdupq_n_u32(0x3F));
    b = vandq_u32(b, vdupq_n_u32(0x1F));

    uint32x4_t result32 = vorrq_u32(vshlq_n_u32(r, 11),
                          vorrq_u32(vshlq_n_u32(g, 5), b));

    return vmovn_u32(result32);
}

void render_crop(const uint32_t* src, uint16_t* dst) {
    int offset_x = (GBA_WIDTH - screen_width) / 2;
    int offset_y = (GBA_HEIGHT - screen_height) / 2;

    for (int y = 0; y < screen_height; y++) {
        const uint32_t* src_row = &src[(y + offset_y) * GBA_WIDTH + offset_x];
        uint16_t* dst_row = &dst[y * screen_width];

        int x = 0;
        for (; x <= screen_width - 4; x += 4) {
            uint32x4_t pixels = vld1q_u32(&src_row[x]);
            uint16x4_t result = convert_color(pixels);
            vst1_u16(&dst_row[x], result);
        }

        for (; x < screen_width; x++) {
            uint32_t pixel = src_row[x];
            uint8_t r = (pixel >> 19) & 0x1F;
            uint8_t g = (pixel >> 10) & 0x3F;
            uint8_t b = (pixel >> 3) & 0x1F;
            dst_row[x] = (r << 11) | (g << 5) | b;
        }
    }
}

void render_scaled(const uint32_t* src, uint16_t* dst) {
    float x_ratio = (float)GBA_WIDTH / screen_width;
    float y_ratio = (float)GBA_HEIGHT / screen_height;

    for (int y = 0; y < screen_height; y++) {
        int src_y = (int)(y * y_ratio);
        const uint32_t* src_row = &src[src_y * GBA_WIDTH];
        uint16_t* dst_row = &dst[y * screen_width];

        int x = 0;
        for (; x <= screen_width - 4; x += 4) {
            uint32_t p[4];
            for (int i = 0; i < 4; i++) {
                int src_x = (int)((x + i) * x_ratio);
                p[i] = src_row[src_x];
            }

            uint32x4_t pixels = vld1q_u32(p);
            uint16x4_t result = convert_color(pixels);
            vst1_u16(&dst_row[x], result);
        }

        for (; x < screen_width; x++) {
            int src_x = (int)(x * x_ratio);
            uint32_t pixel = src_row[src_x];
            uint8_t r = (pixel >> 19) & 0x1F;
            uint8_t g = (pixel >> 10) & 0x3F;
            uint8_t b = (pixel >> 3) & 0x1F;
            dst_row[x] = (r << 11) | (g << 5) | b;
        }
    }
}

void render_integer2x(const uint32_t* src, uint16_t* dst) {
	int scaled_width = GBA_WIDTH / 2;   // 120
	int scaled_height = GBA_HEIGHT / 2; // 80
	int offset_x = (screen_width - scaled_width) / 2;

	for (int y = 0; y < scaled_height; y++) {
		int src_y = y * 2;
		const uint32_t* src_row = &src[src_y * GBA_WIDTH];
		uint16_t* dst_row = &dst[y * screen_width + offset_x];

		int x = 0;
		for (; x <= scaled_width - 4; x += 4) {
			uint32_t p[4];
			for (int i = 0; i < 4; i++) {
				p[i] = src_row[(x + i) * 2];
			}

			uint32x4_t pixels = vld1q_u32(p);
			uint16x4_t converted = convert_color(pixels);

			uint16x4_t result = vorr_u16(vshr_n_u16(converted, 8), vshl_n_u16(converted, 8));

			vst1_u16(&dst_row[x], result);
		}

		for (; x < scaled_width; x++) {
			int src_x = x * 2;
			uint32_t pixel = src_row[src_x];
			uint8_t r = (pixel >> 19) & 0x1F;
			uint8_t g = (pixel >> 10) & 0x3F;
			uint8_t b = (pixel >> 3) & 0x1F;
			uint16_t c = (r << 11) | (g << 5) | b;
			dst_row[x] = (c >> 8) | (c << 8);
		}
	}
}

static void* video_thread_func(void* arg) {
	uint16_t video_framebuffer[framebuffer_size];

	static const uint8_t WRITE_RAM = 0x2C;

	while (video_running) {

		if (!atomic_load(&frame_pending)) {
			sched_yield();
			continue;
		}

		atomic_store(&frame_pending, false);

		const uint32_t* src = pending_src;

		if (!v1)
			render_integer2x(src, video_framebuffer);
		else if (isScaled)
			render_scaled(src, video_framebuffer);
		else
			render_crop(src, video_framebuffer);

		lcd_spi_transfer(1, 1, &WRITE_RAM);

		lcd_spi_transfer(0,
			framebuffer_size * sizeof(uint16_t),
			video_framebuffer);
	}

	return NULL;
}

void video_start_thread() {

	pthread_attr_t attr;
	pthread_attr_init(&attr);

	struct sched_param param = { .sched_priority = 60 };

	pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
	pthread_attr_setschedparam(&attr, &param);

	pthread_create(&video_thread, &attr,
					video_thread_func, NULL);

	pthread_attr_destroy(&attr);
}

void video_init(bool scaled) {
	isScaled = scaled;
	v1 = IsVector1();

	if (v1) {
		SANTEK_Init();
	} else {
		MIDAS_Init();
	}

	video_start_thread();
}

void video_stop_thread() {

	video_running = false;

	pthread_join(video_thread, NULL);
}

void video_submit_frame(struct mCore* core) {

	const void* buffer;
	size_t stride;

	core->getPixels(core, &buffer, &stride);

	pending_src = buffer;
	pending_stride = stride;

	atomic_store(&frame_pending, true);
}