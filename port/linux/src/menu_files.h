/*
MENU_FILES.H

The menus' files (port/assets/menus: their XML, and the PNGs of their
bitmaps), embedded by tools/embed_assets.py; menu_files.c reads them, and
those in a menus folder beside config.toml, for the game (halo_menus.h).
*/

#ifndef MENU_FILES_H
#define MENU_FILES_H

/* an embedded file, by its path in port/assets/menus; on the 64 MB Xbox a
picture's data is NULL, and it lies at `offset` in the pack beside the
program instead (menu_files.c) */
struct menu_file_embedded
{
	const char *path;
	const unsigned int *data;
	unsigned int size;
	unsigned int offset;
};

extern const struct menu_file_embedded menu_files_embedded[];
extern const unsigned int menu_files_embedded_count;

/* the PNG standing for a menu bitmap's D3D texture (data: its Data), and
its size; NULL if it is none. On the 64 MB Xbox it is read from the pack and
kept until menu_art_png_release, or the next call. */
const unsigned char *menu_art_png(unsigned long data, unsigned long *size);
void menu_art_png_release(void);

/* which file that PNG is, the same for as long as the menus are (-1 if it
is none), without reading it */
long menu_art_file(unsigned long data);

/* the GL texture standing for a menu bitmap's D3D texture (data: its Data),
decoded from its PNG on first use, and its mip levels; 0 if it is none */
unsigned int menu_art_texture(unsigned long data, unsigned long *levels);

#endif
