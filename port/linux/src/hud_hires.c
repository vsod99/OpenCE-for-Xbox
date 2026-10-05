/*
HUD_HIRES.C

The high-res HUD's textures (hud_hires.h): which one stands for a bitmap being
uploaded, and each one's GL texture.

Which bitmap is at an address the game knows (from the loaded map's tags:
port/linux/game/hud_hires_tags.c). Each texture is decoded from its PNG when
first drawn and kept: up to 71 of the HUD's, about 227 MB with their mip
levels, though a game draws only some (the scopes' only when zoomed), and
the titles of the menus shown, about 3 MB each (11 MB for the carnage
report's, a whole panel), and their button icons, about 0.3 MB each (5.3
MB for the message icons' sheet). On the desktop builds a map's HUD PNGs
are decoded as it loads, on a thread of their own, so that the HUD's first
frame only uploads them (decoding them there held that frame up); what is
decoded and not drawn is let go when the map is unloaded.
They are drawn with linear filtering and their mip levels (d3d8_gl.c,
configure_sampler), as they are larger than they appear.

The embedded PNGs are the ones tools/hud_assets.py, title_assets.py and
button_assets.py write. A menus folder's PNGs can be anyone's. Only 8-bit
RGBA, non-interlaced PNGs are read (png_decode.c).
*/

#include "hud_hires.h"
#include "png_decode.h"
#include "platform.h"
#include "port_config.h"
#include "xgpu.h"

#include "zlib_prefixed.h"

#if !defined(HALO_ANDROID) && !defined(HALO_WEB)
#define HUD_HIRES_DECODE_AHEAD
#include <pthread.h>
#endif
#include <stdlib.h>
#include <string.h>

/* the game's (port/linux/game/hud_hires_tags.c) */
long hud_hires_asset_at(unsigned long address, long width, long height);
long hud_hires_asset_stock_custom_edition(unsigned long address, long width, long height, long asset);

/* Include every embedded HUD and button style, including stick frames. */
#define MAXIMUM_TEXTURES 256

/* a texture's PNG decoded ahead of its first draw (decode_state) */
enum
{
	_decode_none,
	_decode_queued,
	_decode_decoding,
	_decode_done,
};

static struct
{
	unsigned int texture;
	unsigned long levels;
	int failed;
	int other_pixels_logged;
	/* (decoded ahead: these, the decoding thread's until it is done, under
	decode_lock) */
	int decode_state;
	unsigned char *pixels;
	unsigned long width, height;
} textures[MAXIMUM_TEXTURES];

#ifdef HUD_HIRES_DECODE_AHEAD
/* (the thread, detached, is waited for by its decode_running) */
static pthread_mutex_t decode_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t decode_finished = PTHREAD_COND_INITIALIZER;
static int decode_running, decode_stopping;
#endif

long hud_hires_asset_count(void)
{
	return hud_hires_embedded_count < MAXIMUM_TEXTURES ? (long)hud_hires_embedded_count : MAXIMUM_TEXTURES;
}

/* whether the texture may stand for a Custom Edition map's stock bitmap of
its name too (port/assets/hud/custom_edition.json) */
int hud_hires_asset_custom_edition(long asset)
{
	return asset >= 0 && asset < hud_hires_asset_count() && hud_hires_embedded[asset].custom_edition;
}

char const *hud_hires_asset_tag(long asset)
{
	return hud_hires_embedded[asset].tag;
}

long hud_hires_asset_bitmap(long asset)
{
	return hud_hires_embedded[asset].bitmap;
}

/* whether the texture can stand for a bitmap of this size: a whole multiple
of it, the same both ways */
long hud_hires_asset_fits(long asset, long width, long height)
{
	const struct hud_hires_embedded *embedded = &hud_hires_embedded[asset];

	return width > 0 && height > 0 && embedded->width % width == 0 && embedded->height % height == 0 &&
		embedded->width / width == embedded->height / height && embedded->width / width > 1;
}

/* Changed settings since the previous rendered frame: HUD bit 0, titles bit 1.
 * Pending menu edits do not change config_changes(). */
unsigned long hud_hires_settings_changed(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static unsigned long previous;
	unsigned long current, changed;

	if (read_at == config_changes()) return 0;
	current = (config_boolean("display.high_res_hud") ? 1UL : 0UL) |
		(config_boolean("display.high_res_text") ? 2UL : 0UL);
	changed = read_at == (unsigned long)-1 ? 0 : previous ^ current;
	read_at = config_changes();
	previous = current;
	return changed;
}

int hud_hires_bitmap_affected(unsigned long address, long width, long height, unsigned long changed)
{
	long asset = hud_hires_asset_at(address, width, height);

	return asset >= 0 && asset < hud_hires_asset_count() &&
		(changed & (hud_hires_embedded[asset].title ? 2UL : 1UL)) != 0;
}

long hud_hires_override_find(unsigned long address, unsigned long width, unsigned long height,
	unsigned long level0_size)
{
	static int hud_enabled, titles_enabled;
	static unsigned long read_at = (unsigned long)-1;
	long asset;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		hud_enabled = config_boolean("display.high_res_hud");
		titles_enabled = config_boolean("display.high_res_text");
	}
	if (!hud_enabled && !titles_enabled)
		return -1;
	asset = hud_hires_asset_at(address, (long)width, (long)height);
	if (asset < 0 || asset >= hud_hires_asset_count())
		return -1;
	if (!(hud_hires_embedded[asset].title ? titles_enabled : hud_enabled))
		return -1;
	/* (a Custom Edition map's stock HUD bitmap, laid out as this texture's,
	has its own pixels) */
	if (hud_hires_asset_stock_custom_edition(address, (long)width, (long)height, asset))
		return asset;
	if (crc32(0L, (const Bytef *)address, (uInt)level0_size) != hud_hires_embedded[asset].crc)
	{
		if (!textures[asset].other_pixels_logged)
		{
			platform_log("high-res hud: %s bitmap %d is not the one its texture was drawn for here "
				"(another language's or a modified map): drawn as it is",
				hud_hires_embedded[asset].tag, hud_hires_embedded[asset].bitmap);
			textures[asset].other_pixels_logged = 1;
		}
		return -1;
	}
	return asset;
}

int hud_hires_override_coverage(long asset)
{
	return asset >= 0 && asset < hud_hires_asset_count() && hud_hires_embedded[asset].coverage;
}

/* ---------- sprites */

/* the placeholders of the textures drawn for some of a bitmap's sprites
(port/linux/game/hud_hires_tags.c): their D3D textures' Data */
/* Room for the gameplay button styles and map-owned sprite placeholders. */
#define MAXIMUM_PLACEHOLDERS 32

static struct
{
	unsigned long data;
	long asset;
} placeholders[MAXIMUM_PLACEHOLDERS];
static long placeholder_count = 0;

/* the sequences whose sprites the texture is drawn for (hud_hires.h), or 0 */
unsigned long hud_hires_asset_sprites(long asset)
{
	return hud_hires_embedded[asset].sprites;
}

/* whether the texture drawn for some of a bitmap's sprites can be drawn for
the bitmap whose pixels are at address (guest virtual; its first mip level
level0_size bytes): its setting on, the pixels those it was drawn for, and
the texture decoded */
int hud_hires_sprites_drawable(long asset, unsigned long address, unsigned long level0_size)
{
	static int hud_enabled, titles_enabled;
	static unsigned long read_at = (unsigned long)-1;
	unsigned long levels;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		hud_enabled = config_boolean("display.high_res_hud");
		titles_enabled = config_boolean("display.high_res_text");
	}
	if (asset < 0 || asset >= hud_hires_asset_count() || !hud_hires_embedded[asset].sprites ||
		!(hud_hires_embedded[asset].title ? titles_enabled : hud_enabled))
	{
		return 0;
	}
	if (crc32(0L, (const Bytef *)address, (uInt)level0_size) != hud_hires_embedded[asset].crc)
	{
		if (!textures[asset].other_pixels_logged)
		{
			platform_log("high-res hud: %s bitmap %d is not the one its sprites' texture was drawn for "
				"here (another language's or a modified map): drawn as it is",
				hud_hires_embedded[asset].tag, hud_hires_embedded[asset].bitmap);
			textures[asset].other_pixels_logged = 1;
		}
		return 0;
	}
	return hud_hires_override_texture(asset, &levels) != 0;
}

/* the placeholder the game draws the texture's sprites from: its D3D
texture (NULL forgets it) */
void hud_hires_register_placeholder(long asset, const unsigned long *texture)
{
	long index;

	for (index = 0; index < placeholder_count && placeholders[index].asset != asset; index++)
		;
	if (texture && index == placeholder_count && placeholder_count < MAXIMUM_PLACEHOLDERS)
		placeholder_count++;
	if (index < placeholder_count)
	{
		placeholders[index].data = texture ? texture[1] : 0;
		placeholders[index].asset = asset;
	}
}

unsigned int hud_hires_placeholder_texture(unsigned long data, unsigned long *levels)
{
	long index;

	for (index = 0; data && index < placeholder_count; index++)
	{
		if (placeholders[index].data == data)
			return hud_hires_override_texture(placeholders[index].asset, levels);
	}
	return 0;
}

int hud_hires_override_point_threshold(long asset)
{
	return asset >= 0 && asset < hud_hires_asset_count() && hud_hires_embedded[asset].point_threshold;
}

/* ---------- decoding */

/* Catmull-Rom interpolation of the map's original texels. No font, sprite
 * rectangle, registration point or animation frame is substituted. Keeping
 * the input centres at (x + .5) also keeps the original HUD alignment. */
static void upscale_weights(float t, float *weights)
{
	float square = t * t, cube = square * t;

	weights[0] = -0.5f * t + square - 0.5f * cube;
	weights[1] = 1.0f - 2.5f * square + 1.5f * cube;
	weights[2] = 0.5f * t + 2.0f * square - 1.5f * cube;
	weights[3] = -0.5f * square + 0.5f * cube;
}

unsigned char *hud_hires_upscale_pixels(const unsigned char *bgra, unsigned long width, unsigned long height)
{
	unsigned char *out;
	unsigned long x, y, channel;

	if (!bgra || !width || !height || width > 256 || height > 256)
		return NULL;
	out = malloc(width * height * 16 * 4);
	if (!out)
		return NULL;
	for (y = 0; y < height * 4; y++)
	{
		float sy = (y + 0.5f) * 0.25f - 0.5f, wy[4];
		long iy = (long)sy;

		if (sy < iy) iy--;
		upscale_weights(sy - iy, wy);
		for (x = 0; x < width * 4; x++)
		{
			float sx = (x + 0.5f) * 0.25f - 0.5f, wx[4];
			long ix = (long)sx;

			if (sx < ix) ix--;
			upscale_weights(sx - ix, wx);
			for (channel = 0; channel < 4; channel++)
			{
				float value = 0.0f;
				long row, column;
				unsigned long source_channel = channel == 0 ? 2 : channel == 2 ? 0 : channel;

				for (row = 0; row < 4; row++)
				{
					long py = iy + row - 1;

					if (py < 0) py = 0;
					if (py >= (long)height) py = height - 1;
					for (column = 0; column < 4; column++)
					{
						long px = ix + column - 1;

						if (px < 0) px = 0;
						if (px >= (long)width) px = width - 1;
						value += bgra[(py * width + px) * 4 + source_channel] * wy[row] * wx[column];
					}
				}
				out[(y * width * 4 + x) * 4 + channel] =
					(unsigned char)(value < 0 ? 0 : value > 255 ? 255 : value + 0.5f);
			}
		}
	}
	return out;
}

/* The background's old icon otherwise draws over the existing high-res meter
 * contour. Only the five known Xbox sheets get this matching contour; their
 * text, and customized sheets, retain their original pixels and layout. */
static void upscale_outline(unsigned char *pixels, const unsigned char *bgra,
	unsigned long width, unsigned long height, long kind)
{
	unsigned long crc, png_width, png_height, row;
	long asset;
	unsigned char *outline;
	char const *name;
	unsigned long left, top, right, bottom;

	if (width != 128 || height != 32 || (kind != 2 && kind != 3))
		return;
	crc = crc32(0L, bgra, (uInt)(width * height * 4));
	if (kind == 2)
	{
		if (crc != 0x23894da5UL && crc != 0x38fb467aUL && crc != 0xcd279468UL)
			return;
		name = "ui\\hud\\redraws\\vehicle_wrench_outline";
		left = 73; top = 11; right = 101; bottom = 25;
	}
	else
	{
		if (crc != 0x17446e88UL && crc != 0xb14f00a1UL)
			return;
		name = "ui\\hud\\redraws\\passenger_health_outline";
		left = 74; top = 10; right = 89; bottom = 24;
	}
	for (asset = 0; asset < hud_hires_asset_count(); asset++)
	{
		const struct hud_hires_embedded *embedded = &hud_hires_embedded[asset];

		if (strcmp(embedded->tag, name))
			continue;
		outline = png_decode((const unsigned char *)embedded->png, embedded->png_size, &png_width, &png_height);
		if (outline && png_width == width * 4 && png_height == height * 4)
			for (row = top * 4; row < bottom * 4; row++)
				memcpy(pixels + (row * width * 4 + left * 4) * 4,
					outline + (row * width * 4 + left * 4) * 4, (right - left) * 16);
		free(outline);
		break;
	}
}

unsigned int hud_hires_upscale_texture(const unsigned char *bgra, unsigned long width,
	unsigned long height, long kind, unsigned long *levels)
{
	unsigned char *pixels = hud_hires_upscale_pixels(bgra, width, height);
	unsigned long largest;
	GLuint texture;

	if (!pixels)
		return 0;
	upscale_outline(pixels, bgra, width, height, kind);
	*levels = 1;
	for (largest = (width > height ? width : height) * 4; largest > 1; largest >>= 1)
		(*levels)++;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)*levels - 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width * 4, (GLsizei)height * 4,
		0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glGenerateMipmap(GL_TEXTURE_2D);
	xgpu_gl_state_invalidate();
	free(pixels);
	return texture;
}

/* a GL texture of a decoded PNG's texels (which it frees), as below */
static unsigned int png_pixels_texture(unsigned char *pixels, unsigned long width, unsigned long height,
	unsigned long *levels)
{
	unsigned long largest;
	GLuint texture;

	if (!pixels)
		return 0;
	*levels = 1;
	for (largest = width > height ? width : height; largest > 1; largest >>= 1)
		(*levels)++;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	xgpu_gl_state_invalidate();
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)*levels - 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glGenerateMipmap(GL_TEXTURE_2D);
	xgpu_gl_state_invalidate();
	free(pixels);
	return texture;
}

unsigned int hud_hires_png_texture(const void *png, unsigned long size, unsigned long *levels)
{
	unsigned long width = 0, height = 0;
	unsigned char *pixels = png_decode(png, size, &width, &height);

	return png_pixels_texture(pixels, width, height, levels);
}

unsigned int hud_hires_override_texture(long asset, unsigned long *levels)
{
	const struct hud_hires_embedded *embedded;

	if (asset < 0 || asset >= hud_hires_asset_count() || textures[asset].failed)
		return 0;
	if (textures[asset].texture)
	{
		*levels = textures[asset].levels;
		return textures[asset].texture;
	}
	embedded = &hud_hires_embedded[asset];
	{
		unsigned char *pixels = NULL;
		unsigned long width = 0, height = 0;
		int decoded = 0;

#ifdef HUD_HIRES_DECODE_AHEAD
		/* (its PNG decoded as the map loaded, or being decoded: waited for) */
		pthread_mutex_lock(&decode_lock);
		while (textures[asset].decode_state == _decode_decoding)
			pthread_cond_wait(&decode_finished, &decode_lock);
		if (textures[asset].decode_state == _decode_done)
		{
			pixels = textures[asset].pixels;
			width = textures[asset].width;
			height = textures[asset].height;
			textures[asset].pixels = NULL;
			decoded = 1;
		}
		textures[asset].decode_state = _decode_none;
		pthread_mutex_unlock(&decode_lock);
#endif
		if (!decoded)
			pixels = png_decode((const unsigned char *)embedded->png, embedded->png_size, &width, &height);
		textures[asset].texture = png_pixels_texture(pixels, width, height, &textures[asset].levels);
	}
	if (!textures[asset].texture)
	{
		platform_log("high-res hud: could not decode the texture for %s bitmap %d", embedded->tag, embedded->bitmap);
		textures[asset].failed = 1;
		return 0;
	}
	*levels = textures[asset].levels;
	return textures[asset].texture;
}

/* ---------- decoding ahead */

#ifdef HUD_HIRES_DECODE_AHEAD
static void *decode_thread_main(void *unused)
{
	long asset, count = hud_hires_asset_count();

	(void)unused;
	for (asset = 0; asset < count; asset++)
	{
		const struct hud_hires_embedded *embedded = &hud_hires_embedded[asset];
		unsigned long width = 0, height = 0;
		unsigned char *pixels;

		pthread_mutex_lock(&decode_lock);
		if (decode_stopping)
		{
			pthread_mutex_unlock(&decode_lock);
			break;
		}
		if (textures[asset].decode_state != _decode_queued)
		{
			pthread_mutex_unlock(&decode_lock);
			continue;
		}
		textures[asset].decode_state = _decode_decoding;
		pthread_mutex_unlock(&decode_lock);
		pixels = png_decode((const unsigned char *)embedded->png, embedded->png_size, &width, &height);
		pthread_mutex_lock(&decode_lock);
		textures[asset].pixels = pixels;
		textures[asset].width = width;
		textures[asset].height = height;
		textures[asset].decode_state = _decode_done;
		pthread_cond_broadcast(&decode_finished);
		pthread_mutex_unlock(&decode_lock);
	}
	pthread_mutex_lock(&decode_lock);
	decode_running = 0;
	pthread_cond_broadcast(&decode_finished);
	pthread_mutex_unlock(&decode_lock);
	return NULL;
}
#endif

void hud_hires_map_unloaded(void)
{
#ifdef HUD_HIRES_DECODE_AHEAD
	long asset;

	pthread_mutex_lock(&decode_lock);
	decode_stopping = 1;
	while (decode_running)
		pthread_cond_wait(&decode_finished, &decode_lock);
	pthread_mutex_unlock(&decode_lock);
	for (asset = 0; asset < MAXIMUM_TEXTURES; asset++)
	{
		free(textures[asset].pixels);
		textures[asset].pixels = NULL;
		textures[asset].decode_state = _decode_none;
	}
#endif
}

void hud_hires_map_loaded(const long *assets, long count)
{
#ifdef HUD_HIRES_DECODE_AHEAD
	long index, queued = 0;
	pthread_t thread;

	hud_hires_map_unloaded();
	/* (none for a test's run that draws nothing, or with the high-res HUD
	off: then each is decoded when first drawn, as titles are) */
	if (!config_boolean("display.high_res_hud") || config_boolean("debug.null_renderer"))
		return;
	for (index = 0; index < count; index++)
	{
		long asset = assets[index];

		if (asset >= 0 && asset < hud_hires_asset_count() && !hud_hires_embedded[asset].title &&
			!textures[asset].texture && !textures[asset].failed)
		{
			textures[asset].decode_state = _decode_queued;
			queued++;
		}
	}
	if (!queued)
		return;
	decode_stopping = 0;
	decode_running = 1;
	if (pthread_create(&thread, NULL, decode_thread_main, NULL) == 0)
	{
		pthread_detach(thread);
	}
	else
	{
		decode_running = 0;
		for (index = 0; index < MAXIMUM_TEXTURES; index++)
			textures[index].decode_state = _decode_none;
	}
#else
	(void)assets;
	(void)count;
#endif
}
