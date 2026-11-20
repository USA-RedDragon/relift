/**
 * @file romfs_image.h
 * @brief The RomFS image a game reads, from one of three sources:
 *
 * - an extracted tree: the header and metadata tables are built in memory
 *   when the tree is opened and file data is read from the host files on
 *   demand, so a game with many gigabytes of data needs only the metadata in
 *   memory and no second copy on disk;
 * - a plain image file (relift's romfs.img), possibly at an offset inside a
 *   larger file, served as it is with positioned reads;
 * - a RomFS map (relift's romfs.map, "RLRFMAP1"): the metadata of an image and
 *   the places in other host files (a .nup, a blob of decompressed files)
 *   where each file's data already is. Its format is described in
 *   guests/switch/formats/src/romfs_image.rs. Every source it names is
 *   checked by size and by 32 recorded bytes when the map is opened.
 *
 * The image and map sources take one host file descriptor per file and no
 * directory scan, so opening a game with hundreds of thousands of files costs
 * the same as opening one.
 *
 * The layout follows the RomFS format as libnn reads it: a 0x50 byte header,
 * the directory and file hash tables, the directory and file metadata, then
 * the file data (each file at a 0x10 aligned offset, the data area at a 0x200
 * aligned offset). Entries are sorted by name inside each directory.
 */
#ifndef RELIFT_ROMFS_IMAGE_H
#define RELIFT_ROMFS_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief An open image. Reads are safe from several threads at once. */
typedef struct rl_romfs_image rl_romfs_image;

/** @brief Status of rl_romfs_image_read: the range reaches past the image. */
#define RL_ROMFS_RANGE (-1)

/**
 * @brief Opens root: an extracted tree (scanned, metadata built), a plain
 * image file or a RomFS map. Returns NULL with errno set when it cannot be
 * read, is malformed (EINVAL), names a source that changed (ESTALE) or is
 * missing (ENOLINK) or is too large for the format.
 */
rl_romfs_image *rl_romfs_image_open(const char *root);

/**
 * @brief The path of the source file a RomFS map names that does not exist,
 * set when a rl_romfs_image_open on this thread failed with ENOLINK: the map
 * is there, but a file it reads from is not.
 */
const char *rl_romfs_image_missing_source(void);

/**
 * @brief Opens the plain image of length bytes at offset in the file at path
 * (length 0: to the end of the file), or with offset and length 0 a RomFS map.
 * Returns NULL with errno set as rl_romfs_image_open does.
 */
rl_romfs_image *rl_romfs_image_open_file(const char *path, uint64_t offset, uint64_t length);

/** @brief One tree of a layered image. */
typedef struct rl_romfs_layer {
    /** @brief Name used in conflict reports; NULL uses root. */
    const char *name;
    /** @brief Extracted tree, plain image file or RomFS map. */
    const char *root;
    /** @brief True for the base tree: replacing its files is not a conflict. */
    bool base;
} rl_romfs_layer;

/**
 * @brief Called while an image is built for each file or directory that two
 * layers provide, when the loser is not a base layer. path is the path inside
 * the image, winner and loser are layer names.
 */
typedef void (*rl_romfs_conflict_fn)(void *user, const char *path, const char *winner, const char *loser);

/**
 * @brief Builds one image from layers in order, later layers in front: for a
 * name two layers both provide, the later one wins per file and directories
 * merge. The result is byte-exact for the merged tree, and with one layer
 * equals rl_romfs_image_open (one image or map layer is served as it is).
 * Returns NULL with errno set as rl_romfs_image_open does.
 */
rl_romfs_image *rl_romfs_image_open_layers(const rl_romfs_layer *layers, size_t n, rl_romfs_conflict_fn warn,
                                           void *user);

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
