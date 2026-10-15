// evb.h - unpack the virtual file system that Enigma Virtual Box appends to
// a Windows executable (how the Chinese patch, BGI-CHS.exe, carries its
// translated scripts and pictures).
//
// SPDX-License-Identifier: GPL-2.0-only
#ifndef SUBAHIBI_EVB_H
#define SUBAHIBI_EVB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// progress: bytes written so far and the total to write; return 0 to cancel
typedef int (*EvbProgress_t)(void* user, uint64_t done, uint64_t total);

/* Extract every file of the executable's virtual file system into outDir
 * (created as needed; the folders of the tree become directories).  The
 * number of files written, or -1 with a message in err (errLen bytes). */
int Evb_Extract(const char* exePath, const char* outDir, EvbProgress_t progress, void* user, char* err, size_t errLen);

/* aPLib decompression of one block: the bytes written to dst (at most
 * dstCap).  Decoding stops where the input runs out or turns invalid; the
 * caller checks the total against the expected size.  Exposed for the
 * tests. */
long Evb_Depack(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap);

#ifdef __cplusplus
}
#endif

#endif
