/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * strlit.c - quoted string literals, shared by the assembler, the
 *            disassembler and the scenario tools (declared in asm.h)
 *
 * A literal holds Shift-JIS bytes written as UTF-8 text with C-like
 * escapes.  Asm_WriteStringLiteral chooses between UTF-8 and \xNN bytes so
 * that the reader gets the original bytes back exactly, whatever CP932's
 * duplicate codes do to the conversion; Asm_ReadStringLiteral is the
 * reader.
 */
#include "bgi/asm.h"
#include "bgi/os.h"

#include <ctype.h>

/* Write `n` bytes of Shift-JIS as a quoted literal the reader turns back
 * into the same bytes: in UTF-8 when the conversion there and back is
 * exact (CP932 has duplicate codes for a few characters, and an
 * unmappable one is substituted), else every non-ASCII byte as \xNN;
 * quotes, backslashes and control characters are escaped either way. */
void Asm_WriteStringLiteral(FILE* out, const uint8_t* s, uint32_t n)
{
	char* sjis = (char*)malloc(n + 1);
	char* utf = (char*)malloc(n * 3 + 1);
	char* back = (char*)malloc(n * 2 + 2);
	uint32_t i;
	int exact;
	memcpy(sjis, s, n);
	sjis[n] = 0;
	OS_SjisToUtf8(sjis, utf, n * 3 + 1);
	OS_Utf8ToSjis(utf, back, n * 2 + 2);
	// a '?' in the UTF-8 that the source does not have is a substituted character
	exact = strlen(back) == n && memcmp(back, sjis, n) == 0 && !strchr(utf, '?') == !memchr(s, '?', n);
	fputc('"', out);
	if(exact)
	{
		const uint8_t* u = (const uint8_t*)utf;
		for(; *u; u++)
		{
			if(*u == '"' || *u == '\\')
				fprintf(out, "\\%c", *u);
			else if(*u == '\n')
				fputs("\\n", out);
			else if(*u == '\r')
				fputs("\\r", out);
			else if(*u == '\t')
				fputs("\\t", out);
			else if(*u < 0x20)
				fprintf(out, "\\x%02x", *u);
			else
				fputc(*u, out);
		}
	}
	else
	{
		for(i = 0; i < n; i++)
		{
			uint8_t c = s[i];
			if(c == '"' || c == '\\')
				fprintf(out, "\\%c", c);
			else if(c >= 0x20 && c < 0x7f)
				fputc(c, out);
			else
				fprintf(out, "\\x%02x", c);
		}
	}
	fputc('"', out);
	free(sjis);
	free(utf);
	free(back);
}

/* Read the literal that starts at `p` (the opening quote) and ends before
 * `end`: UTF-8 text becomes Shift-JIS, \xNN inserts a byte, \0 a NUL,
 * \n \r \t \\ \" as usual, any other escaped character itself.  The bytes
 * go to a malloc'ed buffer in `*outBytes` (no NUL added), `*outLen` is
 * their count and `*next` points after the closing quote.  Returns NULL on
 * success, else a message ("unterminated string", ..) with nothing
 * allocated. */
const char* Asm_ReadStringLiteral(const char* p, const char* end, uint8_t** outBytes, uint32_t* outLen, const char** next)
{
	size_t cap = (size_t)(end - p) * 2 + 16;
	uint8_t* buf = (uint8_t*)malloc(cap);
	char* run = (char*)malloc(cap);
	uint32_t n = 0, r = 0;
	if(p >= end || *p != '"')
	{
		free(buf);
		free(run);
		return "a string literal was expected";
	}
	p++;
	for(;;)
	{
		char c;
		if(p >= end)
		{
			free(buf);
			free(run);
			return "unterminated string";
		}
		c = *p++;
		if(c == '"' || c == '\\')
		{
			// a run of text ends here: convert it to Shift-JIS as a whole
			if(r)
			{
				char* sj = (char*)malloc(cap);
				size_t m;
				run[r] = 0;
				m = OS_Utf8ToSjis(run, sj, cap);
				memcpy(buf + n, sj, m);
				n += (uint32_t)m;
				r = 0;
				free(sj);
			}
			if(c == '"')
				break;
			if(p >= end)
			{
				free(buf);
				free(run);
				return "bad escape";
			}
			c = *p++;
			switch(c)
			{
				case 'n': buf[n++] = '\n'; break;
				case 'r': buf[n++] = '\r'; break;
				case 't': buf[n++] = '\t'; break;
				case '0': buf[n++] = 0; break;
				case 'x':
				{
					char hex[3];
					if(p + 2 > end || !isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1]))
					{
						free(buf);
						free(run);
						return "\\x needs two hex digits";
					}
					hex[0] = p[0];
					hex[1] = p[1];
					hex[2] = 0;
					buf[n++] = (uint8_t)strtol(hex, NULL, 16);
					p += 2;
					break;
				}
				default: buf[n++] = (uint8_t)c; break;
			}
			continue;
		}
		run[r++] = c;
	}
	free(run);
	*outBytes = buf;
	*outLen = n;
	*next = p;
	return NULL;
}
