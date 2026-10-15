/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/bpc.c - unit test of the program decompiler and compiler
 *               (src/bpc/, declared in inc/bgi/bpc.h); built as
 *               bin/test_bpc by `make test`
 *
 * The groups of tests, in the order they run:
 *
 *   shapes     - sources compile to the bytes the original compiler's
 *                shapes give (docs/bpc.md): the frame and the parameters,
 *                the argument orders, the two stores, the condition chains
 *                and the statements, the sign test, the wide literal, the
 *                string table and its padding
 *   round trip - the compiled programs decompile to source that compiles
 *                to the same bytes, and the source names the constructs it
 *                should (if / else / while / do / goto / __sign / __pop)
 *   errors     - texts the compiler must reject
 *
 * The expected bytes come from listings assembled with Asm_Assemble, so
 * that labels and encodings need not be worked out by hand.
 */
#include "bgi/bpc.h"
#include "bgi/os.h"

#include <stdio.h>

static int gFails; // the number of failed checks so far
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

#define GEN GEN_1_553

// compile a source; NULL (with the messages on stdout) when it does not compile
static uint8_t* Compile(const char* src, uint32_t* size)
{
	BpcResult_t r;
	Bpc_Compile(src, strlen(src), "test", GEN, stdout, &r);
	BGI_Free(r.labels);
	if(r.errors)
		return NULL;
	*size = r.size;
	return r.file;
}

// assemble a listing; NULL when it does not assemble
static uint8_t* Assemble(const char* listing, uint32_t* size)
{
	AsmResult_t r;
	Asm_Assemble(listing, strlen(listing), GEN, stdout, &r);
	BGI_Free(r.labels);
	if(r.errors)
		return NULL;
	*size = r.size;
	return r.file;
}

// decompile a program into a malloc'ed string
static char* Decompile(const uint8_t* file, uint32_t size, int* blocks)
{
	BpcDecOptions_t opt;
	FILE* mem = tmpfile();
	long n;
	char* text;
	memset(&opt, 0, sizeof opt);
	opt.gen = GEN;
	opt.name = "test";
	*blocks = Bpc_Decompile(file, size, &opt, mem);
	n = ftell(mem);
	fseek(mem, 0, SEEK_SET);
	text = (char*)malloc((size_t)n + 1);
	if(fread(text, 1, (size_t)n, mem) != (size_t)n)
		n = 0;
	text[n] = 0;
	fclose(mem);
	return text;
}

static void Same(const char* what, const uint8_t* a, uint32_t an, const uint8_t* b, uint32_t bn)
{
	if(!a || !b)
	{
		printf("  FAIL %s: not produced\n", what);
		gFails++;
		return;
	}
	if(an != bn || memcmp(a, b, an) != 0)
	{
		uint32_t i;
		for(i = 0; i < an && i < bn && a[i] == b[i]; i++)
			;
		printf("  FAIL %s: %u / %u bytes, first difference at 0x%x\n", what, (unsigned)an, (unsigned)bn, (unsigned)i);
		gFails++;
	}
}

/* compile `src`, check it against the assembled `listing`, then round
 * trip it through the decompiler; returns the decompiled text (malloc'ed)
 * so that the caller can look for its constructs */
static char* Shape(const char* what, const char* src, const char* listing)
{
	uint32_t cn = 0, an = 0, rn = 0;
	uint8_t* c = Compile(src, &cn);
	uint8_t* a = Assemble(listing, &an);
	char* text = NULL;
	Same(what, c, cn, a, an);
	if(c)
	{
		int blocks = -1;
		uint8_t* r;
		text = Decompile(c, cn, &blocks);
		CHECK(blocks == 0);
		r = Compile(text, &rn);
		Same(what, c, cn, r, rn);
		BGI_Free(r);
	}
	BGI_Free(c);
	BGI_Free(a);
	return text;
}

// ---- shapes ----------------------------------------------------------------------------------------

static void TestFrame(void)
{
	char* t = Shape("frame",
		"#engine 1.553\n"
		"main(p1, p2) { int a; char buf[8]; int b = p1;\n"
		"    a = p1 + p2;\n"
		"    *(int*)1208 = a;\n"
		"    return a;\n"
		"}\n",
		".engine 1.553\n"
		"main:\n"
		"\tpush_fp\n\tpush_i8 24\n\tadd\n\tset_fp\n"
		"\tpush_local_addr 0x14\n\tstore 2\n"                                  // p1, the higher of the two parameter slots
		"\tpush_local_addr 0x18\n\tstore 2\n"                                  // p2 at the frame's bottom
		"\tpush_local_addr 0x14\n\tload 2\n\tpush_local_addr 0x4\n\tstore 2\n" // int b = p1: value, address, store
		"\tpush_local_addr 0x10\n\tpush_local_addr 0x14\n\tload 2\n\tpush_local_addr 0x18\n\tload 2\n\tadd\n\tstore_keep 2\n"
		"\tpush_i16 1208\n\tpush_local_addr 0x10\n\tload 2\n\tstore_keep 2\n"
		"\tpush_local_addr 0x10\n\tload 2\n\tpush_code_off EXIT\n\tjmp\n"
		"EXIT:\n\tpush_fp\n\tpush_i8 24\n\tsub\n\tset_fp\n\tret\n"
		"\t.align 16\n");
	if(t)
	{
		CHECK(strstr(t, "main(p1, p2)") != NULL);
		CHECK(strstr(t, "char l") != NULL); // the array nothing accesses
		CHECK(strstr(t, "return ") != NULL);
		free(t);
	}
}

static void TestConditions(void)
{
	char* t = Shape("conditions",
		"#engine 1.553\n"
		"main(p1, p2) {\n"
		"    if (p1 && p2) { sys.yield(); } else { sys.quit(); }\n"
		"    if (p1 || p2) return;\n"
		"    while (p1 < 10) { p1 = p1 + 1; if (p2) break; }\n"
		"    do { p2 = p2 - 1; } while (p2);\n"
		"    if (__sign(p1 - 4) < 0) { sys.yield(); }\n"
		"    if (!(p1 == 3)) {}\n"
		"}\n",
		".engine 1.553\n"
		"main:\n"
		"\tpush_fp\n\tpush_i8 8\n\tadd\n\tset_fp\n"
		"\tpush_local_addr 0x4\n\tstore 2\n\tpush_local_addr 0x8\n\tstore 2\n"
		// if (p1 && p2): both false jumps go to the else part
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_code_off ELSE\n\tjcc 1\n"
		"\tpush_local_addr 0x8\n\tload 2\n\tpush_code_off ELSE\n\tjcc 1\n"
		"\tsys.yield\n\tpush_code_off END\n\tjmp\n"
		"ELSE:\n\tsys.quit\n"
		"END:\n"
		// if (p1 || p2) return: the direct form, true jumps to the exit
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_code_off EXIT\n\tjcc 0\n"
		"\tpush_local_addr 0x8\n\tload 2\n\tpush_code_off EXIT\n\tjcc 0\n"
		// while
		"HEAD:\n\tpush_local_addr 0x4\n\tload 2\n\tpush_i8 10\n\tlt\n\tpush_code_off WEND\n\tjcc 1\n"
		"\tpush_local_addr 0x4\n\tpush_local_addr 0x4\n\tload 2\n\tpush_i8 1\n\tadd\n\tstore_keep 2\n"
		"\tpush_local_addr 0x8\n\tload 2\n\tpush_code_off WEND\n\tjcc 0\n"
		"\tpush_code_off HEAD\n\tjmp\n"
		"WEND:\n"
		// do .. while
		"DHEAD:\n\tpush_local_addr 0x8\n\tpush_local_addr 0x8\n\tload 2\n\tpush_i8 1\n\tsub\n\tstore_keep 2\n"
		"\tpush_local_addr 0x8\n\tload 2\n\tpush_code_off DHEAD\n\tjcc 0\n"
		// __sign(p1 - 4) < 0: the jump tests the sign (jcc 3: >= 0 goes past the then part)
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_i8 4\n\tsub\n\tpush_code_off ELSE2\n\tjcc 3\n"
		"\tsys.yield\n"
		"ELSE2:\n"
		// an empty then part: a jump to the next instruction
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_i8 3\n\teq\n\tpush_code_off ELSE3\n\tjcc 0\n"
		"ELSE3:\n"
		"EXIT:\n\tpush_fp\n\tpush_i8 8\n\tsub\n\tset_fp\n\tret\n"
		"\t.align 16\n");
	if(t)
	{
		CHECK(strstr(t, "if (p1 && p2) {") != NULL);
		CHECK(strstr(t, "} else {") != NULL);
		CHECK(strstr(t, "if (p1 || p2) return;") != NULL);
		CHECK(strstr(t, "while (p1 < 10) {") != NULL);
		CHECK(strstr(t, "if (p2) break;") != NULL);
		CHECK(strstr(t, "} while (p2);") != NULL);
		CHECK(strstr(t, "__sign(p1 - 4) < 0") != NULL);
		CHECK(strstr(t, "if (!(p1 == 3)) {}") != NULL);
		free(t);
	}
}

static void TestValues(void)
{
	char* t = Shape("values",
		"#engine 1.553\n"
		"main(p1) { int t[2]; int r;\n"
		"    *(int*)t = {1, 2};\n"
		"    r = sub(p1, 2);\n"
		"    *(char*)(p1 + 1) <- __pop;\n"
		"    sprintf(t, \"%d %d\", p1, r);\n"
		"    r = p1 ? -1 : 0x00000000;\n"
		"    r = (p1 > 0) && (r != 0);\n"
		"    (*(int*)(t + 4))(3);\n"
		"    gfx0.sprite_show(r, 1);\n"
		"    __inline(t, \"\\x01\\x02\");\n"
		"    debug_msg(\"%d %d\");\n"
		"    return r >>> 2;\n"
		"}\n"
		"sub(p1, p2) { return p1 * p2; }\n",
		".engine 1.553\n"
		"main:\n"
		"\tpush_fp\n\tpush_i8 16\n\tadd\n\tset_fp\n\tpush_local_addr 0x10\n\tstore 2\n"
		"\tpush_local_addr 0xc\n\tpush_i8 1\n\tpush_i8 2\n\tstore_multi 2, 2\n"                                // *(int*)t = {1, 2}
		"\tpush_i8 2\n\tpush_local_addr 0x10\n\tload 2\n\tpush_code_off sub\n\tcall\n"                         // the arguments right to left
		"\tpush_local_addr 0x4\n\tstore 2\n"                                                                   // a call's value: value, address, store
		"\tpush_local_addr 0x10\n\tload 2\n\tpush_i8 1\n\tadd\n\tstore 0\n"                                    // <- __pop: nothing pushed
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_local_addr 0x10\n\tload 2\n"                                  // sprintf: conversions right to left,
		"\tpush_local_addr 0xc\n\tpush_code_addr str_0\n\tsprintf\n"                                           // then the destination and the format
		"\tpush_local_addr 0x10\n\tload 2\n\tpush_i8 1\n\tnot\n\tpush_i8 1\n\tadd\n\tpush_i32 0x0\n\tselect\n" // -1, a wide 0
		"\tpush_local_addr 0x4\n\tstore 2\n"                                                                   // select stores like a call
		"\tpush_local_addr 0x4\n\tpush_local_addr 0x10\n\tload 2\n\tpush_i8 0\n\tgt\n"
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_i8 0\n\tne\n\tland\n\tstore_keep 2\n" // parentheses make values
		"\tpush_i8 3\n\tpush_local_addr 0xc\n\tpush_i8 4\n\tadd\n\tload 2\n\tcall\n"   // a call through a value
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_i8 1\n\tgfx0.sprite_show\n"           // an instruction: left to right
		"\tpush_local_addr 0xc\n\tstore_inline 0x01, 0x02\n"
		"\tpush_code_addr str_1\n\tdebug_msg\n" // the same literal again is another copy
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_i8 2\n\tshr\n\tpush_code_off EXIT\n\tjmp\n"
		"EXIT:\n\tpush_fp\n\tpush_i8 16\n\tsub\n\tset_fp\n\tret\n"
		"sub:\n"
		"\tpush_fp\n\tpush_i8 8\n\tadd\n\tset_fp\n\tpush_local_addr 0x4\n\tstore 2\n\tpush_local_addr 0x8\n\tstore 2\n"
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_local_addr 0x8\n\tload 2\n\tmul\n\tpush_code_off EXIT2\n\tjmp\n"
		"EXIT2:\n\tpush_fp\n\tpush_i8 8\n\tsub\n\tset_fp\n\tret\n"
		"\t.align 16\n"
		"str_0:\n\t.str \"%d %d\"\n"
		"str_1:\n\t.str \"%d %d\"\n"
		"\t.align 16\n");
	if(t)
	{
		CHECK(strstr(t, "= sub_") != NULL || strstr(t, "= sub(") != NULL);
		CHECK(strstr(t, "<- __pop;") != NULL);
		CHECK(strstr(t, "sprintf(") != NULL);
		CHECK(strstr(t, "? -1 : 0x00000000") != NULL);
		CHECK(strstr(t, "__inline(") != NULL);
		CHECK(strstr(t, ">>> 2") != NULL);
		CHECK(strstr(t, "= {1, 2};") != NULL);
		free(t);
	}
}

static void TestJumps(void)
{
	char* t = Shape("jumps",
		"#engine 1.553\n"
		"main(p1) { int i;\n"
		"    i = 0;\n"
		"    for (;;) {\n"
		"        if (p1 == 1) goto done;\n"
		"        if (p1 == 2) goto done;\n"
		"        i = i + 1;\n"
		"        if (i > 3) continue;\n"
		"        sys.yield();\n"
		"    }\n"
		"done:\n"
		"    i;\n"
		"}\n",
		".engine 1.553\n"
		"main:\n"
		"\tpush_fp\n\tpush_i8 8\n\tadd\n\tset_fp\n\tpush_local_addr 0x8\n\tstore 2\n"
		"\tpush_local_addr 0x4\n\tpush_i8 0\n\tstore_keep 2\n"
		"HEAD:\n"
		"\tpush_local_addr 0x8\n\tload 2\n\tpush_i8 1\n\teq\n\tpush_code_off DONE\n\tjcc 0\n"
		"\tpush_local_addr 0x8\n\tload 2\n\tpush_i8 2\n\teq\n\tpush_code_off DONE\n\tjcc 0\n"
		"\tpush_local_addr 0x4\n\tpush_local_addr 0x4\n\tload 2\n\tpush_i8 1\n\tadd\n\tstore_keep 2\n"
		"\tpush_local_addr 0x4\n\tload 2\n\tpush_i8 3\n\tgt\n\tpush_code_off HEAD\n\tjcc 0\n"
		"\tsys.yield\n"
		"\tpush_code_off HEAD\n\tjmp\n"
		"DONE:\n"
		"\tpush_local_addr 0x4\n\tload 2\n"
		"\tpush_fp\n\tpush_i8 8\n\tsub\n\tset_fp\n\tret\n"
		"\t.align 16\n");
	if(t)
	{
		CHECK(strstr(t, "while (!(p1 == 1) && !(p1 == 2)) {") != NULL); // the same bytes as the loop with the gotos
		CHECK(strstr(t, "continue;") != NULL);
		free(t);
	}
}

// ---- errors ----------------------------------------------------------------------------------------

static void TestErrors(void)
{
	static const char* const bad[] = {
		"main() { x = 1; }",                    // an unknown variable
		"main() { break; }",                    // break outside a loop
		"main() { if (p1) }",                   // a missing statement
		"main() { int a; a = __sign(a) + 1; }", // __sign outside a condition
		"main() { sub(); }",                    // an unknown function
		"main() { \"unterminated; }",           // a bad string literal
	};
	size_t i;
	FILE* null = tmpfile();
	for(i = 0; i < sizeof bad / sizeof bad[0]; i++)
	{
		BpcResult_t r;
		Bpc_Compile(bad[i], strlen(bad[i]), "bad", GEN, null ? null : stdout, &r);
		CHECK(r.errors > 0);
		CHECK(r.file == NULL);
		BGI_Free(r.labels);
	}
	if(null)
		fclose(null);
	{ // a file without a usable header
		uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
		BpcDecOptions_t opt;
		FILE* mem = tmpfile();
		memset(&opt, 0, sizeof opt);
		opt.gen = GEN;
		CHECK(Bpc_Decompile(junk, sizeof junk, &opt, mem ? mem : stdout) == -1);
		if(mem)
			fclose(mem);
	}
}

int main(void)
{
	printf("bpc: shapes\n");
	TestFrame();
	TestConditions();
	TestValues();
	TestJumps();
	printf("bpc: errors\n");
	TestErrors();
	if(gFails)
	{
		printf("bpc: %d check(s) failed\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
