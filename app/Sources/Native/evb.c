// evb.c - unpack the virtual file system of an Enigma Virtual Box executable
//
// SPDX-License-Identifier: GPL-2.0-only
//
// The packer appends a tree of nodes to the executable, introduced by the
// magic "EVB\0" and a 64-byte header:
//
//   main node      u32 size, 8 bytes, u32 children        (16 bytes)
//   then, node after node in depth-first order (the main node's last byte
//   doubles as the first byte of the next one):
//   header node    u32 size, 8 bytes, u32 children        (16 bytes)
//   name           UTF-16LE, up to a 0x0000 unit, then a type byte:
//                    2 file   - 53 more bytes: 2, u32 original size, 4,
//                               3 x 8 (file times), 15, u32 stored size
//                    3 folder - 25 more bytes
//   The file contents follow the whole tree, in node order, starting at
//   (end of the main node) + main size - 12.
//
// A file whose stored size differs from its original size is compressed in
// chunks: a block header (u32 size, u32), then (size - 8) bytes of
// 12-byte records whose first word is a chunk's compressed size, then the
// chunks, each an aPLib stream.  The folder named "%DEFAULT FOLDER%" is the
// directory of the executable, i.e. the root.  The format follows what the
// tool evbunpack documents; the aPLib decoder below is written from the
// format's description (LZ77 with Elias-gamma lengths and a repeated-offset
// shortcut).

#include "evb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ---- aPLib ---------------------------------------------------------------------------------

typedef struct ApState
{
	const uint8_t* src;
	const uint8_t* end;
	uint8_t* dst;
	uint8_t* dstStart;
	uint8_t* dstEnd;
	unsigned tag;
	int bits; // bits left in tag
	int bad;
} ApState_t;

static int ApByte(ApState_t* s)
{
	if(s->src >= s->end)
	{
		s->bad = 1;
		return 0;
	}
	return *s->src++;
}

static int ApBit(ApState_t* s)
{
	int bit;
	if(s->bits == 0)
	{
		s->tag = (unsigned)ApByte(s);
		s->bits = 8;
	}
	bit = (s->tag >> 7) & 1;
	s->tag <<= 1;
	s->bits--;
	return bit;
}

// an Elias-gamma number (at least 2)
static unsigned ApGamma(ApState_t* s)
{
	unsigned v = 1;
	do
	{
		v = (v << 1) + (unsigned)ApBit(s);
		if(v > 0x7fffffffu)
		{
			s->bad = 1;
			return 0;
		}
	} while(ApBit(s) && !s->bad);
	return v;
}

static void ApCopy(ApState_t* s, unsigned offs, unsigned len)
{
	if(offs == 0 || offs > (size_t)(s->dst - s->dstStart) || len > (size_t)(s->dstEnd - s->dst))
	{
		s->bad = 1;
		return;
	}
	while(len--)
	{
		*s->dst = *(s->dst - offs);
		s->dst++;
	}
}

static void ApLiteral(ApState_t* s)
{
	int c = ApByte(s);
	if(s->dst >= s->dstEnd)
	{
		s->bad = 1;
		return;
	}
	*s->dst++ = (uint8_t)c;
}

long Evb_Depack(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap)
{
	ApState_t s;
	unsigned lastOffs = 0;
	int lastWasMatch = 0;
	// a stream with the "AP32" header of the packer's own tool: skip the header
	if(srcLen >= 24 && memcmp(src, "AP32", 4) == 0)
	{
		uint32_t hdr = src[4] | src[5] << 8 | src[6] << 16 | (uint32_t)src[7] << 24;
		if(hdr >= 24 && hdr < srcLen)
		{
			src += hdr;
			srcLen -= hdr;
		}
	}
	memset(&s, 0, sizeof s);
	s.src = src;
	s.end = src + srcLen;
	s.dst = s.dstStart = dst;
	s.dstEnd = dst + dstCap;
	if(srcLen == 0)
		return 0;
	ApLiteral(&s); // the first byte is always a literal
	while(!s.bad)
	{
		if(!ApBit(&s))
		{ // 0: a literal byte
			ApLiteral(&s);
			lastWasMatch = 0;
			continue;
		}
		if(!ApBit(&s))
		{ // 10: a match with a gamma-coded offset (or the last offset again)
			unsigned offs = ApGamma(&s), len;
			if(!lastWasMatch && offs == 2)
			{
				len = ApGamma(&s);
				ApCopy(&s, lastOffs, len);
			}
			else
			{
				offs -= lastWasMatch ? 2 : 3;
				offs = (offs << 8) + (unsigned)ApByte(&s);
				len = ApGamma(&s);
				if(offs >= 32000)
					len++;
				if(offs >= 1280)
					len++;
				if(offs < 128)
					len += 2;
				ApCopy(&s, offs, len);
				lastOffs = offs;
			}
			lastWasMatch = 1;
			continue;
		}
		if(!ApBit(&s))
		{ // 110: a short match, offset 1..127 and length 2..3 in one byte; offset 0 ends the stream
			unsigned b = (unsigned)ApByte(&s), offs = b >> 1, len = 2 + (b & 1);
			if(offs == 0)
				break;
			ApCopy(&s, offs, len);
			lastOffs = offs;
			lastWasMatch = 1;
			continue;
		}
		{ // 111: a single byte from 1..15 back, or a zero byte
			unsigned offs = 0;
			int i;
			for(i = 0; i < 4; i++)
				offs = (offs << 1) + (unsigned)ApBit(&s);
			if(offs)
				ApCopy(&s, offs, 1);
			else if(s.dst < s.dstEnd)
				*s.dst++ = 0;
			else
				s.bad = 1;
			lastWasMatch = 0;
		}
	}
	/* The packer's chunks carry no end marker: the stream simply runs out
	 * (as with an offset before the start, this stops the decoding).  What
	 * was produced is returned; the caller checks the file's total size. */
	return (long)(s.dst - s.dstStart);
}

// ---- the tree ------------------------------------------------------------------------------

typedef struct EvbNode
{
	char name[512]; // UTF-8
	int type;       // 2 file, 3 folder
	uint32_t children;
	uint32_t original, stored;
	uint64_t offset; // file data, from the start of the executable
} EvbNode_t;

typedef struct EvbReader
{
	const uint8_t* base;
	size_t size;
	size_t pos;
	EvbNode_t* nodes;
	size_t count, cap;
	char* err;
	size_t errLen;
} EvbReader_t;

static uint32_t Le32(const uint8_t* p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int Fail(EvbReader_t* r, const char* msg)
{
	if(r->errLen)
		snprintf(r->err, r->errLen, "%s", msg);
	return -1;
}

// a UTF-16LE name into UTF-8; 0 when it is empty or holds a separator or "." / ".."
static int NameToUtf8(const uint8_t* p, size_t units, char* out, size_t n)
{
	size_t o = 0, i;
	for(i = 0; i < units; i++)
	{
		uint32_t c = p[i * 2] | p[i * 2 + 1] << 8;
		if(c >= 0xd800 && c < 0xdc00 && i + 1 < units)
		{
			uint32_t lo = p[i * 2 + 2] | p[i * 2 + 3] << 8;
			c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
			i++;
		}
		if(c == '/' || c == '\\' || c == ':' || c == 0)
			return 0;
		if(o + 4 >= n)
			return 0;
		if(c < 0x80)
			out[o++] = (char)c;
		else if(c < 0x800)
		{
			out[o++] = (char)(0xc0 | c >> 6);
			out[o++] = (char)(0x80 | (c & 0x3f));
		}
		else if(c < 0x10000)
		{
			out[o++] = (char)(0xe0 | c >> 12);
			out[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
			out[o++] = (char)(0x80 | (c & 0x3f));
		}
		else
		{
			out[o++] = (char)(0xf0 | c >> 18);
			out[o++] = (char)(0x80 | ((c >> 12) & 0x3f));
			out[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
			out[o++] = (char)(0x80 | (c & 0x3f));
		}
	}
	out[o] = 0;
	return o > 0 && strcmp(out, ".") != 0 && strcmp(out, "..") != 0;
}

// read every node after the main one; 0 ok, -1 on a broken tree
static int ReadTree(EvbReader_t* r, uint32_t rootChildren, uint64_t dataStart)
{
	uint64_t data = dataStart;
	uint64_t expected = rootChildren, seen = 0;
	while(seen < expected)
	{
		EvbNode_t* n;
		size_t units = 0;
		const uint8_t* name;
		if(r->pos + 16 > r->size)
			return Fail(r, "EVB tree truncated");
		if(r->count == r->cap)
		{
			size_t cap = r->cap ? r->cap * 2 : 256;
			EvbNode_t* grown = (EvbNode_t*)realloc(r->nodes, cap * sizeof *grown);
			if(!grown)
				return Fail(r, "out of memory");
			r->nodes = grown;
			r->cap = cap;
		}
		n = &r->nodes[r->count];
		memset(n, 0, sizeof *n);
		n->children = Le32(r->base + r->pos + 12);
		r->pos += 16;
		name = r->base + r->pos;
		while(r->pos + 2 <= r->size && (r->base[r->pos] || r->base[r->pos + 1]))
		{
			r->pos += 2;
			units++;
		}
		if(r->pos + 3 > r->size)
			return Fail(r, "EVB name truncated");
		r->pos += 2;
		n->type = r->base[r->pos++];
		if(n->type == 2)
		{
			if(r->pos + 53 > r->size)
				return Fail(r, "EVB file node truncated");
			n->original = Le32(r->base + r->pos + 2);
			n->stored = Le32(r->base + r->pos + 49);
			r->pos += 53;
			n->offset = data;
			data += n->stored;
			if(data > r->size)
				return Fail(r, "EVB file data beyond the end");
		}
		else if(n->type == 3)
		{
			if(r->pos + 25 > r->size)
				return Fail(r, "EVB folder node truncated");
			r->pos += 25;
			expected += n->children;
		}
		else
			return Fail(r, "unknown EVB node type");
		if(!NameToUtf8(name, units, n->name, sizeof n->name))
		{
			if(n->type == 3 && units == 0)
				n->name[0] = 0;
			else
				return Fail(r, "unsafe EVB file name");
		}
		if(n->type == 3 && strcmp(n->name, "%DEFAULT FOLDER%") == 0)
			n->name[0] = 0;
		r->count++;
		seen++;
	}
	return 0;
}

// ---- writing ---------------------------------------------------------------------------------

typedef struct EvbWriter
{
	EvbReader_t* r;
	EvbProgress_t progress;
	void* user;
	uint64_t done, total;
	int files;
} EvbWriter_t;

static int MakeDirs(const char* path)
{
	char tmp[2048];
	size_t i;
	snprintf(tmp, sizeof tmp, "%s", path);
	for(i = 1; tmp[i]; i++)
		if(tmp[i] == '/')
		{
			tmp[i] = 0;
			if(mkdir(tmp, 0755) != 0 && errno != EEXIST)
				return 0;
			tmp[i] = '/';
		}
	return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static int WriteFile(EvbWriter_t* w, const EvbNode_t* n, const char* path)
{
	EvbReader_t* r = w->r;
	const uint8_t* p = r->base + n->offset;
	FILE* f = fopen(path, "wb");
	int ok = 1;
	if(!f)
		return Fail(r, "cannot create a file of the patch");
	if(n->original == n->stored)
		ok = fwrite(p, 1, n->stored, f) == n->stored;
	else
	{
		uint32_t blk, nchunks, i;
		const uint8_t *table, *chunk, *end = p + n->stored;
		uint8_t* out;
		uint64_t written = 0;
		if(n->stored < 8 || (blk = Le32(p)) < 8 || blk > n->stored)
		{
			fclose(f);
			return Fail(r, "broken chunk table");
		}
		table = p + 8;
		nchunks = ((blk - 8) / 4 + 2) / 3; // 12-byte records, the last one with its size alone
		chunk = p + blk;
		out = (uint8_t*)malloc(n->original ? n->original : 1);
		if(!out)
		{
			fclose(f);
			return Fail(r, "out of memory");
		}
		for(i = 0; i < nchunks && chunk < end; i++)
		{
			uint32_t csize = Le32(table + i * 12);
			long got;
			if(csize > (size_t)(end - chunk))
				csize = (uint32_t)(end - chunk);
			got = Evb_Depack(chunk, csize, out + written, n->original - written);
			if(got < 0)
			{
				ok = 0;
				break;
			}
			written += (uint64_t)got;
			chunk += csize;
		}
		if(ok && written != n->original)
			ok = 0;
		if(ok)
			ok = fwrite(out, 1, n->original, f) == n->original;
		free(out);
	}
	if(fclose(f) != 0)
		ok = 0;
	if(!ok)
		return Fail(r, "a file of the patch did not decompress");
	w->done += n->original;
	w->files++;
	if(w->progress && !w->progress(w->user, w->done, w->total))
		return Fail(r, "cancelled");
	return 0;
}

// write node *idx (and, for a folder, its children) under dir; advances *idx
static int WriteNode(EvbWriter_t* w, size_t* idx, const char* dir)
{
	EvbReader_t* r = w->r;
	const EvbNode_t* n;
	char path[2048];
	if(*idx >= r->count)
		return Fail(r, "EVB tree ends early");
	n = &r->nodes[(*idx)++];
	if(n->name[0])
		snprintf(path, sizeof path, "%s/%s", dir, n->name);
	else
		snprintf(path, sizeof path, "%s", dir);
	if(n->type == 2)
		return WriteFile(w, n, path);
	if(!MakeDirs(path))
		return Fail(r, "cannot create a folder of the patch");
	{
		uint32_t c;
		for(c = 0; c < n->children; c++)
			if(WriteNode(w, idx, path) != 0)
				return -1;
	}
	return 0;
}

int Evb_Extract(const char* exePath, const char* outDir, EvbProgress_t progress, void* user, char* err, size_t errLen)
{
	EvbReader_t r;
	EvbWriter_t w;
	struct stat st;
	const uint8_t* magic;
	uint32_t mainSize, rootChildren, c;
	size_t idx = 0, i;
	int fd, result = -1;
	memset(&r, 0, sizeof r);
	r.err = err;
	r.errLen = errLen;
	fd = open(exePath, O_RDONLY);
	if(fd < 0 || fstat(fd, &st) != 0 || st.st_size < 128)
	{
		if(fd >= 0)
			close(fd);
		return Fail(&r, "cannot open the patch executable");
	}
	r.size = (size_t)st.st_size;
	r.base = (const uint8_t*)mmap(NULL, r.size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if(r.base == MAP_FAILED)
		return Fail(&r, "cannot map the patch executable");
	magic = (const uint8_t*)memmem(r.base, r.size, "EVB\0", 4);
	if(!magic || (size_t)(magic - r.base) + 64 + 16 > r.size)
	{
		Fail(&r, "no Enigma Virtual Box file system in the executable");
		goto done;
	}
	r.pos = (size_t)(magic - r.base) + 64;
	mainSize = Le32(r.base + r.pos);
	rootChildren = Le32(r.base + r.pos + 12);
	r.pos += 16;
	{
		// the file data is counted from the end of the main node; the first
		// node of the tree starts on the main node's last byte
		uint64_t dataStart = (uint64_t)r.pos + mainSize - 12;
		r.pos -= 1;
		if(ReadTree(&r, rootChildren, dataStart) != 0)
			goto done;
	}
	if(r.count == 0)
	{
		Fail(&r, "empty EVB tree");
		goto done;
	}
	memset(&w, 0, sizeof w);
	w.r = &r;
	w.progress = progress;
	w.user = user;
	for(i = 0; i < r.count; i++)
		if(r.nodes[i].type == 2)
			w.total += r.nodes[i].original;
	if(!MakeDirs(outDir))
	{
		Fail(&r, "cannot create the output folder");
		goto done;
	}
	for(c = 0; c < rootChildren; c++)
		if(WriteNode(&w, &idx, outDir) != 0)
			goto done;
	result = w.files;
done:
	free(r.nodes);
	munmap((void*)r.base, r.size);
	return result;
}
