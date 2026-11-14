/**
 * @file romfs_image.h
 * @brief A byte-exact RomFS image synthesized from an extracted tree: the
 * header and metadata tables are built in memory when the tree is opened and
 * file data is read from the host files on demand, so a game with many
 * gigabytes of data needs only the metadata in memory and no second copy on
 * disk.
 *
 * The layout follows the RomFS format as libnn reads it: a 0x50 byte header,
 * the directory and file hash tables, the directory and file metadata, then
 * the file data (each file at a 0x10 aligned offset, the data area at a 0x200
 * aligned offset). Entries are sorted by name inside each directory.
 */
#ifndef RELIFT_ROMFS_IMAGE_H
#define RELIFT_ROMFS_IMAGE_H

#include <stdint.h>

/** @brief An open image. Reads are safe from several threads at once. */
typedef struct rl_romfs_image rl_romfs_image;

/** @brief Status of rl_romfs_image_read: the range reaches past the image. */
#define RL_ROMFS_RANGE (-1)

/**
 * @brief Scans the extracted tree at root and builds the metadata. Returns
 * NULL with errno set when the directory cannot be read or is too large for
 * the format.
 */
rl_romfs_image *rl_romfs_image_open(const char *root);

/** @brief Frees the image. */
void rl_romfs_image_close(rl_romfs_image *img);

/** @brief Size of the image in bytes. */
uint64_t rl_romfs_image_size(const rl_romfs_image *img);

/** @brief Number of files in the image. */
uint32_t rl_romfs_image_file_count(const rl_romfs_image *img);

/**
 * @brief Reads n bytes at offset off into dst. Padding between files reads as
 * zeros. Returns 0, RL_ROMFS_RANGE when off + n passes the end, or an errno
 * value when a host file cannot be read (including a file that changed size
 * since the scan).
 */
int rl_romfs_image_read(const rl_romfs_image *img, uint64_t off, void *dst, uint64_t n);

#endif
