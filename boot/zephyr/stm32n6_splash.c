/*
 * Boot splash for the STM32N6570-DK: light the panel and draw a centered
 * "B" while MCUBoot loads and verifies the application, so the user is
 * not staring at a dark screen. The BootROM phase (~2 s) stays dark —
 * nothing runs yet — this covers the MCUBoot + app-init window until the
 * application presents its first frame.
 *
 * Draws directly into the same 800x480 ARGB8888 scan buffer the
 * application uses (AXISRAM3..6 @ 0x34200000) and points the zero-copy
 * LTDC driver (CONFIG_STM32_LTDC_FB_NUM=0) at it, so the handover to the
 * application is seamless: the splash stays visible until the app's
 * first rotate-present overwrites the very same memory.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/cache.h>

#include <stm32n6xx_hal.h>

#include <stdbool.h>
#include <string.h>

#include "dfu_text.h"   /* pre-rendered Inter strings (8-bit alpha bitmaps) */

static void draw_glyph(const uint8_t *glyph, int x0, int y0);
static void present(const struct device *disp);

#define PORTRAIT_W 480
#define PORTRAIT_H 800
#define SCAN_W     800
#define SCAN_H     480

#define SPLASH_BG  0xFF101018u   /* app UI background */
#define SPLASH_FG  0xFFF4F4F8u

static uint32_t *const scan_buf = (uint32_t *)0x34200000UL;

/* 5x7 block "B" for the boot splash (the DFU recovery screen uses the
 * pre-rendered Inter strings from dfu_text.h instead). */
static const uint8_t glyph_b[7] = {
	0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E,
};

#define GLYPH_SCALE 16
#define GLYPH_W (5 * GLYPH_SCALE)
#define GLYPH_H (7 * GLYPH_SCALE)

/* portrait (px,py) -> scan-buffer index; the same 90-degree CCW mapping
 * the application's cpu_rotate_present() uses.
 */
static inline uint32_t scan_index(int px, int py)
{
	return (uint32_t)((PORTRAIT_W - 1 - px) * SCAN_W + py);
}

void stm32n6_splash_show(void)
{
	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(disp)) {
		return;
	}

	/* AXISRAM3..6 power up in shutdown: enable memory clocks and exit
	 * shutdown before the banks hold data (mirrors the application's
	 * APP_InitDrawing; the sleep-clock enables matter there, not here).
	 */
	__HAL_RCC_AXISRAM3_MEM_CLK_ENABLE();
	__HAL_RCC_AXISRAM4_MEM_CLK_ENABLE();
	__HAL_RCC_AXISRAM5_MEM_CLK_ENABLE();
	__HAL_RCC_AXISRAM6_MEM_CLK_ENABLE();
	__HAL_RCC_RAMCFG_CLK_ENABLE();
	CLEAR_BIT(RAMCFG_SRAM3_AXI->CR, RAMCFG_CR_SRAMSD);
	CLEAR_BIT(RAMCFG_SRAM4_AXI->CR, RAMCFG_CR_SRAMSD);
	CLEAR_BIT(RAMCFG_SRAM5_AXI->CR, RAMCFG_CR_SRAMSD);
	CLEAR_BIT(RAMCFG_SRAM6_AXI->CR, RAMCFG_CR_SRAMSD);

	for (uint32_t i = 0; i < SCAN_W * SCAN_H; i++) {
		scan_buf[i] = SPLASH_BG;
	}

	const int x0 = (PORTRAIT_W - GLYPH_W) / 2;
	const int y0 = (PORTRAIT_H - GLYPH_H) / 2;

	draw_glyph(glyph_b, x0, y0);
	present(disp);
}

static void draw_glyph(const uint8_t *glyph, int x0, int y0)
{
	for (int gy = 0; gy < GLYPH_H; gy++) {
		uint8_t row = glyph[gy / GLYPH_SCALE];

		for (int gx = 0; gx < GLYPH_W; gx++) {
			if (row & (0x10 >> (gx / GLYPH_SCALE))) {
				scan_buf[scan_index(x0 + gx, y0 + gy)] =
					SPLASH_FG;
			}
		}
	}
}

static void present(const struct device *disp)
{
	/* CPU writes go through the cache; the LTDC reads memory. Must be
	 * the Zephyr sys_cache API: CMSIS SCB_CleanDCache_by_Addr() is a
	 * silent no-op on this SoC (__DCACHE_PRESENT undefined through
	 * Zephyr's cmsis_core.h — the trap documented in the app's
	 * platform_cache.c; proven again 2026-07-05 by RAM readbacks in
	 * recovery). DO NOT touch CACHEAXI here either: its registers are
	 * in the unpowered NPU domain and any access hangs the FSBL.
	 */
	sys_cache_data_flush_range((void *)scan_buf, SCAN_W * SCAN_H * 4);

	struct display_buffer_descriptor desc = {
		.buf_size = SCAN_W * SCAN_H * 4,
		.width = SCAN_W,
		.height = SCAN_H,
		.pitch = SCAN_W,
	};

	(void)display_write(disp, 0, 0, &desc, scan_buf);
	(void)display_blanking_off(disp);
}

/* Software-requested DFU: the application writes this magic to
 * TAMP->BKP30R and reboots (see the app's dfu_request.c); main()
 * checks it and enters serial recovery. One-shot: always cleared.
 */
#define DFU_REQUEST_MAGIC 0xDF11DF11U

bool stm32n6_dfu_requested(void)
{
	bool req;

	__HAL_RCC_RTCAPB_CLK_ENABLE();
	SET_BIT(PWR->DBPCR, PWR_DBPCR_DBP);
	req = (TAMP->BKP30R == DFU_REQUEST_MAGIC);
	TAMP->BKP30R = 0U;
	return req;
}

#if defined(CONFIG_MCUBOOT_ACTION_HOOKS)
#include <bootutil/mcuboot_status.h>

/* Alpha-composite an 8-bit coverage bitmap (Inter glyph run from
 * dfu_text.h) over the scan buffer, in portrait coords. */
static void blit_alpha(const DfuImg *im, int x0, int y0, uint32_t fg)
{
	int fr = (fg >> 16) & 0xFF, fgc = (fg >> 8) & 0xFF, fb = fg & 0xFF;

	for (int gy = 0; gy < im->h; gy++) {
		int py = y0 + gy;

		if (py < 0 || py >= PORTRAIT_H) {
			continue;
		}
		for (int gx = 0; gx < im->w; gx++) {
			int a = im->a[gy * im->w + gx];
			int px = x0 + gx;

			if (a == 0 || px < 0 || px >= PORTRAIT_W) {
				continue;
			}

			uint32_t idx = scan_index(px, py);
			uint32_t d = scan_buf[idx];
			int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF,
			    db = d & 0xFF;
			int r = dr + (fr - dr) * a / 255;
			int g = dg + (fgc - dg) * a / 255;
			int b = db + (fb - db) * a / 255;

			scan_buf[idx] = 0xFF000000u | (uint32_t)(r << 16) |
					(uint32_t)(g << 8) | (uint32_t)b;
		}
	}
}

/* Exit DFU mode with the USER button (sw0 / PC13 — deliberately NOT the
 * touch panel: GT911/I2C in the FSBL puts the LCD module in a dead state,
 * see the DO-NOT note in stm32n657xx_fsbl.conf). 100 ms poll; triggers on
 * press-then-RELEASE so the finger is off the button before reboot —
 * a still-held button at the next boot would re-enter recovery (it is
 * also the recovery entrance pin). The TAMP DFU magic is one-shot and
 * already consumed, so the reboot lands in the application.
 *
 * The button is ALSO the recovery entrance pin, so on button-entry it is
 * still held when this poll starts. Arm the press-then-release detector
 * only after the button has first been seen released — otherwise the
 * entry hold itself counted as the press and letting go of USER exited
 * recovery immediately (the "must keep holding through the whole upload"
 * bug, fixed 2026-07-06).
 */
static const struct gpio_dt_spec exit_btn =
	GPIO_DT_SPEC_GET(DT_ALIAS(mcuboot_button0), gpios);
static struct k_work_delayable s_btn_poll;
static bool s_btn_armed;
static bool s_btn_was_down;

static void btn_poll_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int v = gpio_pin_get_dt(&exit_btn);

	if (v > 0) {
		if (s_btn_armed) {
			s_btn_was_down = true;
		}
	} else {
		if (s_btn_was_down) {
			sys_reboot(SYS_REBOOT_COLD);
		}
		s_btn_armed = true;
	}
	k_work_schedule(&s_btn_poll, K_MSEC(100));
}

/* Waiting in recovery: Inter-rendered status screen (idle recovery does
 * no flash writes, so drawing causes no LTDC underruns) + start the
 * exit-button poll.
 */
void mcuboot_status_change(mcuboot_status_type_t status)
{
	if (status != MCUBOOT_STATUS_SERIAL_DFU_ENTERED) {
		return;
	}

	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	k_work_init_delayable(&s_btn_poll, btn_poll_fn);

	if (!device_is_ready(disp)) {
		return;
	}

	for (uint32_t i = 0; i < SCAN_W * SCAN_H; i++) {
		scan_buf[i] = SPLASH_BG;
	}

	blit_alpha(&dfu_title, (PORTRAIT_W - dfu_title.w) / 2, 330,
		   SPLASH_FG);
	blit_alpha(&dfu_subtitle, (PORTRAIT_W - dfu_subtitle.w) / 2, 386,
		   0xFF9A9AA4u);
	blit_alpha(&dfu_hint, (PORTRAIT_W - dfu_hint.w) / 2, 700,
		   0xFF6A6A74u);

	present(disp);

	if (gpio_is_ready_dt(&exit_btn) &&
	    gpio_pin_configure_dt(&exit_btn, GPIO_INPUT) == 0) {
		s_btn_armed = false;
		s_btn_was_down = false;
		k_work_schedule(&s_btn_poll, K_MSEC(100));
	}
}

/* First upload chunk: stop the exit poll (no reboots mid-flash) and
 * blank the panel for the transfer - flash-write bus traffic starves the
 * LTDC's framebuffer reads and the image would flicker (underruns). The
 * post-update reboot brings the splash back.
 */
void mcuboot_serial_upload_started(void)
{
	(void)k_work_cancel_delayable(&s_btn_poll);

	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (device_is_ready(disp)) {
		(void)display_blanking_on(disp);
	}
}
#endif /* CONFIG_MCUBOOT_ACTION_HOOKS */
