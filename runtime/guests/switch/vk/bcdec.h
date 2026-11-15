/**
 * @file bcdec.h
 * @brief Decoders for one 4x4 block of BC1 to BC7 (bcdec.c, ported from bcdec, MIT).
 *
 * Each writes the 4x4 texels of a block, rows destinationPitch apart: bytes
 * for BC1 to BC5 and BC7, 16-bit halves for BC6H.
 */
#ifndef RELIFT_SWITCH_BCDEC_H
#define RELIFT_SWITCH_BCDEC_H

/** @brief BC1: 8 bytes to RGBA8 (alpha 0 for the transparent texels of three-color blocks). */
void bcdec_bc1(const void *compressedBlock, void *decompressedBlock, int destinationPitch);
/** @brief BC2: 16 bytes to RGBA8. */
void bcdec_bc2(const void *compressedBlock, void *decompressedBlock, int destinationPitch);
/** @brief BC3: 16 bytes to RGBA8. */
void bcdec_bc3(const void *compressedBlock, void *decompressedBlock, int destinationPitch);
/** @brief BC4: 8 bytes to R8, unsigned or signed. */
void bcdec_bc4(const void *compressedBlock, void *decompressedBlock, int destinationPitch, int isSigned);
/** @brief BC5: 16 bytes to RG8, unsigned or signed. */
void bcdec_bc5(const void *compressedBlock, void *decompressedBlock, int destinationPitch, int isSigned);
/** @brief BC6H: 16 bytes to RGB16F; the pitch counts halves. */
void bcdec_bc6h_half(const void *compressedBlock, void *decompressedBlock, int destinationPitch, int isSigned);
/** @brief BC7: 16 bytes to RGBA8. */
void bcdec_bc7(const void *compressedBlock, void *decompressedBlock, int destinationPitch);

#endif
