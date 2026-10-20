/*
XBOX_PLATFORM.C

The desktop ports' hooks the game calls (port/linux/src/sdl_platform.c has
them there), as the Xbox answers them, and what it leaves out or does not
have yet:

- the high-res HUD and text (port/linux/src/hud_hires.c, text_hires.c),
  which the Xbox leaves out (port/xbox/README.md): no textures, no fonts;
- voice chat (port/linux/src/voice_audio.c): no microphone, and no voices;
- Halo Custom Edition maps (port/linux/src/xbox_memory.c's tag cache for
  them): none, so the game plays the Xbox's own maps alone.
*/

#include "platform.h"
#include "halo_ui_pointer.h"
#include "hud_hires.h"
#include "nxdk_platform.h"
#include "port_mods.h"
#include "text_hires.h"
#include "voice_audio.h"

#include <string.h>

/* ---------- messages, the clipboard and the display */

void platform_show_message(const char *title, const char *message)
{
	platform_log("%s: %s", title, message);
}

/* no clipboard: invites will be typed on the on-screen keyboard */
int platform_clipboard_get(char *text, int size)
{
	if (size > 0)
		text[0] = 0;
	return 0;
}

void platform_clipboard_set(const char *text)
{
	(void)text;
}

/* one video mode, which the renderer sets */
void platform_display_apply(void)
{
}

/* the maps come from the Halo disc or the hard disk (port/xbox/README.md):
nothing to copy them out of, so the player is asked for the disc */
BOOL platform_offer_game_data(const char *destination)
{
	(void)destination;
	return xbox_wait_for_maps() ? TRUE : FALSE;
}

/* the scoreboard's mouse wheel and page keys: none (a closing scoreboard
passes no counts: game_engine_scoreboard_closed) */
void platform_scoreboard_scroll(int open, long *notches, long *pages)
{
	(void)open;
	if (notches)
		*notches = 0;
	if (pages)
		*pages = 0;
}

/* a frame per game tick, as the Xbox drew it (the plan: no frames between
ticks) */
int halo_interpolation_enabled(void)
{
	return 0;
}

/* no mouse */
int halo_ui_pointer_update(int menus_active, struct halo_ui_pointer *pointer)
{
	(void)menus_active;
	(void)pointer;
	return 0;
}

int halo_scoreboard_pointer_update(int offered, struct halo_ui_pointer *pointer)
{
	(void)offered;
	(void)pointer;
	return 0;
}

/* the menus' choices of a desktop's displays, windows and sound devices:
none (port/linux/game/menu_tags.c) */
int platform_display_resolutions(long *widths, long *heights, int maximum)
{
	(void)widths;
	(void)heights;
	(void)maximum;
	return 0;
}

int platform_window_sizes(long *widths, long *heights, int maximum)
{
	(void)widths;
	(void)heights;
	(void)maximum;
	return 0;
}

int platform_audio_devices(int recording, char (*names)[128], int maximum)
{
	(void)recording;
	(void)names;
	(void)maximum;
	return 0;
}

/* ---------- Halo Custom Edition maps: the tag cache is nxdk_memory.c's.
The renderer samples every texture in the Xbox's channel order (the NV2A
has no texture swizzle), so a Custom Edition multipurpose map or HUD meter
is drawn with its channels as Halo PC keeps them */

void halo_custom_edition_texels_channels(const void *texels, unsigned char channel_order)
{
	(void)texels;
	(void)channel_order;
}

void halo_custom_edition_texels_forget(void)
{
}

/* ---------- voice chat: no microphone, and no voices */

int halo_push_to_talk_held(void)
{
	return 0;
}

int voice_audio_microphone(int open)
{
	(void)open;
	return 0;
}

int voice_audio_read_frame(float *frame)
{
	(void)frame;
	return 0;
}

float voice_audio_level(const float *frame)
{
	(void)frame;
	return 0.0f;
}

int voice_audio_encode(const float *frame, int bitrate, unsigned char *packet, int maximum)
{
	(void)frame;
	(void)bitrate;
	(void)packet;
	(void)maximum;
	return 0;
}

void voice_audio_play(int speaker, unsigned short sequence, const unsigned char *packet, int length, float gain,
	float pan)
{
	(void)speaker;
	(void)sequence;
	(void)packet;
	(void)length;
	(void)gain;
	(void)pan;
}

void voice_audio_forget(int speaker)
{
	(void)speaker;
}

void voice_audio_forget_all(void)
{
}

int voice_audio_speaking(int speaker)
{
	(void)speaker;
	return 0;
}

void voice_audio_set_volume(float volume)
{
	(void)volume;
}

void voice_audio_mix(float *output, unsigned long frames)
{
	(void)output;
	(void)frames;
}

/* ---------- the high-res HUD and text: left out */

long hud_hires_asset_count(void)
{
	return 0;
}

char const *hud_hires_asset_tag(long asset)
{
	(void)asset;
	return "";
}

long hud_hires_asset_bitmap(long asset)
{
	(void)asset;
	return -1;
}

long hud_hires_asset_fits(long asset, long width, long height)
{
	(void)asset;
	(void)width;
	(void)height;
	return 0;
}

unsigned long hud_hires_asset_sprites(long asset)
{
	(void)asset;
	return 0;
}

int hud_hires_sprites_drawable(long asset, unsigned long address, unsigned long level0_size)
{
	(void)asset;
	(void)address;
	(void)level0_size;
	return 0;
}

void hud_hires_register_placeholder(long asset, unsigned long const *texture)
{
	(void)asset;
	(void)texture;
}

int hud_hires_asset_custom_edition(long asset)
{
	(void)asset;
	return 0;
}

void hud_hires_map_loaded(const long *assets, long count)
{
	(void)assets;
	(void)count;
}

void hud_hires_map_unloaded(void)
{
}

int text_hires_enabled(void)
{
	return 0;
}

/* ---------- the GL renderer's additions: the Xbox's own picture

Models at the detail the Xbox's 480 lines choose (stock: display.model_detail
is not offered on the Xbox), no HDR tone mapping (display.hdr, GL's), and the
plasma shader's height fade (GL's) not drawn. */

float halo_model_detail_scale(int level)
{
	(void)level;
	return 1.0f;
}

int halo_model_detail_stock(void)
{
	return 1;
}

void halo_screen_tone_map(short x0, short y0, short x1, short y1)
{
	(void)x0;
	(void)y0;
	(void)x1;
	(void)y1;
}

void d3d8_gl_set_height_fade(float inverse_softness, float height, float offset, float scale)
{
	(void)inverse_softness;
	(void)height;
	(void)offset;
	(void)scale;
}

/* ---------- mods: none on the Xbox

port/linux/src/port_mods.c reads the mods folder through SDL, and a mod's
tags and files would not fit beside a map in the Xbox's memory: the Mods
menu lists none, and maps play as they are. */

long port_mods_scan(void)
{
	return 0;
}

unsigned long port_mods_scans(void)
{
	return 0;
}

long port_mods_played(void)
{
	return 0;
}

long port_mods_file_find(const char *tag_file, const char *map_name)
{
	(void)tag_file;
	(void)map_name;
	return -1;
}

int port_mods_file_stamp(long file, unsigned long *size, unsigned long stamp[2])
{
	(void)file;
	(void)size;
	(void)stamp;
	return 0;
}

int port_mods_file_read(long file, unsigned long offset, void *buffer, unsigned long size)
{
	(void)file;
	(void)offset;
	(void)buffer;
	(void)size;
	return 0;
}

long port_mods_file_order(long file)
{
	(void)file;
	return -1;
}

const char *port_mods_file_mod(long file)
{
	(void)file;
	return "";
}

const char *port_mods_file_path(long file)
{
	(void)file;
	return "";
}

long port_mods_count(void)
{
	return 0;
}

const char *port_mods_folder(long index)
{
	(void)index;
	return "";
}

int port_mods_describe(long index, char *text, unsigned long size, const char *map_name)
{
	(void)index;
	(void)map_name;
	if (size)
		text[0] = 0;
	return 0;
}

long port_mods_at(long position)
{
	(void)position;
	return -1;
}

const char *port_mods_name(long index)
{
	(void)index;
	return "";
}

int port_mods_on(long index)
{
	(void)index;
	return 0;
}

int port_mods_status(long index, char *text, unsigned long size)
{
	(void)index;
	if (size)
		text[0] = 0;
	return 0;
}

int port_mods_details(long index, char *text, unsigned long size)
{
	(void)index;
	if (size)
		text[0] = 0;
	return 0;
}

long port_mods_move(long position, long places)
{
	(void)places;
	return position;
}

void port_mods_turn(long index, int on)
{
	(void)index;
	(void)on;
}

int port_mods_save(void)
{
	return 1;
}

void port_mods_revert(void)
{
}

int port_mods_conflicts(char *text, unsigned long size)
{
	if (size)
		text[0] = 0;
	return 0;
}

int port_mods_conflicting_names(char *text, unsigned long size)
{
	if (size)
		text[0] = 0;
	return 0;
}

void port_mods_disable_conflicting(void)
{
}

void port_mods_startup(void)
{
}

/* (the NV2A renderer keeps no GL texture names to forget) */
void xgpu_gl_texture_deleted(unsigned int texture)
{
	(void)texture;
}

/* (the menus' bitmaps: the renderer's, once it has one) */
unsigned int hud_hires_png_texture(const void *png, unsigned long size, unsigned long *levels)
{
	(void)png;
	(void)size;
	*levels = 0;
	return 0;
}

long text_hires_font(char const *tag_name, float cap_height, short const *letter_widths, float oversample)
{
	(void)tag_name;
	(void)cap_height;
	(void)letter_widths;
	(void)oversample;
	return -1;
}

int text_hires_covers(long font, unsigned long code)
{
	(void)font;
	(void)code;
	return 0;
}

int text_hires_glyph(long font, unsigned long code, struct text_hires_glyph *glyph)
{
	(void)font;
	(void)code;
	(void)glyph;
	return 0;
}

void text_hires_register_atlas(const unsigned long *texture, unsigned long width, unsigned long height)
{
	(void)texture;
	(void)width;
	(void)height;
}

void text_hires_set_reset_hook(void (*hook)(void))
{
	(void)hook;
}

/* ---------- the renderer's texture write tracking (port/linux/src/memory_watch.c):
none, as the Xbox's GPU reads the textures where the game writes them */

void memory_watch_prepare_write(void *address, unsigned long size)
{
	(void)address;
	(void)size;
}
