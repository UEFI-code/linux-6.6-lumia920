// SPDX-License-Identifier: GPL-2.0

#include <linux/console.h>
#include <linux/font.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/types.h>

#include <asm/early_ioremap.h>

#define LUMIAFB_PHYS		0x80400000
#define LUMIAFB_WIDTH		768
#define LUMIAFB_HEIGHT		1280
#define LUMIAFB_BPP		4
#define LUMIAFB_STRIDE		(LUMIAFB_WIDTH * LUMIAFB_BPP)

#define LUMIAFB_BGRA_BLACK	0xff000000U
#define LUMIAFB_BGRA_WHITE	0xffffffffU

static u8 __iomem *fb_raw = (u8 __iomem *)LUMIAFB_PHYS; // use identity mapping
static u32 __iomem *fb = (u32 __iomem *)LUMIAFB_PHYS;

static const struct font_desc *lumiafb_font = &font_vga_8x16;
static unsigned int lumiafb_x;
static unsigned int lumiafb_y;

static inline void lumiafb_plot(u32 x, u32 y, u32 color)
{
	if (x >= LUMIAFB_WIDTH || y >= LUMIAFB_HEIGHT)
		return;

	fb[y * LUMIAFB_WIDTH + x] = color;
}

static void lumiafb_clear(void)
{
	u32 x;
	u32 y;

	for (y = 0; y < LUMIAFB_HEIGHT; y++)
		for (x = 0; x < LUMIAFB_WIDTH; x++)
			lumiafb_plot(x, y, LUMIAFB_BGRA_BLACK);
}

static void lumiafb_scroll(void)
{
	memmove(fb_raw,
		fb_raw + lumiafb_font->height * LUMIAFB_STRIDE,
		(LUMIAFB_HEIGHT - lumiafb_font->height) * LUMIAFB_STRIDE);

	for (; lumiafb_y < LUMIAFB_HEIGHT; lumiafb_y++) {
		unsigned int x;

		for (x = 0; x < LUMIAFB_WIDTH; x++)
			lumiafb_plot(x, lumiafb_y, LUMIAFB_BGRA_BLACK);
	}

	lumiafb_y = LUMIAFB_HEIGHT - lumiafb_font->height;
}

static void lumiafb_putchar(char c)
{
	const u8 *glyph;
	unsigned int row;

	if (c == '\n') {
		lumiafb_x = 0;
		lumiafb_y += lumiafb_font->height;
		goto check_scroll;
	}

	if (c == '\r') {
		lumiafb_x = 0;
		return;
	}

	if (c == '\t') {
		lumiafb_x += lumiafb_font->width * 4;
		goto check_wrap;
	}

	glyph = lumiafb_font->data + c * lumiafb_font->height;

	for (row = 0; row < lumiafb_font->height; row++) {
		u8 bits = glyph[row];
		unsigned int col;

		for (col = 0; col < lumiafb_font->width; col++) {
			u32 color = bits & (0x80 >> col) ?
				LUMIAFB_BGRA_WHITE : LUMIAFB_BGRA_BLACK;

			lumiafb_plot(lumiafb_x + col,
					 lumiafb_y + row,
					 color);
		}
	}

	lumiafb_x += lumiafb_font->width;

check_wrap:
	if (lumiafb_x + lumiafb_font->width > LUMIAFB_WIDTH) {
		lumiafb_x = 0;
		lumiafb_y += lumiafb_font->height;
	}

check_scroll:
	if (lumiafb_y + lumiafb_font->height > LUMIAFB_HEIGHT)
		lumiafb_scroll();
}

static void lumiafb_write(struct console *con, const char *s,
				  unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		lumiafb_putchar(s[i]);
}

static struct console lumiafb_console = {
	.name = "lumiafb",
	.write = lumiafb_write,
	.flags = CON_BOOT | CON_PRINTBUFFER,
	.index = -1,
};

void __init lumiafb_early_init(void)
{
	lumiafb_clear();
	register_console(&lumiafb_console);
	pr_info("lumiafb: early BGRA console at 0x%08x\n",
		(unsigned int)LUMIAFB_PHYS);
}