/* opencp module Player
 * copyright (c) '18-'21 Stian Skjelstad <stian.skjelstad@gmail.com>
 *
 * SDL2 graphic driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define _CONSOLE_DRIVER
#include "config.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <SDL.h>
#include "types.h"
#include "boot/console.h"
#include "boot/psetting.h"
#include "cpiface/cpiface.h"
#include "framelock.h"
#include "poutput.h"
#include "poutput-sdl2.h"
#include "poutput-fontengine.h"
#include "poutput-swtext.h"

typedef enum
{
	MODE_320_200 = 0,
	MODE_640_400 =  1,
	MODE_640_480 =  2,
	MODE_1024_768 = 3,
	MODE_1280_1024 =  4,
	MODE_BIGGEST = 5
} mode_gui_t;

typedef struct
{
	const mode_t mode;
	int width;
	int height;
} mode_gui_data_t;

static const mode_gui_data_t mode_gui_data[] =
{
	{MODE_320_200, 320, 200},
	{MODE_640_400, 640, 400},
	{MODE_640_480, 640, 480},
	{MODE_1024_768, 1024, 768},
	{MODE_1280_1024, 1280, 1024},
	{-1, -1, -1}
};

static FontSizeEnum plCurrentFontWanted;

typedef struct
{
	int text_width;
	int text_height;
	mode_gui_t gui_mode;
	FontSizeEnum font;
} mode_tui_data_t;

static const mode_tui_data_t mode_tui_data[] =
{
	{  80,  25, MODE_640_400,   _8x16/*, MODE_640x400*/},
	{  80,  30, MODE_640_480,   _8x16/*, MODE_640x480*/},
	{  80,  50, MODE_640_400,   _8x8/*, MODE_640x400*/},
	{  80,  60, MODE_640_480,   _8x8/*, MODE_640x480*/},
	{ 128,  48, MODE_1024_768,  _8x16/*, MODE_1024x768*/},
	{ 160,  64, MODE_1280_1024, _8x16/*, MODE_1280x1024*/},
	{ 128,  96, MODE_1024_768,  _8x8/*, MODE_1024x768*/},
	{ 160, 128, MODE_1280_1024, _8x8/*, MODE_1280x1024*/},
	{ -1, -1, -1, -1}
};

static SDL_Window *current_window = NULL;
static SDL_Renderer *current_renderer = NULL;
static SDL_Texture *current_texture = NULL;

static int last_text_height;
static int last_text_width;

static void (*set_state)(int fullscreen, int width, int height) = 0;
static int do_fullscreen = 0;
static int plScrRowBytes = 0;
static int ekbhit(void);
static int ___valid_key(uint16_t key);
static void sdl2_gflushpal(void);
static void sdl2_gupdatepal(unsigned char color, unsigned char _red, unsigned char _green, unsigned char _blue);

static void *SDL2ScrTextGUIOverlayAddBGRA(unsigned int x, unsigned int y, unsigned int width, unsigned int height, unsigned int pitch, uint8_t *data_bgra);
static void SDL2ScrTextGUIOverlayRemove(void *handle);

static uint32_t sdl2_palette[256] = {
	0xff000000,
	0xff0000aa,
	0xff00aa00,
	0xff00aaaa,
	0xffaa0000,
	0xffaa00aa,
	0xffaa5500,
	0xffaaaaaa,
	0xff555555,
	0xff5555ff,
	0xff55ff55,
	0xff55ffff,
	0xffff5555,
	0xffff55ff,
	0xffffff55,
	0xffffffff,
};

static uint8_t *virtual_framebuffer = 0;

static void sdl2_close_window(void)
{
	if (current_texture)
	{
		SDL_DestroyTexture (current_texture);
		current_texture = 0;
	}
	if (current_renderer)
	{
		SDL_DestroyRenderer (current_renderer);
		current_renderer = 0;
	}
	if (current_window)
	{
		SDL_DestroyWindow (current_window);
		current_window = 0;
	}
}

static void sdl2_dump_renderer (void)
{
#ifdef SDL2_DEBUG
	SDL_RendererInfo info;
	if (!SDL_GetRendererInfo (current_renderer, &info))
	{
		int i;
		fprintf (stderr, "[SDL2-video] Renderer.name = \"%s\"\n", info.name);
		fprintf (stderr, "             Renderer.flags = %d%s%s%s%s\n", info.flags,
				(info.flags&SDL_RENDERER_SOFTWARE)?" SDL_RENDERER_SOFTWARE":"",
				(info.flags&SDL_RENDERER_ACCELERATED)?" SDL_RENDERER_ACCELERATED":"",
				(info.flags&SDL_RENDERER_PRESENTVSYNC)?" SDL_RENDERER_PRESENTVSYNC":"",
				(info.flags&SDL_RENDERER_TARGETTEXTURE)?" SDL_RENDERER_TARGETTEXTURE":"");
		for (i=0; (i<16) && info.texture_formats[i]; i++)
		{
			int bitmap = 0;
			int packed = 0;
			int array = 0;
			fprintf (stderr, "             Renderer.texture_formats[%d] = %d", i, info.texture_formats[i]);

			if (info.texture_formats[i] == SDL_PIXELFORMAT_UNKNOWN) fprintf (stderr, " (SDL_PIXELFORMAT_UNKNOWN)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_INDEX1LSB) fprintf (stderr, " (SDL_PIXELFORMAT_INDEX1LSB)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_INDEX1MSB) fprintf (stderr, " (SDL_PIXELFORMAT_INDEX1MSB)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_INDEX4LSB) fprintf (stderr, " (SDL_PIXELFORMAT_INDEX4LSB)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_INDEX4MSB) fprintf (stderr, " (SDL_PIXELFORMAT_INDEX4MSB)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_INDEX8) fprintf (stderr, " (SDL_PIXELFORMAT_INDEX8)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_RGB332) fprintf (stderr, " (SDL_PIXELFORMAT_RGB332)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_RGB444) fprintf (stderr, " (SDL_PIXELFORMAT_RGB444)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_RGB555) fprintf (stderr, " (SDL_PIXELFORMAT_RGB555)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_BGR555) fprintf (stderr, " (SDL_PIXELFORMAT_BGR555)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_ARGB4444) fprintf (stderr, " (SDL_PIXELFORMAT_ARGB4444)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_RGBA4444) fprintf (stderr, " (SDL_PIXELFORMAT_RGBA4444)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_ABGR4444) fprintf (stderr, " (SDL_PIXELFORMAT_ABGR4444)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_BGRA4444) fprintf (stderr, " (SDL_PIXELFORMAT_BGRA4444)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_ARGB1555) fprintf (stderr, " (SDL_PIXELFORMAT_ARGB1555)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_RGBA5551) fprintf (stderr, " (SDL_PIXELFORMAT_RGBA5551)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_ABGR1555) fprintf (stderr, " (SDL_PIXELFORMAT_ABGR1555)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_BGRA5551) fprintf (stderr, " (SDL_PIXELFORMAT_BGRA5551)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_RGB565) fprintf (stderr, " (SDL_PIXELFORMAT_RGB565)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_BGR565) fprintf (stderr, " (SDL_PIXELFORMAT_BGR565)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_RGB24) fprintf (stderr, " (SDL_PIXELFORMAT_RGB24)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_BGR24) fprintf (stderr, " (SDL_PIXELFORMAT_BGR24)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_RGB888) fprintf (stderr, " (SDL_PIXELFORMAT_RGB888)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_RGBX8888) fprintf (stderr, " (SDL_PIXELFORMAT_RGBX8888)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_BGR888) fprintf (stderr, " (SDL_PIXELFORMAT_BGR888)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_BGRX8888) fprintf (stderr, " (SDL_PIXELFORMAT_BGRX8888)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_ARGB8888) fprintf (stderr, " (SDL_PIXELFORMAT_ARGB8888)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_RGBA8888) fprintf (stderr, " (SDL_PIXELFORMAT_RGBA8888)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_ABGR8888) fprintf (stderr, " (SDL_PIXELFORMAT_ABGR8888)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_BGRA8888) fprintf (stderr, " (SDL_PIXELFORMAT_BGRA8888)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_ARGB2101010) fprintf (stderr, " (SDL_PIXELFORMAT_ARGB2101010)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_RGBA32) fprintf (stderr, " ((SDL_PIXELFORMAT_RGBA32))");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_ARGB32) fprintf (stderr, " ((SDL_PIXELFORMAT_ARGB32))");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_BGRA32) fprintf (stderr, " ((SDL_PIXELFORMAT_BGRA32))");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_ABGR32) fprintf (stderr, " ((SDL_PIXELFORMAT_ABGR32))");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_YV12) fprintf (stderr, " (SDL_PIXELFORMAT_YV12)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_IYUV) fprintf (stderr, " (SDL_PIXELFORMAT_IYUV)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_YUY2) fprintf (stderr, " (SDL_PIXELFORMAT_YUY2)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_UYVY) fprintf (stderr, " (SDL_PIXELFORMAT_UYVY)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_YVYU) fprintf (stderr, " (SDL_PIXELFORMAT_YVYU)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_NV12) fprintf (stderr, " (SDL_PIXELFORMAT_NV12)");
			if (info.texture_formats[i] == SDL_PIXELFORMAT_NV21) fprintf (stderr, " (SDL_PIXELFORMAT_NV21)");

			switch (SDL_PIXELTYPE (info.texture_formats[i]))
			{
				default:
				case SDL_PIXELTYPE_UNKNOWN: fprintf (stderr, " SDL_PIXELTYPE_UNKNOWN"); break;
				case SDL_PIXELTYPE_INDEX1: fprintf (stderr, " SDL_PIXELTYPE_INDEX1"); bitmap = 1; break;
				case SDL_PIXELTYPE_INDEX4: fprintf (stderr, " SDL_PIXELTYPE_INDEX4"); bitmap = 1; break;
				case SDL_PIXELTYPE_INDEX8: fprintf (stderr, " SDL_PIXELTYPE_INDEX8"); bitmap = 1; break;
				case SDL_PIXELTYPE_PACKED8: fprintf (stderr, " SDL_PIXELTYPE_PACKED8"); packed = 1; break;
				case SDL_PIXELTYPE_PACKED16: fprintf (stderr, " SDL_PIXELTYPE_PACKED16"); packed = 1; break;
				case SDL_PIXELTYPE_PACKED32: fprintf (stderr, " SDL_PIXELTYPE_PACKED32"); packed = 1; break;
				case SDL_PIXELTYPE_ARRAYU8: fprintf (stderr, " SDL_PIXELTYPE_ARRAYU8"); array = 1; break;
				case SDL_PIXELTYPE_ARRAYU16: fprintf (stderr, " SDL_PIXELTYPE_ARRAYU16"); array = 1; break;
				case SDL_PIXELTYPE_ARRAYF16: fprintf (stderr, " SDL_PIXELTYPE_ARRAYF16"); array = 1; break;
				case SDL_PIXELTYPE_ARRAYF32: fprintf (stderr, " SDL_PIXELTYPE_ARRAYF32"); array = 1; break;
			}
			if (bitmap)
			{
				switch (SDL_PIXELORDER (info.texture_formats[i]))
				{
					default:
					case SDL_BITMAPORDER_NONE: fprintf (stderr, " SDL_BITMAPORDER_NONE"); break;
					case SDL_BITMAPORDER_4321: fprintf (stderr, " SDL_BITMAPORDER_4321"); break;
					case SDL_BITMAPORDER_1234: fprintf (stderr, " SDL_BITMAPORDER_1234"); break;
				}
			}
			if (packed)
			{
				switch (SDL_PIXELORDER (info.texture_formats[i]))
				{
					default:
					case SDL_PACKEDORDER_NONE: fprintf (stderr, " SDL_PACKEDORDER_NONE"); break;
					case SDL_PACKEDORDER_XRGB: fprintf (stderr, " SDL_PACKEDORDER_XRGB"); break;
					case SDL_PACKEDORDER_RGBX: fprintf (stderr, " SDL_PACKEDORDER_RGBX"); break;
					case SDL_PACKEDORDER_ARGB: fprintf (stderr, " SDL_PACKEDORDER_ARGB"); break;
					case SDL_PACKEDORDER_RGBA: fprintf (stderr, " SDL_PACKEDORDER_RGBA"); break;
					case SDL_PACKEDORDER_XBGR: fprintf (stderr, " SDL_PACKEDORDER_XBGR"); break;
					case SDL_PACKEDORDER_BGRX: fprintf (stderr, " SDL_PACKEDORDER_BGRX"); break;
					case SDL_PACKEDORDER_ABGR: fprintf (stderr, " SDL_PACKEDORDER_ABGR"); break;
					case SDL_PACKEDORDER_BGRA: fprintf (stderr, " SDL_PACKEDORDER_BGRA"); break;
				}
				switch (SDL_PIXELLAYOUT (info.texture_formats[i]))
				{
					default:
					case SDL_PACKEDLAYOUT_NONE: fprintf (stderr, " SDL_PACKEDLAYOUT_NONE"); break;
					case SDL_PACKEDLAYOUT_332: fprintf (stderr, " SDL_PACKEDLAYOUT_332"); break;
					case SDL_PACKEDLAYOUT_4444: fprintf (stderr, " SDL_PACKEDLAYOUT_4444"); break;
					case SDL_PACKEDLAYOUT_1555: fprintf (stderr, " SDL_PACKEDLAYOUT_1555"); break;
					case SDL_PACKEDLAYOUT_5551: fprintf (stderr, " SDL_PACKEDLAYOUT_5551"); break;
					case SDL_PACKEDLAYOUT_565: fprintf (stderr, " SDL_PACKEDLAYOUT_565"); break;
					case SDL_PACKEDLAYOUT_8888: fprintf (stderr, " SDL_PACKEDLAYOUT_8888"); break;
					case SDL_PACKEDLAYOUT_2101010: fprintf (stderr, " SDL_PACKEDLAYOUT_2101010"); break;
					case SDL_PACKEDLAYOUT_1010102: fprintf (stderr, " SDL_PACKEDLAYOUT_1010102"); break;
				}
			}
			if (array)
			{
				switch (SDL_PIXELORDER (info.texture_formats[i]))
				{
					default:
					case SDL_ARRAYORDER_NONE: fprintf (stderr, " SDL_ARRAYORDER_NONE"); break;
					case SDL_ARRAYORDER_RGB: fprintf (stderr, " SDL_ARRAYORDER_RGB"); break;
					case SDL_ARRAYORDER_RGBA: fprintf (stderr, " SDL_ARRAYORDER_RGBA"); break;
					case SDL_ARRAYORDER_ARGB: fprintf (stderr, " SDL_ARRAYORDER_ARGB"); break;
					case SDL_ARRAYORDER_BGR: fprintf (stderr, " SDL_ARRAYORDER_BGR"); break;
					case SDL_ARRAYORDER_BGRA: fprintf (stderr, " SDL_ARRAYORDER_BGRA"); break;
					case SDL_ARRAYORDER_ABGR: fprintf (stderr, " SDL_ARRAYORDER_ABGR"); break;
				}
			}
			fprintf (stderr, " bits_per_pixel=%d", SDL_BITSPERPIXEL (info.texture_formats[i]));
			fprintf (stderr, " bytes_per_pixel=%d\n", SDL_BYTESPERPIXEL (info.texture_formats[i]));
		}
	}
#endif
}

static void set_state_textmode(int fullscreen, int width, int height)
{
	/* texture WILL for sure resize, so just get rid of it! */
	if (current_texture)
	{
		SDL_DestroyTexture (current_texture);
		current_texture = 0;
	}

	if (virtual_framebuffer)
	{
		free (virtual_framebuffer);
		plVidMem = virtual_framebuffer=0;
	}

	if (fullscreen != do_fullscreen)
	{
		if (fullscreen)
		{
			last_text_width  = plScrLineBytes;
			last_text_height = plScrLines;
		} else {
			width = last_text_width;
			height = last_text_height;
		}
	}

	if ((fullscreen != do_fullscreen) || (!current_window))
	{
		sdl2_close_window();

		do_fullscreen = fullscreen;

		if (fullscreen)
		{
			current_window = SDL_CreateWindow ("Open Cubic Player",
			                                   SDL_WINDOWPOS_UNDEFINED,
			                                   SDL_WINDOWPOS_UNDEFINED,
			                                   0, 0,
		                                           SDL_WINDOW_FULLSCREEN_DESKTOP);
			if (current_window)
			{
				SDL_GetWindowSize (current_window, &width, &height);
			}
		} else {
			if (!width)
			{
#ifdef SDL2_DEBUG
				fprintf (stderr, "[SDL2-video] set_state_textmode() width==0 ???\n");
#endif
				width = 640;
			}
			if (!height)
			{
#ifdef SDL2_DEBUG
				fprintf (stderr, "[SDL2-video] set_state_textmode() height==0 ???\n");
#endif
				height = 480;
			}
			current_window = SDL_CreateWindow ("Open Cubic Player",
			                                   SDL_WINDOWPOS_UNDEFINED,
			                                   SDL_WINDOWPOS_UNDEFINED,
		                                           width, height,
		                                           SDL_WINDOW_RESIZABLE);
		}

		if (!current_window)
		{
			fprintf (stderr, "[SDL2-video]: SDL_CreateWindow: %s (fullscreen=%d %dx%d)\n", SDL_GetError(), fullscreen, width, height);
			SDL_ClearError();
		}
	}

	while ( ((width/FontSizeInfo[plCurrentFont].w) < 80) || ((height/FontSizeInfo[plCurrentFont].h) < 25))
	{
#ifdef SDL2_DEBUG
		fprintf (stderr, "[SDL2-video] find a smaller font, since (%d/%d)=%d < 80   or   (%d/%d)=%d < 25\n",
				width, FontSizeInfo[plCurrentFont].w, width/FontSizeInfo[plCurrentFont].w,
				height, FontSizeInfo[plCurrentFont].h, width/FontSizeInfo[plCurrentFont].h);
#endif
		if (plCurrentFont)
			plCurrentFont--;
		else {
			if (!fullscreen)
			{
				fprintf(stderr, "[SDL2-video] unable to find a small enough font for %d x %d, increasing window size\n", width, height);
				width = FontSizeInfo[plCurrentFont].w * 80;
				height = FontSizeInfo[plCurrentFont].h * 25;
				SDL_SetWindowSize (current_window, width, height);
			} else {
				fprintf(stderr, "[SDL2-video] unable to find a small enough font for %d x %d\n", width, height);
				exit(-1);
			}
		}
	}

	plScrWidth     = width/FontSizeInfo[plCurrentFont].w;
	plScrHeight    = height/FontSizeInfo[plCurrentFont].h;
	plScrLineBytes = width;
	plScrLines     = height;

	plScrRowBytes = plScrWidth*2;

	if (!current_renderer)
	{
		current_renderer = SDL_CreateRenderer (current_window, -1, 0);

		if (!current_renderer)
		{
			fprintf (stderr, "[SD2-video]: SDL_CreateRenderer: %s\n", SDL_GetError());
			SDL_ClearError();
			exit(-1);
		} else {
			sdl2_dump_renderer();
		}
	}

	if (!current_texture)
	{
		current_texture = SDL_CreateTexture (current_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
		if (!current_texture)
		{
			fprintf (stderr, "[SDL2-video]: SDL_CreateTexture: %s\n", SDL_GetError());
			SDL_ClearError();
			current_texture = SDL_CreateTexture (current_renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, width, height);
			if (!current_texture)
			{
				fprintf (stderr, "[SDL2-video]: SDL_CreateTexture: %s\n", SDL_GetError());
				SDL_ClearError();
				exit(-1);
			}
		}
	}

	plVidMem = virtual_framebuffer = calloc (plScrLineBytes, plScrLines);

	sdl2_gflushpal();

	___push_key(VIRT_KEY_RESIZE);
}

static void plSetTextMode(unsigned char x)
{
	set_state = set_state_textmode;

	___setup_key(ekbhit, ekbhit);
	_validkey=___valid_key;

	if ((x==plScrMode) && (current_window))
	{
		memset(virtual_framebuffer, 0, plScrLineBytes * plScrLines);
		return;
	}

	_plSetGraphMode(-1); /* closes the window, so gdb helper below will have a clear screen */

	if (x==255)
	{
		plScrMode=255;
		return; /* gdb helper */
	}

	/* if invalid mode, set it to custom */
	if (x>7)
	{
		x=8;

		set_state_textmode(
			do_fullscreen,
			last_text_width,
			last_text_height);
	} else {
		plCurrentFont = mode_tui_data[x].font;

		set_state_textmode(
			do_fullscreen,
			mode_gui_data[mode_tui_data[x].gui_mode].width,
			mode_gui_data[mode_tui_data[x].gui_mode].height);
	}

	plScrType = plScrMode = x;
}

static int cachemode = -1;

static void set_state_graphmode(int fullscreen, int width, int height)
{
	mode_gui_t mode;

	/* texture WILL for sure resize, so just get rid of it! */
	if (current_texture)
	{
		SDL_DestroyTexture (current_texture);
		current_texture = 0;
	}

	switch (cachemode)
	{
		case 13:
			plScrMode=13;
			mode = MODE_320_200;
			break;
		case 0:
			plScrMode=100;
			mode = MODE_640_480;
			break;
		case 1:
			plScrMode=101;
			mode = MODE_1024_768;
			break;
		default:
			fprintf(stderr, "[SDL2-video] plSetGraphMode helper: invalid graphmode\n");
			exit(-1);
	}
	width = mode_gui_data[mode].width;
	height = mode_gui_data[mode].height;

	if ((fullscreen != do_fullscreen) || (!current_window))
	{
		sdl2_close_window();

		do_fullscreen = fullscreen;

		if (fullscreen)
		{
			current_window = SDL_CreateWindow ("Open Cubic Player",
			                                   SDL_WINDOWPOS_UNDEFINED,
			                                   SDL_WINDOWPOS_UNDEFINED,
		                                           0, 0,
		                                           SDL_WINDOW_FULLSCREEN_DESKTOP);
		} else {
			current_window = SDL_CreateWindow ("Open Cubic Player",
		                                           SDL_WINDOWPOS_UNDEFINED,
		                                           SDL_WINDOWPOS_UNDEFINED,
		                                           width, height,
		                                           0);
		}
	}

	if (!current_renderer)
	{
		current_renderer = SDL_CreateRenderer (current_window, -1, 0);

		if (!current_renderer)
		{
			fprintf (stderr, "[SD2-video]: SDL_CreateRenderer: %s\n", SDL_GetError());
			SDL_ClearError();
			exit(-1);
		} else {
			sdl2_dump_renderer();
		}
	}

	if (!current_texture)
	{
		current_texture = SDL_CreateTexture (current_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
		if (!current_texture)
		{
			fprintf (stderr, "[SDL2-video]: SDL_CreateTexture: %s\n", SDL_GetError());
			SDL_ClearError();
			current_texture = SDL_CreateTexture (current_renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, width, height);
			if (!current_texture)
			{
				fprintf (stderr, "[SDL2-video]: SDL_CreateTexture: %s\n", SDL_GetError());
				SDL_ClearError();
				exit(-1);
			}
		}
	}

	plScrLineBytes=width;
	plScrLines=height;
	plScrWidth=plScrLineBytes/8;
	plScrHeight=plScrLines/16;

	plScrRowBytes=plScrWidth*2;

	sdl2_gflushpal();

	___push_key(VIRT_KEY_RESIZE);
	return;
}

static int __plSetGraphMode(int high)
{
	if (high>=0)
		set_state = set_state_graphmode;

	if ((high==cachemode)&&(high>=0))
		goto quick;
	cachemode=high;

	if (virtual_framebuffer)
	{
		free (virtual_framebuffer);
		plVidMem = virtual_framebuffer = 0;
	}
	sdl2_close_window();

	if (high<0)
	{
		return 0;
	}

	___setup_key(ekbhit, ekbhit);
	_validkey=___valid_key;

	set_state_graphmode(do_fullscreen, 0, 0);

	plVidMem = virtual_framebuffer=malloc(plScrLineBytes * plScrLines);

quick:
	if (virtual_framebuffer)
		memset(virtual_framebuffer, 0, plScrLineBytes * plScrLines);

	sdl2_gflushpal();
	return 0;
}

static void __vga13(void)
{
	_plSetGraphMode(13);
}

static int conRestore(void)
{
	return 0;
}
static void conSave(void)
{
}

static void plDisplaySetupTextMode(void)
{
	while (1)
	{
		uint16_t c;
		memset(virtual_framebuffer, 0, plScrLineBytes * plScrLines);
		make_title("sdl2-driver setup");
		swtext_displaystr_cp437(1, 0, 0x07, "1:  font-size:", 14);
		swtext_displaystr_cp437(1, 15, plCurrentFont == _4x4 ? 0x0f : 0x07, "4x4", 3);
		swtext_displaystr_cp437(1, 19, plCurrentFont == _8x8 ? 0x0f : 0x07, "8x8", 3);
		swtext_displaystr_cp437(1, 23, plCurrentFont == _8x16 ? 0x0f : 0x07, "8x16", 4);
/*
		swtext_displaystr_cp437(2, 0, 0x07, "2:  fullscreen: ", 16);
		swtext_displaystr_cp437(3, 0, 0x07, "3:  resolution in fullscreen:", 29);*/

		swtext_displaystr_cp437(plScrHeight-1, 0, 0x17, "  press the number of the item you wish to change and ESC when done", plScrWidth);

		while (!_ekbhit())
				framelock();
		c=_egetch();

		switch (c)
		{
			case '1':
				/* we can assume that we are in text-mode if we are here */
				plCurrentFontWanted = plCurrentFont = (plCurrentFont+1)%3;
				set_state_textmode(do_fullscreen, plScrLineBytes, plScrLines);
				cfSetProfileInt("x11", "font", plCurrentFont, 10);
				break;
			case 27: return;
		}
	}
}

static const char *plGetDisplayTextModeName(void)
{
	static char mode[48];
	snprintf(mode, sizeof(mode), "res(%dx%d), font(%s)%s", plScrWidth, plScrHeight,
		plCurrentFont == _4x4 ? "4x4"
		: plCurrentFont == _8x8 ? "8x8" : "8x16", do_fullscreen?" fullscreen":"");
	return mode;
}

struct keytranslate_t
{
	SDL_Keycode SDL;
	uint16_t OCP;
};

struct keytranslate_t translate[] =
{
	{SDLK_BACKSPACE,    KEY_BACKSPACE},
	{SDLK_TAB,          KEY_TAB},
	{SDLK_DELETE,       KEY_DELETE}, /* ??? */
	{SDLK_RETURN,       _KEY_ENTER},
	{SDLK_RETURN2,      _KEY_ENTER},
	/*SDLK_PAUSE*/
	{SDLK_ESCAPE,       KEY_ESC},
	{SDLK_SPACE,        ' '},
	{SDLK_EXCLAIM,      '!'},
	{SDLK_QUOTEDBL,     '\"'},
	{SDLK_HASH,         '#'},
	{SDLK_DOLLAR,       '$'},
	{SDLK_AMPERSAND,    '&'},
	{SDLK_QUOTE,        '\''},
	{SDLK_LEFTPAREN,    '('},
	{SDLK_RIGHTPAREN,   ')'},
	{SDLK_ASTERISK,     '*'},
	{SDLK_PLUS,         '+'},
	{SDLK_COMMA,        ','},
	{SDLK_MINUS,        '-'},
	{SDLK_PERIOD,       '.'},
	{SDLK_SLASH,        '/'},
	{SDLK_0,            '0'},
	{SDLK_1,            '1'},
	{SDLK_2,            '2'},
	{SDLK_3,            '3'},
	{SDLK_4,            '4'},
	{SDLK_5,            '5'},
	{SDLK_6,            '6'},
	{SDLK_7,            '7'},
	{SDLK_8,            '8'},
	{SDLK_9,            '9'},
	{SDLK_COLON,        ':'},
	{SDLK_SEMICOLON,    ';'},
	{SDLK_LESS,         '<'},
	{SDLK_EQUALS,       '='},
	{SDLK_GREATER,      '>'},
	{SDLK_QUESTION,     '?'},
	{SDLK_AT,           '@'},
	{65,                'A'},
	{66,                'B'},
	{67,                'C'},
	{68,                'D'},
	{69,                'E'},
	{70,                'F'},
	{71,                'G'},
	{72,                'H'},
	{73,                'I'},
	{74,                'J'},
	{75,                'K'},
	{76,                'L'},
	{77,                'M'},
	{78,                'N'},
	{79,                'O'},
	{80,                'P'},
	{81,                'Q'},
	{82,                'R'},
	{83,                'S'},
	{84,                'T'},
	{85,                'U'},
	{86,                'V'},
	{87,                'W'},
	{88,                'X'},
	{89,                'Y'},
	{90,                'Z'},
	{'|',               '|'},
	{SDLK_LEFTBRACKET,  '['},
	{SDLK_BACKSLASH,    '\\'},
	{SDLK_RIGHTBRACKET, ']'},
	{SDLK_CARET,        '^'},
	{SDLK_UNDERSCORE,   '_'},
	{SDLK_BACKQUOTE,    '`'},
	{SDLK_a,            'a'},
	{SDLK_b,            'b'},
	{SDLK_c,            'c'},
	{SDLK_d,            'd'},
	{SDLK_e,            'e'},
	{SDLK_f,            'f'},
	{SDLK_g,            'g'},
	{SDLK_h,            'h'},
	{SDLK_i,            'i'},
	{SDLK_j,            'j'},
	{SDLK_k,            'k'},
	{SDLK_l,            'l'},
	{SDLK_m,            'm'},
	{SDLK_n,            'n'},
	{SDLK_o,            'o'},
	{SDLK_p,            'p'},
	{SDLK_q,            'q'},
	{SDLK_r,            'r'},
	{SDLK_s,            's'},
	{SDLK_t,            't'},
	{SDLK_u,            'u'},
	{SDLK_v,            'v'},
	{SDLK_w,            'w'},
	{SDLK_x,            'x'},
	{SDLK_y,            'y'},
	{SDLK_z,            'z'},
	{SDLK_KP_0,         '0'},
	{SDLK_KP_1,         '1'},
	{SDLK_KP_2,         '2'},
	{SDLK_KP_3,         '3'},
	{SDLK_KP_4,         '4'},
	{SDLK_KP_5,         '5'},
	{SDLK_KP_6,         '6'},
	{SDLK_KP_7,         '7'},
	{SDLK_KP_8,         '8'},
	{SDLK_KP_9,         '9'},
	{SDLK_KP_PERIOD,    ','},
	{SDLK_KP_DIVIDE,    '/'},
	{SDLK_KP_MULTIPLY,  '*'},
	{SDLK_KP_MINUS,     '-'},
	{SDLK_KP_PLUS,      '+'},
	{SDLK_KP_ENTER,     _KEY_ENTER},
	{SDLK_KP_EQUALS,    '='},
	{SDLK_UP,           KEY_UP},
	{SDLK_DOWN,         KEY_DOWN},
	{SDLK_RIGHT,        KEY_RIGHT},
	{SDLK_LEFT,         KEY_LEFT},
	{SDLK_INSERT,       KEY_INSERT},
	{SDLK_HOME,         KEY_HOME},
	{SDLK_END,          KEY_END},
	{SDLK_PAGEUP,       KEY_PPAGE},
	{SDLK_PAGEDOWN,     KEY_NPAGE},
	{SDLK_F1,           KEY_F(1)},
	{SDLK_F2,           KEY_F(2)},
	{SDLK_F3,           KEY_F(3)},
	{SDLK_F4,           KEY_F(4)},
	{SDLK_F5,           KEY_F(5)},
	{SDLK_F6,           KEY_F(6)},
	{SDLK_F7,           KEY_F(7)},
	{SDLK_F8,           KEY_F(8)},
	{SDLK_F9,           KEY_F(9)},
	{SDLK_F10,          KEY_F(10)},
	{SDLK_F11,          KEY_F(11)},
	{SDLK_F12,          KEY_F(12)},
	/*SDLK_F13*/
	/*SDLK_F14*/
	/*SDLK_F15*/
	{-1,                0xffff},
};

struct keytranslate_t translate_shift[] =
{
	{SDLK_TAB,          KEY_SHIFT_TAB},
	{SDLK_LESS,         '>'},
	{SDLK_COMMA,        '<'},
	{SDLK_MINUS,        '?'},
	{SDLK_PERIOD,       '>'},
	{SDLK_SEMICOLON,    ':'},
	{SDLK_QUOTE,        '\"'},
	{SDLK_LEFTBRACKET,  '{'},
	{SDLK_RIGHTBRACKET, '}'},
	{SDLK_MINUS,        '_'},
	{SDLK_EQUALS,       '+'},
	{SDLK_a,            'A'},
	{SDLK_b,            'B'},
	{SDLK_c,            'C'},
	{SDLK_d,            'D'},
	{SDLK_e,            'E'},
	{SDLK_f,            'F'},
	{SDLK_g,            'G'},
	{SDLK_h,            'H'},
	{SDLK_i,            'I'},
	{SDLK_j,            'J'},
	{SDLK_k,            'K'},
	{SDLK_l,            'L'},
	{SDLK_m,            'M'},
	{SDLK_n,            'N'},
	{SDLK_o,            'O'},
	{SDLK_p,            'P'},
	{SDLK_q,            'Q'},
	{SDLK_r,            'R'},
	{SDLK_s,            'S'},
	{SDLK_t,            'T'},
	{SDLK_u,            'U'},
	{SDLK_v,            'V'},
	{SDLK_w,            'W'},
	{SDLK_x,            'X'},
	{SDLK_y,            'Y'},
	{SDLK_z,            'Z'},
	{-1,                0xffff},
};

struct keytranslate_t translate_ctrl[] =
{
	{SDLK_BACKSPACE,    KEY_CTRL_BS},
	{SDLK_RETURN,       KEY_CTRL_ENTER},
	/*{65,                'A'},
	{66,                'B'},
	{67,                'C'},*/
	{68,                KEY_CTRL_D},
	/*{69,                'E'},
	{70,                'F'},
	{71,                'G'},*/
	{72,                KEY_CTRL_H},
	/*{73,                'I'},*/
	{74,                KEY_CTRL_J},
	{75,                KEY_CTRL_K},
	{76,                KEY_CTRL_L},
	/*{77,                'M'},
	{78,                'N'},
	{79,                'O'},*/
	{80,                KEY_CTRL_P},
	{81,                KEY_CTRL_Q},
	/*{82,                'R'},*/
	{83,                KEY_CTRL_S},
	/*{84,                'T'},
	{85,                'U'},
	{86,                'V'},
	{87,                'W'},
	{88,                'X'},
	{89,                'Y'},*/
	{90,                KEY_CTRL_Z},
	/*{SDLK_a,            'a'},
	{SDLK_b,            'b'},
	{SDLK_c,            'c'},*/
	{SDLK_d,            KEY_CTRL_D},
	/*{SDLK_e,            'e'},
	{SDLK_f,            'f'},
	{SDLK_g,            'g'},*/
	{SDLK_h,            KEY_CTRL_H},
	/*{SDLK_i,            'i'},*/
	{SDLK_j,            KEY_CTRL_J},
	{SDLK_k,            KEY_CTRL_K},
	{SDLK_l,            KEY_CTRL_L},
	/*{SDLK_m,            'm'},
	{SDLK_n,            'n'},
	{SDLK_o,            'o'},*/
	{SDLK_p,            KEY_CTRL_P},
	{SDLK_q,            KEY_CTRL_Q},
	/*{SDLK_r,            'r'},*/
	{SDLK_s,            KEY_CTRL_S},
	/*{SDLK_t,            't'},
	{SDLK_u,            'u'},
	{SDLK_v,            'v'},
	{SDLK_w,            'w'},
	{SDLK_x,            'x'},
	{SDLK_y,            'y'},*/
	{SDLK_z,            KEY_CTRL_Z},
	{SDLK_KP_ENTER,     KEY_CTRL_ENTER},
	{SDLK_UP,           KEY_CTRL_UP},
	{SDLK_DOWN,         KEY_CTRL_DOWN},
	{SDLK_RIGHT,        KEY_CTRL_RIGHT},
	{SDLK_LEFT,         KEY_CTRL_LEFT},
	{SDLK_PAGEUP,       KEY_CTRL_PGUP},
	{SDLK_PAGEDOWN,     KEY_CTRL_PGDN},
	{-1,                0xffff},
};

struct keytranslate_t translate_alt[] =
{
	{65,                KEY_ALT_A},
	{66,                KEY_ALT_B},
	{67,                KEY_ALT_C},
	/*{68,                'D'},*/
	{69,                KEY_ALT_E},
	/*{70,                'F'},*/
	{71,                KEY_ALT_G},
	/*{72,                'H'},*/
	{73,                KEY_ALT_I},
	/*{74,                'J'},*/
	{75,                KEY_ALT_K},
	{76,                KEY_ALT_L},
	{77,                KEY_ALT_M},
	/*{78,                'N'},*/
	{79,                KEY_ALT_O},
	{80,                KEY_ALT_P},
	/*{81,                'Q'},*/
	{82,                KEY_ALT_R},
	{83,                KEY_ALT_S},
	/*{84,                'T'},
	{85,                'U'},
	{86,                'V'},
	{87,                'W'},*/
	{88,                KEY_ALT_X},
	/*{89,                'Y'},*/
	{90,                KEY_ALT_Z},
	{SDLK_a,            KEY_ALT_A},
	{SDLK_b,            KEY_ALT_B},
	{SDLK_c,            KEY_ALT_C},
	/*{SDLK_d,            'd'},*/
	{SDLK_e,            KEY_ALT_E},
	/*{SDLK_f,            'f'},*/
	{SDLK_g,            KEY_ALT_G},
	/*{SDLK_h,            'h'},*/
	{SDLK_i,            KEY_ALT_I},
	/*{SDLK_j,            'j'},*/
	{SDLK_k,            KEY_ALT_K},
	{SDLK_l,            KEY_ALT_L},
	{SDLK_m,            KEY_ALT_M},
	/*{SDLK_n,            'n'},*/
	{SDLK_o,            KEY_ALT_O},
	{SDLK_p,            KEY_ALT_P},
	/*{SDLK_q,            'q'},*/
	{SDLK_r,            KEY_ALT_R},
	{SDLK_s,            KEY_ALT_S},
	/*{SDLK_t,            't'},
	{SDLK_u,            'u'},
	{SDLK_v,            'v'},
	{SDLK_w,            'w'},*/
	{SDLK_x,            KEY_ALT_X},
	/*{SDLK_y,            'y'},*/
	{SDLK_z,            KEY_ALT_Z},
	{-1,                0xffff},
};

static int ___valid_key(uint16_t key)
{
	int index;

	if (key==KEY_ALT_ENTER)
		return 0;

	for (index=0;translate[index].OCP!=0xffff;index++)
		if (translate[index].OCP==key)
			return 1;
	for (index=0;translate_shift[index].OCP!=0xffff;index++)
		if (translate_shift[index].OCP==key)
			return 1;
	for (index=0;translate_ctrl[index].OCP!=0xffff;index++)
		if (translate_ctrl[index].OCP==key)
			return 1;
	for (index=0;translate_alt[index].OCP!=0xffff;index++)
		if (translate_alt[index].OCP==key)
			return 1;

	fprintf(stderr, __FILE__ ": unknown key 0x%04x\n", (int)key);
	return 0;
}

struct SDL2ScrTextGUIOverlay_t
{
	unsigned int x;
	unsigned int y;
	unsigned int width;
	unsigned int height;
	unsigned int pitch;
	uint8_t     *data_bgra;

};

static struct SDL2ScrTextGUIOverlay_t **SDL2ScrTextGUIOverlays;
static int                              SDL2ScrTextGUIOverlays_count;
static int                              SDL2ScrTextGUIOverlays_size;

static void *SDL2ScrTextGUIOverlayAddBGRA(unsigned int x, unsigned int y, unsigned int width, unsigned int height, unsigned int pitch, uint8_t *data_bgra)
{
	struct SDL2ScrTextGUIOverlay_t *e = malloc (sizeof (*e));
	e->x = x;
	e->y = y;
	e->width = width;
	e->height = height;
	e->pitch = pitch;
	e->data_bgra = data_bgra;

	if (SDL2ScrTextGUIOverlays_count == SDL2ScrTextGUIOverlays_size)
	{
		SDL2ScrTextGUIOverlays_size += 10;
		SDL2ScrTextGUIOverlays = realloc (SDL2ScrTextGUIOverlays, sizeof (SDL2ScrTextGUIOverlays[0]) * SDL2ScrTextGUIOverlays_size);
	}
	SDL2ScrTextGUIOverlays[SDL2ScrTextGUIOverlays_count++] = e;

	return e;
}

static void SDL2ScrTextGUIOverlayRemove(void *handle)
{
	int i;
	for (i=0; i < SDL2ScrTextGUIOverlays_count; i++)
	{
		if (SDL2ScrTextGUIOverlays[i] == handle)
		{
			memmove (SDL2ScrTextGUIOverlays + i, SDL2ScrTextGUIOverlays + i + 1, sizeof (SDL2ScrTextGUIOverlays[0]) * (SDL2ScrTextGUIOverlays_count - i - 1));
			SDL2ScrTextGUIOverlays_count--;
			free (handle);
			return;
		}
	}
	fprintf (stderr, "[SDL2] Warning: SDL2ScrTextGUIOverlayRemove, handle %p not found\n", handle);
}

static void RefreshScreenGraph(void)
{
	void *pixels;
	int pitch;

	if (!current_texture)
		return;

	if (!virtual_framebuffer)
		return;

	SDL_LockTexture (current_texture, NULL, &pixels, &pitch);

	{
		uint8_t *src=virtual_framebuffer;
		uint8_t *dst_line = (uint8_t *)/*current_surface->*/pixels;
		int Y=0;
		int i;

		uint32_t *dst;

		while (1)
		{
			int j;

			dst = (uint32_t *)dst_line;
			for (j=0;j<plScrLineBytes;j++)
				*(dst++)=sdl2_palette[*(src++)];
			if ((++Y)>=plScrLines)
				break;
			dst_line += /*current_surface->*/pitch;
		}

		for (i=0; i < SDL2ScrTextGUIOverlays_count; i++)
		{
			int ty, y;
			ty = SDL2ScrTextGUIOverlays[i]->y + SDL2ScrTextGUIOverlays[i]->height;
			y = (SDL2ScrTextGUIOverlays[i]->y < 0) ? 0 : SDL2ScrTextGUIOverlays[i]->y;
			for (; y < ty; y++)
			{
				int tx, x;
				uint8_t *src, *dst;

				if (y >= plScrLines) { break; }

				tx = SDL2ScrTextGUIOverlays[i]->x + SDL2ScrTextGUIOverlays[i]->width;
				x = (SDL2ScrTextGUIOverlays[i]->x < 0) ? 0 : SDL2ScrTextGUIOverlays[i]->x;

				src = SDL2ScrTextGUIOverlays[i]->data_bgra + (((y - SDL2ScrTextGUIOverlays[i]->y) * SDL2ScrTextGUIOverlays[i]->pitch + (x - SDL2ScrTextGUIOverlays[i]->x)) << 2);
				dst = (uint8_t *)pixels + (y * pitch) + (x<<2);

				for (; x < tx; x++)
				{
					if (x >= plScrLineBytes) { break; }

					if (src[3] == 0)
					{
						src+=4;
						dst+=4;
					} else if (src[3] == 255)
					{
						*(dst++) = *(src++);
						*(dst++) = *(src++);
						*(dst++) = *(src++);
						src++;
						dst++;
					} else{
						//uint8_t b = src[0];
						//uint8_t g = src[1];
						//uint8_t r = src[2];
						uint8_t a = src[3];
						uint8_t na = a ^ 0xff;
						*dst = ((*dst * na) >> 8) + ((*src * a) >> 8); dst++; src++; // b
						*dst = ((*dst * na) >> 8) + ((*src * a) >> 8); dst++; src++; // g
						*dst = ((*dst * na) >> 8) + ((*src * a) >> 8); dst++; src++; // r
						dst++;
						src++;
					}
				}
			}
		}
	}

	SDL_UnlockTexture (current_texture);

	SDL_RenderCopy (current_renderer, current_texture, NULL, NULL);
	SDL_RenderPresent (current_renderer);

	if (plCurrentFont == _8x8)
	{
		fontengine_8x8_iterate ();
	} else if (plCurrentFont == _8x16)
	{
		fontengine_8x16_iterate ();
	}
}

void RefreshScreenText(void)
{
	if (!current_texture)
		return;

	if (!virtual_framebuffer)
		return;

	swtext_cursor_inject();

	RefreshScreenGraph();

	swtext_cursor_eject();
}

static int ekbhit(void)
{
	SDL_Event event;

	if (plScrMode<=8)
	{
		RefreshScreenText();
	} else {
		RefreshScreenGraph();
	}

	while(SDL_PollEvent(&event))
	{
		switch (event.type)
		{
			case SDL_WINDOWEVENT:
			{
				switch (event.window.event)
				{
					case SDL_WINDOWEVENT_CLOSE:
					{
#ifdef SDL2_DEBUG
						fprintf(stderr, "[SDL2-video] CLOSE:\n");
#endif
						___push_key(KEY_ESC);
						break;
					}
					case SDL_WINDOWEVENT_SIZE_CHANGED:
					{
#ifdef SDL2_DEBUG
						fprintf(stderr, "[SDL2-video] SIZE_CHANGED:\n");
						fprintf(stderr, "              w: %d\n", event.window.data1);
						fprintf(stderr, "              h: %d\n", event.window.data2);
						fprintf(stderr, "              windowID: %d\n", event.window.windowID);
#endif
						break;
					}

					case SDL_WINDOWEVENT_RESIZED:
					{
#ifdef SDL2_DEBUG
						fprintf(stderr, "[SDL2-video] RESIZED:\n");
						fprintf(stderr, "              w: %d\n", event.window.data1);
						fprintf(stderr, "              h: %d\n", event.window.data2);
						fprintf(stderr, "              windowID: %d\n", event.window.windowID);
#endif
						if (current_window && (SDL_GetWindowID(current_window) == event.window.windowID))
						{
							plCurrentFont = plCurrentFontWanted;
							if (!do_fullscreen && (plScrMode == plScrType))
							{
								last_text_height = event.window.data2;
								last_text_width = event.window.data1;
							}
							set_state(do_fullscreen, event.window.data1, event.window.data2);
							if (!do_fullscreen)
							{
								if (plScrType == plScrMode) /* if we are in text-mode, make it a custom one */
								{
									fprintf (stderr, "CUSTOM MODE!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
									plScrType = plScrMode = 8;
								}
							}
						} else {
#ifdef SDL2_DEBUG
							fprintf (stderr, "[SDL2-video] we ignored that event, it does not belong to our window...\n");
#endif
						}
						break;
					}
				}
				break;
			}
			case SDL_MOUSEBUTTONDOWN:
			{
#ifdef SDL2_DEBUG
				fprintf(stderr, "[SDL2-video] MOUSEBUTTONDOWN\n");
				fprintf(stderr, "              which: %d\n", (int)event.button.which);
				fprintf(stderr, "              button: %d\n", (int)event.button.button);
				fprintf(stderr, "              state: %d\n", (int)event.button.state);
				fprintf(stderr, "              x,y: %d, %d\n", (int)event.button.x, (int)event.button.y);
#endif
				switch (event.button.button)
				{
					case 1:
						___push_key(_KEY_ENTER);
						break;

					case 3:
						set_state(!do_fullscreen, plScrLineBytes, plScrLines);
						break;

					case 4:
						___push_key(KEY_UP);
						break;

					case 5:
						___push_key(KEY_DOWN);
						break;
				}
				break;
			}
			case SDL_KEYUP:
			{
#ifdef SDL2_DEBUG
				fprintf(stderr, "[SDL2-video] KEYUP\n");
				fprintf(stderr, "              keysym.key.keysym.scancode=0x%08x\n", (int)event.key.keysym.scancode);
				fprintf(stderr, "              keysym.mod: 0x%04x%s%s%s%s%s%s%s%s%s%s%s\n", (int)event.key.keysym.mod,
						(event.key.keysym.mod & KMOD_LSHIFT) ? " KMOD_LSHIFT":"",
						(event.key.keysym.mod & KMOD_RSHIFT) ? " KMOD_RSHIFT":"",
						(event.key.keysym.mod & KMOD_LCTRL) ? " KMOD_LCTRL":"",
						(event.key.keysym.mod & KMOD_RCTRL) ? " KMOD_RCTRL":"",
						(event.key.keysym.mod & KMOD_LALT) ? " KMOD_LALT":"",
						(event.key.keysym.mod & KMOD_RALT) ? " KMOD_RALT":"",
						(event.key.keysym.mod & KMOD_LGUI) ? " KMOD_LGUI":"",
						(event.key.keysym.mod & KMOD_RGUI) ? " KMOD_RGUI":"",
						(event.key.keysym.mod & KMOD_NUM) ? " KMOD_NUM":"",
						(event.key.keysym.mod & KMOD_CAPS) ? " KMOD_CAPS":"",
						(event.key.keysym.mod & KMOD_MODE) ? " KMOD_MODE":"");
				fprintf(stderr, "              keysym.sym: 0x%08x ->%s<-\n", (int)event.key.keysym.sym, SDL_GetKeyName(event.key.keysym.sym));
#endif
				break;
			}
			case SDL_KEYDOWN:
			{
				int index;

#ifdef SDL2_DEBUG
				fprintf(stderr, "[SDL2-video] KEYDOWN\n");
				fprintf(stderr, "              keysym.key.keysym.scancode=0x%08x\n", (int)event.key.keysym.scancode);
				fprintf(stderr, "              keysym.mod: 0x%04x%s%s%s%s%s%s%s%s%s%s%s\n", (int)event.key.keysym.mod,
						(event.key.keysym.mod & KMOD_LSHIFT) ? " KMOD_LSHIFT":"",
						(event.key.keysym.mod & KMOD_RSHIFT) ? " KMOD_RSHIFT":"",
						(event.key.keysym.mod & KMOD_LCTRL) ? " KMOD_LCTRL":"",
						(event.key.keysym.mod & KMOD_RCTRL) ? " KMOD_RCTRL":"",
						(event.key.keysym.mod & KMOD_LALT) ? " KMOD_LALT":"",
						(event.key.keysym.mod & KMOD_RALT) ? " KMOD_RALT":"",
						(event.key.keysym.mod & KMOD_LGUI) ? " KMOD_LGUI":"",
						(event.key.keysym.mod & KMOD_RGUI) ? " KMOD_RGUI":"",
						(event.key.keysym.mod & KMOD_NUM) ? " KMOD_NUM":"",
						(event.key.keysym.mod & KMOD_CAPS) ? " KMOD_CAPS":"",
						(event.key.keysym.mod & KMOD_MODE) ? " KMOD_MODE":"");
				fprintf(stderr, "              keysym.sym: 0x%08x ->%s<-\n", (int)event.key.keysym.sym, SDL_GetKeyName(event.key.keysym.sym));
#endif

				if ((event.key.keysym.mod & KMOD_CTRL) && (!(event.key.keysym.mod & ~(KMOD_CTRL|KMOD_SHIFT|KMOD_ALT|KMOD_NUM))))
				{
					for (index=0;translate_ctrl[index].OCP!=0xffff;index++)
						if (translate_ctrl[index].SDL==event.key.keysym.sym)
						{
							___push_key(translate_ctrl[index].OCP);
							break;
						}
					break;
				}

				if ((event.key.keysym.mod & KMOD_SHIFT) && (!(event.key.keysym.mod & ~(KMOD_CTRL|KMOD_SHIFT|KMOD_ALT|KMOD_NUM))))
				{
					for (index=0;translate_shift[index].OCP!=0xffff;index++)
						if (translate_shift[index].SDL==event.key.keysym.sym)
						{
							___push_key(translate_shift[index].OCP);
							break;
						}
					/* no break here */
				}

				if ((event.key.keysym.mod & KMOD_ALT) && (!(event.key.keysym.mod & ~(KMOD_CTRL|KMOD_SHIFT|KMOD_ALT|KMOD_NUM))))
				{
					/* TODO, handle ALT-ENTER */
					if (event.key.keysym.sym==SDLK_RETURN)
					{
						set_state(!do_fullscreen, plScrLineBytes, plScrLines);
					} else {
						for (index=0;translate_alt[index].OCP!=0xffff;index++)
							if (translate_alt[index].SDL==event.key.keysym.sym)
							{
								___push_key(translate_alt[index].OCP);
								break;
							}
					}
					break;
				}

				if (event.key.keysym.mod & (KMOD_CTRL|KMOD_SHIFT|KMOD_ALT))
					break;

				for (index=0;translate[index].OCP!=0xffff;index++)
				{
					if (translate[index].SDL==event.key.keysym.sym)
					{
						___push_key(translate[index].OCP);
						break;
					}
				}
				break;
			}
		}
	}

	return 0;
}

static void sdl2_gflushpal(void)
{
}

static void sdl2_gupdatepal(unsigned char index, unsigned char _red, unsigned char _green, unsigned char _blue)
{
	uint8_t *pal = (uint8_t *)sdl2_palette;
	pal[(index<<2)+3] = 0xff;
	pal[(index<<2)+2] = _red<<2;
	pal[(index<<2)+1] = _green<<2;
	pal[(index<<2)+0] = _blue<<2;
}

static int need_quit = 0;

int sdl2_init(void)
{
	if ( SDL_Init(/*SDL_INIT_AUDIO|*/SDL_INIT_VIDEO) < 0 )
	{
		fprintf(stderr, "[SDL2 video] Unable to init SDL: %s\n", SDL_GetError());
		SDL_ClearError();
		return 1;
	}

	if (fontengine_init())
	{
		SDL_Quit();
		return 1;
	}

	/* we now test-spawn one window, and so we can fallback to other drivers */
	current_window = SDL_CreateWindow ("Open Cubic Player detection",
	                                   SDL_WINDOWPOS_UNDEFINED,
	                                   SDL_WINDOWPOS_UNDEFINED,
                                           320, 200,
                                           0);

	if (!current_window)
	{
		fprintf(stderr, "[SDL2 video] Unable to create window: %s\n", SDL_GetError());
		goto error_out;
	}

	current_renderer = SDL_CreateRenderer (current_window, -1, 0);
	if (!current_renderer)
	{
		fprintf (stderr, "[SD2-video]: Unable to create renderer: %s\n", SDL_GetError());
		goto error_out;
	}

	current_texture = SDL_CreateTexture (current_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 200);
	if (!current_texture)
	{
		fprintf (stderr, "[SDL2-video]: Unable to create texture (will do one more attempt): %s\n", SDL_GetError());
		SDL_ClearError();
		current_texture = SDL_CreateTexture (current_renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, 320, 200);
		if (!current_texture)
		{
			fprintf (stderr, "[SDL2-video]: Unable to create texture: %s\n", SDL_GetError());
			goto error_out;
		}
	}

	sdl2_close_window ();

	SDL_EventState (SDL_WINDOWEVENT, SDL_ENABLE);
	SDL_EventState (SDL_MOUSEBUTTONDOWN, SDL_ENABLE);
	SDL_EventState (SDL_KEYDOWN, SDL_ENABLE);

	/* Fill some default font and window sizes */
	plCurrentFontWanted = plCurrentFont = cfGetProfileInt("x11", "font", _8x16, 10);
	if (plCurrentFont > _FONT_MAX)
	{
		plCurrentFont = _8x16;
	}
	last_text_width  = plScrLineBytes = 640;
	last_text_height = plScrLines     = 480;
	plScrType = plScrMode = 8;

	need_quit = 1;

	_plSetTextMode=plSetTextMode;
	_plSetGraphMode=__plSetGraphMode;
	_gdrawstr=generic_gdrawstr;
	_gdrawchar8=generic_gdrawchar8;
	_gdrawchar8p=generic_gdrawchar8p;
	_gdrawchar8t=generic_gdrawchar8t;
	_gdrawcharp=generic_gdrawcharp;
	_gdrawchar=generic_gdrawchar;
	_gupdatestr=generic_gupdatestr;
	_gupdatepal=sdl2_gupdatepal;
	_gflushpal=sdl2_gflushpal;
	_vga13=__vga13;

	_displayvoid=swtext_displayvoid;
	_displaystrattr=swtext_displaystrattr_cp437;
	_displaystr=swtext_displaystr_cp437;
	_displaystr_utf8=swtext_displaystr_utf8;
	_measurestr_utf8=swtext_measurestr_utf8;

	_drawbar=swtext_drawbar;
	_idrawbar=swtext_idrawbar;

	_setcur=swtext_setcur;
	_setcurshape=swtext_setcurshape;
	_conRestore=conRestore;
	_conSave=conSave;

	_plGetDisplayTextModeName = plGetDisplayTextModeName;
	_plDisplaySetupTextMode = plDisplaySetupTextMode;

	plScrTextGUIOverlay        = 1;
	plScrTextGUIOverlayAddBGRA = SDL2ScrTextGUIOverlayAddBGRA;
	plScrTextGUIOverlayRemove  = SDL2ScrTextGUIOverlayRemove;

	plVidType=vidModern;

	return 0;
error_out:
	SDL_ClearError();
	sdl2_close_window ();
	fontengine_done ();
	SDL_Quit();
	return 1;
}

void sdl2_done(void)
{
	sdl2_close_window ();

	if (!need_quit)
		return;

	fontengine_done ();

	SDL_Quit();

	if (virtual_framebuffer)
	{
		free (virtual_framebuffer);
		plVidMem = virtual_framebuffer = 0;
	}

	need_quit = 0;

	free (SDL2ScrTextGUIOverlays);
	SDL2ScrTextGUIOverlays = 0;
	SDL2ScrTextGUIOverlays_size = 0;
	SDL2ScrTextGUIOverlays_count = 0;
}
