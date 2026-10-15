/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bpcomp.c - the program compiler: the C-like source (docs/bpc.md) to a
 *            program file (interface in bpc.h, the code shapes in
 *            bpc_internal.h)
 *
 * A recursive-descent parser builds a tree of the program; the generator
 * then writes an assembler listing (docs/asm.md) the way the original
 * compiler laid the code out - the frame prologue and the parameter
 * pops, every statement's value left on the stack, the arguments of a
 * function right to left and of an instruction left to right, conditions
 * as chains of conditional jumps, one exit per function, the string
 * literals after the code in order of appearance - and Asm_Assemble
 * turns the listing into the file.  The decompiler (bpdec.c) relies on
 * these rules to reproduce a program exactly.
 */
#include "bpc_internal.h"

#include <ctype.h>
#include <stdarg.h>

// ---- the lexer ------------------------------------------------------------------------------------

enum TokKind
{
	T_EOF,
	T_IDENT,
	T_NUM,
	T_STR,
	T_PUNCT,
	T_DIRECTIVE
};

typedef struct Token
{
	int kind;
	int line;
	char text[128];
	char arg[64]; // T_DIRECTIVE: the rest of the line for #engine
	int64_t num;
	int wide;       // T_NUM: written with eight hex digits, which the compiler pushed as 32 bits whatever the value
	uint8_t* bytes; // T_STR: the bytes (Shift-JIS), freed by whoever consumes them
	uint32_t len;
} Token_t;

typedef struct Lex
{
	const char* p;
	const char* end;
	int line;
	int errors, warnings;
	FILE* log;
	const char* fileName;
	Token_t tok;
	int peeked;
	Token_t peek;
} Lex_t;

static void Errorf(Lex_t* l, int line, const char* fmt, ...)
{
	va_list ap;
	l->errors++;
	if(l->errors > 50)
		return;
	fprintf(l->log, "%s:%d: error: ", l->fileName, line);
	va_start(ap, fmt);
	vfprintf(l->log, fmt, ap);
	va_end(ap);
	fputc('\n', l->log);
}

static int IsIdentStart(int c)
{
	return isalpha(c) || c == '_' || c >= 0x80;
}

static int IsIdentChar(int c)
{
	return isalnum(c) || c == '_' || c >= 0x80;
}

static void SkipSpace(Lex_t* l)
{
	for(;;)
	{
		while(l->p < l->end && isspace((unsigned char)*l->p))
		{
			if(*l->p == '\n')
				l->line++;
			l->p++;
		}
		if(l->p + 1 < l->end && l->p[0] == '/' && l->p[1] == '/')
		{
			while(l->p < l->end && *l->p != '\n')
				l->p++;
			continue;
		}
		if(l->p + 1 < l->end && l->p[0] == '/' && l->p[1] == '*')
		{
			l->p += 2;
			while(l->p + 1 < l->end && !(l->p[0] == '*' && l->p[1] == '/'))
			{
				if(*l->p == '\n')
					l->line++;
				l->p++;
			}
			l->p += 2;
			continue;
		}
		break;
	}
}

static const char* const kPuncts[] = {">>>", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "<-", "(", ")", "{", "}", "[", "]", ",",
	";", ":", "=", "<", ">", "+", "-", "*", "/", "%", "&", "|", "^", "!", "~", "?", NULL};

static void ReadToken(Lex_t* l, Token_t* t)
{
	memset(t, 0, sizeof *t);
	SkipSpace(l);
	t->line = l->line;
	if(l->p >= l->end)
	{
		t->kind = T_EOF;
		return;
	}
	if(*l->p == '#')
	{
		const char* s = ++l->p;
		while(l->p < l->end && IsIdentChar((unsigned char)*l->p))
			l->p++;
		t->kind = T_DIRECTIVE;
		snprintf(t->text, sizeof t->text, "%.*s", (int)(l->p - s), s);
		if(strcmp(t->text, "engine") == 0)
		{ // the engine name: the rest of the line (it has dots and slashes)
			while(l->p < l->end && (*l->p == ' ' || *l->p == '\t'))
				l->p++;
			s = l->p;
			while(l->p < l->end && !isspace((unsigned char)*l->p))
				l->p++;
			snprintf(t->arg, sizeof t->arg, "%.*s", (int)(l->p - s), s);
		}
		return;
	}
	if(*l->p == '"')
	{
		const char* next;
		const char* err = Asm_ReadStringLiteral(l->p, l->end, &t->bytes, &t->len, &next);
		if(err)
		{
			Errorf(l, l->line, "%s", err);
			t->kind = T_EOF;
			l->p = l->end;
			return;
		}
		{
			const char* q;
			for(q = l->p; q < next; q++)
				if(*q == '\n')
					l->line++;
		}
		l->p = next;
		t->kind = T_STR;
		return;
	}
	if(*l->p == '\'' && l->p + 2 < l->end && l->p[2] == '\'')
	{
		t->kind = T_NUM;
		t->num = (uint8_t)l->p[1];
		l->p += 3;
		return;
	}
	if(isdigit((unsigned char)*l->p))
	{
		char* e;
		t->kind = T_NUM;
		t->wide = 0;
		if(l->p[0] == '0' && (l->p[1] == 'x' || l->p[1] == 'X'))
		{
			t->num = (int64_t)strtoull(l->p, &e, 16);
			t->wide = e - l->p == 10; // 0x00000000: eight digits
		}
		else
			t->num = (int64_t)strtoull(l->p, &e, 10);
		l->p = e;
		return;
	}
	if(IsIdentStart((unsigned char)*l->p))
	{
		const char* s = l->p;
		while(l->p < l->end && IsIdentChar((unsigned char)*l->p))
			l->p++;
		// family.name / family.0xNN: one token when the prefix names an instruction family (`op` the base set)
		if(l->p < l->end && *l->p == '.' && (Asm_FamilyByte(s, (size_t)(l->p - s)) >= 0 || (l->p - s == 2 && memcmp(s, "op", 2) == 0)))
		{
			l->p++;
			while(l->p < l->end && IsIdentChar((unsigned char)*l->p))
				l->p++;
		}
		t->kind = T_IDENT;
		snprintf(t->text, sizeof t->text, "%.*s", (int)(l->p - s), s);
		return;
	}
	{
		int i;
		for(i = 0; kPuncts[i]; i++)
		{
			size_t n = strlen(kPuncts[i]);
			if((size_t)(l->end - l->p) >= n && memcmp(l->p, kPuncts[i], n) == 0)
			{
				t->kind = T_PUNCT;
				memcpy(t->text, kPuncts[i], n + 1);
				l->p += n;
				return;
			}
		}
	}
	Errorf(l, l->line, "unexpected character '%c'", *l->p);
	l->p++;
	ReadToken(l, t);
}

static void Next(Lex_t* l)
{
	if(l->peeked)
	{
		l->tok = l->peek;
		l->peeked = 0;
	}
	else
		ReadToken(l, &l->tok);
}

static Token_t* Peek(Lex_t* l)
{
	if(!l->peeked)
	{
		ReadToken(l, &l->peek);
		l->peeked = 1;
	}
	return &l->peek;
}

static int IsPunct(const Lex_t* l, const char* p)
{
	return l->tok.kind == T_PUNCT && strcmp(l->tok.text, p) == 0;
}

static int IsIdent(const Lex_t* l, const char* name)
{
	return l->tok.kind == T_IDENT && strcmp(l->tok.text, name) == 0;
}

static int Accept(Lex_t* l, const char* p)
{
	if(IsPunct(l, p))
	{
		Next(l);
		return 1;
	}
	return 0;
}

static void Expect(Lex_t* l, const char* p)
{
	if(!Accept(l, p))
	{
		Errorf(l, l->tok.line, "'%s' expected, found '%s'", p, l->tok.kind == T_EOF ? "end of file" : l->tok.text);
		// resynchronize a little
		if(l->tok.kind != T_EOF)
			Next(l);
	}
}

// ---- the tree ---------------------------------------------------------------------------------------

enum NodeKind
{
	N_NUM,
	N_STR,      // bytes, len
	N_NAME,     // text
	N_CALL,     // a: the callee (N_NAME of a function, or any expression); items: the arguments
	N_OPCALL,   // op (fam << 8 | opcode); items: the arguments
	N_UNARY,    // op: '-', '~', '!'; a
	N_ADDR,     // &name: text
	N_DEREF,    // *(T*)(a): size (0 / 1 / 2)
	N_BINARY,   // op: the opcode; a, b
	N_SELECT,   // a ? b : c
	N_ASSIGN,   // a = b (store_keep, or store when b is a call)
	N_ASSIGN2,  // a <- b (store)
	N_PAREN,    // (a): a value in a condition
	N_INITLIST, // {items}
	// statements
	N_EXPR,
	N_DECL,    // text, size (element), op (elements), num (1: an array), a: the initializer or NULL
	N_BLOCK,   // items
	N_IF,      // a: cond, b: then, c: else (may be NULL); num: 1 when the then part had no braces
	N_WHILE,   // a: cond (NULL: for (;;)), b: body
	N_DOWHILE, // a: cond, b: body
	N_FOR,     // a: init, b: cond, c: incr (statements / expressions, NULL when empty), items[0]: body
	N_BREAK,
	N_CONTINUE,
	N_RETURN,  // a: the value or NULL
	N_GOTO,    // text
	N_LABEL,   // text
	N_ASM,     // text: the lines
	N_INLINE,  // a: the address, bytes: the literal
	N_STOREMUL // a: the address, size, items: the values
};

typedef struct Node Node_t;
struct Node
{
	int kind;
	int line;
	int op;
	int size;
	int count, cap;
	int64_t num;
	Node_t* a;
	Node_t* b;
	Node_t* c;
	Node_t** items;
	char text[128];
	uint8_t* bytes;
	uint32_t len;
};

static Node_t* NewNode(int kind, int line)
{
	Node_t* n = (Node_t*)calloc(1, sizeof *n);
	n->kind = kind;
	n->line = line;
	return n;
}

static void AddItem(Node_t* n, Node_t* item)
{
	if(n->count == n->cap)
	{
		n->cap = n->cap ? n->cap * 2 : 4;
		n->items = (Node_t**)realloc(n->items, (size_t)n->cap * sizeof *n->items);
	}
	n->items[n->count++] = item;
}

static void FreeNode(Node_t* n)
{
	int i;
	if(!n)
		return;
	FreeNode(n->a);
	FreeNode(n->b);
	FreeNode(n->c);
	for(i = 0; i < n->count; i++)
		FreeNode(n->items[i]);
	free(n->items);
	free(n->bytes);
	free(n);
}

typedef struct Var
{
	char name[128];
	int elem;      // 1, 2, 4
	uint32_t size; // bytes
	int isArray;
	uint32_t off; // the frame offset from the frame's lowest address
} Var_t;

typedef struct Func
{
	char name[128];
	int params;
	Var_t* vars; // the parameters first, then the locals in declaration order
	int varCount, varCap;
	uint32_t frame;
	Node_t* body; // N_BLOCK
	int isAsm;    // the body is a single __asm block
	Node_t* asmNode;
} Func_t;

typedef struct Unit
{
	EngineGen_t gen;
	int genSet;
	Func_t** funcs;
	int funcCount, funcCap;
	char* program; // __program { listing }: the whole file as a listing
} Unit_t;

typedef struct Parser
{
	Lex_t lex;
	Unit_t* unit;
	Func_t* func;
} Parser_t;

// ---- declarations ------------------------------------------------------------------------------------

static Var_t* FindVar(Func_t* f, const char* name)
{
	int i;
	if(!f)
		return NULL;
	for(i = 0; i < f->varCount; i++)
		if(strcmp(f->vars[i].name, name) == 0)
			return &f->vars[i];
	return NULL;
}

static Var_t* AddVar(Func_t* f, const char* name, int elem, uint32_t size, int isArray)
{
	Var_t* v;
	if(f->varCount == f->varCap)
	{
		f->varCap = f->varCap ? f->varCap * 2 : 16;
		f->vars = (Var_t*)realloc(f->vars, (size_t)f->varCap * sizeof *f->vars);
	}
	v = &f->vars[f->varCount++];
	memset(v, 0, sizeof *v);
	snprintf(v->name, sizeof v->name, "%s", name);
	v->elem = elem;
	v->size = size;
	v->isArray = isArray;
	v->off = f->frame;
	f->frame += size;
	return v;
}

static Func_t* FindFunc(Unit_t* u, const char* name)
{
	int i;
	for(i = 0; i < u->funcCount; i++)
		if(strcmp(u->funcs[i]->name, name) == 0)
			return u->funcs[i];
	return NULL;
}

static int TypeKeyword(const Lex_t* l, int* elem)
{
	if(IsIdent(l, "int"))
		*elem = 4;
	else if(IsIdent(l, "char"))
		*elem = 1;
	else if(IsIdent(l, "short"))
		*elem = 2;
	else
		return 0;
	return 1;
}

// ---- expressions -------------------------------------------------------------------------------------

static Node_t* ParseExpression(Parser_t* p);
static Node_t* ParseAssignment(Parser_t* p);

static Node_t* StrNode(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Node_t* n = NewNode(N_STR, l->tok.line);
	n->bytes = l->tok.bytes;
	n->len = l->tok.len;
	l->tok.bytes = NULL;
	Next(l);
	return n;
}

static Node_t* ParseArgs(Parser_t* p, Node_t* call)
{
	Lex_t* l = &p->lex;
	Expect(l, "(");
	if(!IsPunct(l, ")"))
		for(;;)
		{
			AddItem(call, ParseAssignment(p));
			if(!Accept(l, ","))
				break;
		}
	Expect(l, ")");
	return call;
}

/* an instruction by its listing name, or `op.0xNN` for a base opcode
 * without a name (the listing writes those bare, which is no identifier) */
static int LookupInstruction(const char* name, EngineGen_t gen, int* fam, int* op)
{
	if(strncmp(name, "op.0x", 5) == 0)
	{
		char* end;
		long v = strtol(name + 5, &end, 16);
		if(*end || v < 0 || v >= 0x80)
			return 0;
		*fam = 0;
		*op = (int)v;
		return 1;
	}
	return Asm_LookupName(name, strlen(name), gen, fam, op);
}

static Node_t* ParsePrimary(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Node_t* n;
	int line = l->tok.line;
	if(l->tok.kind == T_NUM)
	{
		n = NewNode(N_NUM, line);
		n->num = l->tok.num;
		n->size = l->tok.wide ? 4 : 0; // a wide literal
		Next(l);
		return n;
	}
	if(l->tok.kind == T_STR)
		return StrNode(p);
	if(Accept(l, "("))
	{
		int elem;
		// a cast: (int*) / (char*) / (short*) followed by an operand - only in the *(T*)(e) form, handled in unary
		if(TypeKeyword(l, &elem) && Peek(l)->kind == T_PUNCT && strcmp(Peek(l)->text, "*") == 0)
		{
			Errorf(l, line, "a cast is written as *(type*)(address)");
			Next(l);
			Next(l);
			Expect(l, ")");
			return ParsePrimary(p);
		}
		n = NewNode(N_PAREN, line);
		n->a = ParseExpression(p);
		Expect(l, ")");
		return n;
	}
	if(l->tok.kind == T_IDENT)
	{
		char name[128];
		int fam, op;
		strcpy(name, l->tok.text);
		Next(l);
		if(IsPunct(l, "("))
		{
			if(!FindVar(p->func, name) && !FindFunc(p->unit, name) && LookupInstruction(name, p->unit->gen, &fam, &op) && !(fam == 0 && (op >= 0x80 || BpcOp_ByOpcode(op))))
			{ // an instruction (the operators' own opcodes, `add`, `sub`, .. are not callable: they are expressions)
				n = NewNode(N_OPCALL, line);
				n->op = (fam << 8) | op;
				strcpy(n->text, name);
				return ParseArgs(p, n);
			}
			n = NewNode(N_CALL, line);
			n->a = NewNode(N_NAME, line);
			strcpy(n->a->text, name);
			return ParseArgs(p, n);
		}
		n = NewNode(N_NAME, line);
		strcpy(n->text, name);
		return n;
	}
	Errorf(l, line, "an expression was expected, found '%s'", l->tok.kind == T_EOF ? "end of file" : l->tok.text);
	if(l->tok.kind != T_EOF)
		Next(l);
	return NewNode(N_NUM, line);
}

static Node_t* ParsePostfix(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Node_t* n = ParsePrimary(p);
	while(IsPunct(l, "("))
	{ // a call through a value: (expr)(args)
		Node_t* call = NewNode(N_CALL, l->tok.line);
		call->a = n;
		n = ParseArgs(p, call);
	}
	return n;
}

static Node_t* ParseUnary(Parser_t* p)
{
	Lex_t* l = &p->lex;
	int line = l->tok.line;
	Node_t* n;
	if(IsPunct(l, "-") || IsPunct(l, "~") || IsPunct(l, "!"))
	{
		char op = l->tok.text[0];
		Next(l);
		n = NewNode(N_UNARY, line);
		n->op = op;
		n->a = ParseUnary(p);
		return n;
	}
	if(IsPunct(l, "&"))
	{
		Next(l);
		if(l->tok.kind != T_IDENT)
		{
			Errorf(l, line, "& takes a variable name");
			return ParseUnary(p);
		}
		n = NewNode(N_ADDR, line);
		strcpy(n->text, l->tok.text);
		Next(l);
		return n;
	}
	if(IsPunct(l, "*"))
	{
		int elem = 4;
		Next(l);
		// *(type*)(address) or *(type*)number
		if(Accept(l, "("))
		{
			if(!TypeKeyword(l, &elem))
			{
				Errorf(l, line, "*(type*) expected");
				return ParseUnary(p);
			}
			Next(l);
			Expect(l, "*");
			Expect(l, ")");
		}
		n = NewNode(N_DEREF, line);
		n->size = elem == 1 ? 0 : elem == 2 ? 1
											: 2;
		n->a = ParseUnary(p);
		return n;
	}
	return ParsePostfix(p);
}

// the binary operators by precedence, lowest first; '?:' and assignments sit below them
static Node_t* ParseBinary(Parser_t* p, int minPrec)
{
	Lex_t* l = &p->lex;
	Node_t* left = ParseUnary(p);
	for(;;)
	{
		const BpcOperator_t* op = l->tok.kind == T_PUNCT ? BpcOp_ByText(l->tok.text, strlen(l->tok.text)) : NULL;
		Node_t* n;
		if(!op || op->prec < minPrec)
			return left;
		Next(l);
		n = NewNode(N_BINARY, left->line);
		n->op = op->op;
		n->a = left;
		n->b = ParseBinary(p, op->prec + 1);
		left = n;
	}
}

static Node_t* ParseSelect(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Node_t* c = ParseBinary(p, 0);
	if(Accept(l, "?"))
	{
		Node_t* n = NewNode(N_SELECT, c->line);
		n->a = c;
		n->b = ParseAssignment(p);
		Expect(l, ":");
		n->c = ParseSelect(p);
		return n;
	}
	return c;
}

static Node_t* ParseAssignment(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Node_t* left = ParseSelect(p);
	if(IsPunct(l, "=") || IsPunct(l, "<-"))
	{
		int store = IsPunct(l, "<-");
		Node_t* n;
		Next(l);
		n = NewNode(store ? N_ASSIGN2 : N_ASSIGN, left->line);
		n->a = left;
		if(IsPunct(l, "{"))
		{ // an initializer list: a store_multi statement
			Node_t* list = NewNode(N_INITLIST, l->tok.line);
			Next(l);
			if(!IsPunct(l, "}"))
				for(;;)
				{
					AddItem(list, ParseAssignment(p));
					if(!Accept(l, ","))
						break;
				}
			Expect(l, "}");
			n->b = list;
			return n;
		}
		n->b = ParseAssignment(p);
		return n;
	}
	return left;
}

static Node_t* ParseExpression(Parser_t* p)
{
	return ParseAssignment(p);
}

// ---- statements -------------------------------------------------------------------------------------

static Node_t* ParseStatement(Parser_t* p);

static Node_t* ParseBlock(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Node_t* n = NewNode(N_BLOCK, l->tok.line);
	Expect(l, "{");
	while(!IsPunct(l, "}") && l->tok.kind != T_EOF)
	{
		Node_t* s = ParseStatement(p);
		if(s)
			AddItem(n, s);
	}
	Expect(l, "}");
	return n;
}

// `{ lines }` after __asm: the text between the braces, verbatim (braces inside strings are allowed)
static Node_t* ParseAsmBlock(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Node_t* n = NewNode(N_ASM, l->tok.line);
	const char* s;
	const char* q;
	int depth = 1, inStr = 0;
	if(!IsPunct(l, "{"))
	{
		Errorf(l, l->tok.line, "'{' expected after __asm");
		return n;
	}
	// the lexer sits after the brace token; take the raw text from there
	s = l->peeked ? l->peek.text : NULL; // (no peek expected here)
	(void)s;
	if(l->peeked)
	{
		Errorf(l, l->tok.line, "internal: lookahead before __asm");
		return n;
	}
	s = l->p;
	for(q = s; q < l->end; q++)
	{
		if(inStr)
		{
			if(*q == '\\' && q + 1 < l->end)
				q++;
			else if(*q == '"')
				inStr = 0;
			continue;
		}
		if(*q == '"')
			inStr = 1;
		else if(*q == '{')
			depth++;
		else if(*q == '}')
		{
			if(--depth == 0)
				break;
		}
		else if(*q == '\n')
			l->line++;
	}
	n->len = (uint32_t)(q - s);
	n->bytes = (uint8_t*)malloc(n->len + 1);
	memcpy(n->bytes, s, n->len);
	n->bytes[n->len] = 0;
	l->p = q < l->end ? q + 1 : q;
	Next(l);
	return n;
}

static Node_t* ParseDeclaration(Parser_t* p, int elem)
{
	Lex_t* l = &p->lex;
	Node_t* n = NewNode(N_DECL, l->tok.line);
	Next(l); // the type
	if(l->tok.kind != T_IDENT)
	{
		Errorf(l, n->line, "a variable name was expected");
		return n;
	}
	strcpy(n->text, l->tok.text);
	n->size = elem;
	n->op = 1;
	Next(l);
	if(Accept(l, "["))
	{
		if(l->tok.kind != T_NUM)
			Errorf(l, n->line, "an array size was expected");
		n->op = (int)l->tok.num;
		n->num = 1; // an array
		Next(l);
		Expect(l, "]");
	}
	if(Accept(l, "="))
		n->a = ParseAssignment(p);
	Expect(l, ";");
	if(p->func)
	{
		if(FindVar(p->func, n->text))
			Errorf(l, n->line, "%s is declared twice", n->text);
		else
			AddVar(p->func, n->text, elem, (uint32_t)elem * (uint32_t)n->op, n->num == 1);
	}
	return n;
}

static Node_t* ParseStatement(Parser_t* p)
{
	Lex_t* l = &p->lex;
	int line = l->tok.line;
	int elem;
	Node_t* n;
	if(IsPunct(l, "{"))
		return ParseBlock(p);
	if(Accept(l, ";"))
		return NULL;
	if(TypeKeyword(l, &elem))
		return ParseDeclaration(p, elem);
	if(l->tok.kind == T_IDENT)
	{
		if(Peek(l)->kind == T_PUNCT && strcmp(Peek(l)->text, ":") == 0 && !IsIdent(l, "default"))
		{
			n = NewNode(N_LABEL, line);
			strcpy(n->text, l->tok.text);
			Next(l);
			Next(l);
			return n;
		}
		if(IsIdent(l, "if"))
		{
			Next(l);
			n = NewNode(N_IF, line);
			Expect(l, "(");
			n->a = ParseExpression(p);
			Expect(l, ")");
			n->num = !IsPunct(l, "{");
			n->b = ParseStatement(p);
			if(!n->b)
				n->b = NewNode(N_BLOCK, line);
			if(IsIdent(l, "else"))
			{
				Next(l);
				n->c = ParseStatement(p);
				if(!n->c)
					n->c = NewNode(N_BLOCK, line);
			}
			return n;
		}
		if(IsIdent(l, "while"))
		{
			Next(l);
			n = NewNode(N_WHILE, line);
			Expect(l, "(");
			n->a = ParseExpression(p);
			Expect(l, ")");
			n->b = ParseStatement(p);
			if(!n->b)
				n->b = NewNode(N_BLOCK, line);
			return n;
		}
		if(IsIdent(l, "do"))
		{
			Next(l);
			n = NewNode(N_DOWHILE, line);
			n->b = ParseStatement(p);
			if(!n->b)
				n->b = NewNode(N_BLOCK, line);
			if(!IsIdent(l, "while"))
				Errorf(l, l->tok.line, "'while' expected after the do body");
			else
				Next(l);
			Expect(l, "(");
			n->a = ParseExpression(p);
			Expect(l, ")");
			Expect(l, ";");
			return n;
		}
		if(IsIdent(l, "for"))
		{
			Next(l);
			n = NewNode(N_FOR, line);
			Expect(l, "(");
			if(!IsPunct(l, ";"))
				n->a = ParseExpression(p);
			Expect(l, ";");
			if(!IsPunct(l, ";"))
				n->b = ParseExpression(p);
			Expect(l, ";");
			if(!IsPunct(l, ")"))
				n->c = ParseExpression(p);
			Expect(l, ")");
			{
				Node_t* body = ParseStatement(p);
				AddItem(n, body ? body : NewNode(N_BLOCK, line));
			}
			return n;
		}
		if(IsIdent(l, "break"))
		{
			Next(l);
			Expect(l, ";");
			return NewNode(N_BREAK, line);
		}
		if(IsIdent(l, "continue"))
		{
			Next(l);
			Expect(l, ";");
			return NewNode(N_CONTINUE, line);
		}
		if(IsIdent(l, "return"))
		{
			Next(l);
			n = NewNode(N_RETURN, line);
			if(!IsPunct(l, ";"))
				n->a = ParseExpression(p);
			Expect(l, ";");
			return n;
		}
		if(IsIdent(l, "goto"))
		{
			Next(l);
			n = NewNode(N_GOTO, line);
			strcpy(n->text, l->tok.text);
			Next(l);
			Expect(l, ";");
			return n;
		}
		if(IsIdent(l, "__asm"))
		{
			Next(l);
			return ParseAsmBlock(p);
		}
		if(IsIdent(l, "__inline"))
		{
			Next(l);
			n = NewNode(N_INLINE, line);
			Expect(l, "(");
			n->a = ParseAssignment(p);
			Expect(l, ",");
			if(l->tok.kind != T_STR)
				Errorf(l, line, "__inline takes a string literal");
			else
			{
				n->bytes = l->tok.bytes;
				n->len = l->tok.len;
				l->tok.bytes = NULL;
				Next(l);
			}
			Expect(l, ")");
			Expect(l, ";");
			return n;
		}
	}
	n = NewNode(N_EXPR, line);
	n->a = ParseExpression(p);
	Expect(l, ";");
	return n;
}

// ---- the unit ----------------------------------------------------------------------------------------

static void ParseDirective(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Unit_t* u = p->unit;
	char name[128], arg[64];
	int line = l->tok.line;
	strcpy(name, l->tok.text);
	strcpy(arg, l->tok.arg);
	Next(l);
	if(strcmp(name, "engine") == 0)
	{
		EngineGen_t g = GEN_COUNT;
		if(strcmp(arg, "any") != 0)
		{
			int i;
			for(i = 0; i < GEN_COUNT; i++)
				if(strcmp(kGenNames[i], arg) == 0)
					g = (EngineGen_t)i;
			if(g == GEN_COUNT)
				Errorf(l, line, "unknown engine %s", arg);
		}
		u->gen = g;
		u->genSet = 1;
		return;
	}
	Errorf(l, line, "unknown directive #%s", name);
}

static void ParseUnit(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Unit_t* u = p->unit;
	Next(l);
	while(l->tok.kind != T_EOF)
	{
		int line = l->tok.line;
		if(l->tok.kind == T_DIRECTIVE)
		{
			ParseDirective(p);
			continue;
		}
		if(IsIdent(l, "__program"))
		{
			Node_t* a;
			Next(l);
			a = ParseAsmBlock(p);
			u->program = (char*)a->bytes;
			a->bytes = NULL;
			FreeNode(a);
			continue;
		}
		if(l->tok.kind == T_IDENT)
		{
			Func_t* f = (Func_t*)calloc(1, sizeof *f);
			snprintf(f->name, sizeof f->name, "%s", l->tok.text);
			Next(l);
			if(FindFunc(u, f->name))
				Errorf(l, line, "function %s is defined twice", f->name);
			if(u->funcCount == u->funcCap)
			{
				u->funcCap = u->funcCap ? u->funcCap * 2 : 16;
				u->funcs = (Func_t**)realloc(u->funcs, (size_t)u->funcCap * sizeof *u->funcs);
			}
			u->funcs[u->funcCount++] = f;
			Expect(l, "(");
			if(!IsPunct(l, ")"))
				for(;;)
				{
					if(l->tok.kind != T_IDENT)
					{
						Errorf(l, l->tok.line, "a parameter name was expected");
						break;
					}
					AddVar(f, l->tok.text, 4, 4, 0);
					f->params++;
					Next(l);
					if(!Accept(l, ","))
						break;
				}
			Expect(l, ")");
			{ // the parameters lie in the frame in reverse order: the last one at its lowest address
				int k;
				for(k = 0; k < f->params; k++)
					f->vars[k].off = (uint32_t)(f->params - 1 - k) * 4;
			}
			p->func = f;
			f->body = ParseBlock(p);
			p->func = NULL;
			if(f->body->count == 1 && f->body->items[0]->kind == N_ASM)
			{
				f->isAsm = 1;
				f->asmNode = f->body->items[0];
			}
			continue;
		}
		Errorf(l, line, "a function definition was expected, found '%s'", l->tok.text);
		Next(l);
	}
}

static void FreeUnit(Unit_t* u)
{
	int i;
	for(i = 0; i < u->funcCount; i++)
	{
		FreeNode(u->funcs[i]->body);
		free(u->funcs[i]->vars);
		free(u->funcs[i]);
	}
	free(u->funcs);
	free(u->program);
}

// ---- the generator ------------------------------------------------------------------------------------

typedef struct Gen
{
	Unit_t* u;
	Lex_t* lex;
	Func_t* func;
	FILE* out; // the listing
	int labels;
	int errors;
	// the string table: appended in order of appearance
	int strings;
	struct
	{
		uint8_t* bytes;
		uint32_t len;
	}* strTab;
	int strCap;
	// the loop contexts
	int breakLabel[64], continueLabel[64];
	int depth;
	int exitLabel;
	// named labels of the current function: name -> label id
	struct
	{
		char name[128];
		int label;
	} named[512];
	int namedCount;
} Gen_t;

static void GenError(Gen_t* g, int line, const char* fmt, ...)
{
	va_list ap;
	g->errors++;
	if(g->errors > 50)
		return;
	fprintf(g->lex->log, "%s:%d: error: ", g->lex->fileName, line);
	va_start(ap, fmt);
	vfprintf(g->lex->log, fmt, ap);
	va_end(ap);
	fputc('\n', g->lex->log);
}

static void Emit(Gen_t* g, const char* fmt, ...)
{
	va_list ap;
	fputc('\t', g->out);
	va_start(ap, fmt);
	vfprintf(g->out, fmt, ap);
	va_end(ap);
	fputc('\n', g->out);
}

static int NewLabel(Gen_t* g)
{
	return g->labels++;
}

static void PlaceLabel(Gen_t* g, int label)
{
	fprintf(g->out, "L%d:\n", label);
}

// an unconditional jump to a label
static void EmitJump(Gen_t* g, int label)
{
	Emit(g, "push_code_off L%d", label);
	Emit(g, "jmp");
}

static int InternString(Gen_t* g, const uint8_t* bytes, uint32_t len)
{
	if(g->strings == g->strCap)
	{
		g->strCap = g->strCap ? g->strCap * 2 : 64;
		g->strTab = realloc(g->strTab, (size_t)g->strCap * sizeof *g->strTab);
	}
	g->strTab[g->strings].bytes = (uint8_t*)malloc(len + 1);
	memcpy(g->strTab[g->strings].bytes, bytes, len);
	g->strTab[g->strings].bytes[len] = 0;
	g->strTab[g->strings].len = len;
	return g->strings++;
}

static void EmitPush(Gen_t* g, uint32_t v)
{
	if(v <= 127)
		Emit(g, "push_i8 %u", v);
	else if(v <= 32767)
		Emit(g, "push_i16 %u", v);
	else
		Emit(g, "push_i32 0x%x", v);
}

static int NamedLabel(Gen_t* g, const char* name, int line)
{
	int i;
	for(i = 0; i < g->namedCount; i++)
		if(strcmp(g->named[i].name, name) == 0)
			return g->named[i].label;
	if(g->namedCount >= (int)(sizeof g->named / sizeof g->named[0]))
	{
		GenError(g, line, "too many labels");
		return 0;
	}
	snprintf(g->named[g->namedCount].name, sizeof g->named[0].name, "%s", name);
	g->named[g->namedCount].label = NewLabel(g);
	return g->named[g->namedCount++].label;
}

static void GenExpr(Gen_t* g, Node_t* n);
static void GenCond(Gen_t* g, Node_t* n, int trueLabel, int falseLabel, int trueFalls);

static const char* OpMnemonic(Gen_t* g, int op)
{
	const char* name = Asm_OpName(0, op, g->u->gen);
	static char buf[16];
	if(name)
		return name;
	snprintf(buf, sizeof buf, "0x%02x", op);
	return buf;
}

// a variable reference: its frame address (push_local_addr F - off)
static int VarAddress(Gen_t* g, const char* name, int line, Var_t** out)
{
	Var_t* v = FindVar(g->func, name);
	if(!v)
	{
		GenError(g, line, "unknown variable %s", name);
		return 0;
	}
	Emit(g, "push_local_addr 0x%x", (unsigned)(g->func->frame - v->off));
	*out = v;
	return 1;
}

static int IsCallNode(const Node_t* n)
{
	return n->kind == N_CALL || n->kind == N_OPCALL || n->kind == N_SELECT;
}

// the address of an lvalue and the size code of the access
static int GenLvalue(Gen_t* g, Node_t* n)
{
	Var_t* v;
	switch(n->kind)
	{
		case N_NAME:
			if(!VarAddress(g, n->text, n->line, &v))
				return 2;
			if(v->isArray)
				GenError(g, n->line, "%s is an array", n->text);
			return v->elem == 1 ? 0 : v->elem == 2 ? 1
												   : 2;
		case N_DEREF: GenExpr(g, n->a); return n->size;
		case N_PAREN: return GenLvalue(g, n->a);
		default: GenError(g, n->line, "not an lvalue"); return 2;
	}
}

static void GenCall(Gen_t* g, Node_t* n)
{
	int i;
	if(n->kind == N_OPCALL)
	{
		int fam = n->op >> 8, op = n->op & 0xff;
		const char* name = Asm_OpName(fam, op, g->u->gen);
		{ // a name the assembler would take for another number (a reused one under "any"): the number instead
			int f2, o2;
			char full[64];
			if(name)
				snprintf(full, sizeof full, fam ? "%s.%s" : "%s%s", fam ? Asm_FamilyName(fam) : "", name);
			if(name && (!Asm_LookupName(full, strlen(full), g->u->gen, &f2, &o2) || f2 != fam || o2 != op))
				name = NULL;
		}
		if(fam == 0 && op == OP_SPRINTF)
		{ // sprintf(dst, fmt, args..): the arguments right to left, then dst, then fmt
			if(n->count < 2)
			{
				GenError(g, n->line, "sprintf takes a destination and a format");
				return;
			}
			for(i = n->count - 1; i >= 2; i--)
				GenExpr(g, n->items[i]);
			GenExpr(g, n->items[0]);
			GenExpr(g, n->items[1]);
			Emit(g, "sprintf");
			return;
		}
		for(i = 0; i < n->count; i++)
			GenExpr(g, n->items[i]);
		if(fam)
			Emit(g, "%s.%s", Asm_FamilyName(fam), name ? name : n->text + strlen(Asm_FamilyName(fam)) + 1);
		else if(name)
			Emit(g, "%s", name);
		else
			Emit(g, "0x%02x", op); // op.0xNN: the listing's bare number
		return;
	}
	// a function: the arguments right to left, then the callee
	for(i = n->count - 1; i >= 0; i--)
		GenExpr(g, n->items[i]);
	if(n->a->kind == N_NAME && FindFunc(g->u, n->a->text) && !FindVar(g->func, n->a->text))
		Emit(g, "push_code_off %s", n->a->text);
	else
		GenExpr(g, n->a);
	Emit(g, "call");
}

static void GenExpr(Gen_t* g, Node_t* n)
{
	Var_t* v;
	switch(n->kind)
	{
		case N_NUM:
			if(n->size == 4)
				Emit(g, "push_i32 0x%x", (unsigned)n->num); // written as eight hex digits: pushed wide
			else
				EmitPush(g, (uint32_t)n->num);
			break;
		case N_STR: Emit(g, "push_code_addr str_%d", InternString(g, n->bytes, n->len)); break;
		case N_NAME:
			if(strcmp(n->text, "__pop") == 0)
				break; // the value already on the stack: nothing to emit
			if(FindVar(g->func, n->text))
			{
				VarAddress(g, n->text, n->line, &v);
				if(!v->isArray)
					Emit(g, "load %d", v->elem == 1 ? 0 : v->elem == 2 ? 1
																	   : 2);
			}
			else if(FindFunc(g->u, n->text))
				Emit(g, "push_code_off %s", n->text);
			else
				GenError(g, n->line, "unknown name %s", n->text);
			break;
		case N_ADDR:
			if(!VarAddress(g, n->text, n->line, &v))
				break;
			break;
		case N_DEREF:
			GenExpr(g, n->a);
			Emit(g, "load %d", n->size);
			break;
		case N_PAREN: GenExpr(g, n->a); break;
		case N_CALL:
			if(n->a->kind == N_NAME && strcmp(n->a->text, "__sign") == 0)
			{
				GenError(g, n->line, "__sign(v) compares only in a condition: __sign(v) < 0 and the like");
				break;
			}
			GenCall(g, n);
			break;
		case N_OPCALL: GenCall(g, n); break;
		case N_SELECT:
			GenExpr(g, n->a);
			GenExpr(g, n->b);
			GenExpr(g, n->c);
			Emit(g, "select");
			break;
		case N_UNARY:
			GenExpr(g, n->a);
			if(n->op == '-')
			{
				Emit(g, "not");
				Emit(g, "push_i8 1");
				Emit(g, "add");
			}
			else if(n->op == '~')
				Emit(g, "not");
			else
				Emit(g, "lnot");
			break;
		case N_BINARY:
			GenExpr(g, n->a);
			GenExpr(g, n->b);
			Emit(g, "%s", OpMnemonic(g, n->op));
			break;
		case N_ASSIGN:
		{
			int size;
			if(n->b->kind == N_INITLIST)
			{
				int i;
				size = GenLvalue(g, n->a);
				for(i = 0; i < n->b->count; i++)
					GenExpr(g, n->b->items[i]);
				Emit(g, "store_multi %d, %d", size, n->b->count);
				break;
			}
			if(IsCallNode(n->b))
			{ // a call's value: the value first, then the address, and a plain store
				GenExpr(g, n->b);
				size = GenLvalue(g, n->a);
				Emit(g, "store %d", size);
				break;
			}
			size = GenLvalue(g, n->a);
			GenExpr(g, n->b);
			Emit(g, "store_keep %d", size);
			break;
		}
		case N_ASSIGN2:
		{
			int size;
			GenExpr(g, n->b);
			size = GenLvalue(g, n->a);
			Emit(g, "store %d", size);
			break;
		}
		default: GenError(g, n->line, "not an expression"); break;
	}
}

/* The jumps of a condition: `n` with the true target `trueLabel` and the
 * false target `falseLabel`, one of which is the fall-through
 * (`trueFalls`: the true one).  `&&`, `||` and `!` are chains of jumps;
 * anything else - a parenthesized expression among it - is a value and
 * one jcc. */
// whether n is `__sign(v) OP 0` with OP one of < <= > >=
static int IsSignTest(const Node_t* n)
{
	return n->kind == N_BINARY && n->op >= 0x32 && n->op <= 0x35 && n->a->kind == N_CALL && n->a->a->kind == N_NAME && strcmp(n->a->a->text, "__sign") == 0 && n->a->count == 1 && n->b->kind == N_NUM && n->b->num == 0;
}

static void GenCond(Gen_t* g, Node_t* n, int trueLabel, int falseLabel, int trueFalls)
{
	if(n->kind == N_BINARY && n->op == 0x38)
	{ // a && b: a false -> false target; a true -> b
		int next = NewLabel(g);
		GenCond(g, n->a, next, falseLabel, 1);
		PlaceLabel(g, next);
		GenCond(g, n->b, trueLabel, falseLabel, trueFalls);
		return;
	}
	if(n->kind == N_BINARY && n->op == 0x39)
	{ // a || b: a true -> true target; a false -> b
		int next = NewLabel(g);
		GenCond(g, n->a, trueLabel, next, 0);
		PlaceLabel(g, next);
		GenCond(g, n->b, trueLabel, falseLabel, trueFalls);
		return;
	}
	if(n->kind == N_UNARY && n->op == '!')
	{
		GenCond(g, n->a, falseLabel, trueLabel, !trueFalls);
		return;
	}
	if(n->kind == N_PAREN && IsSignTest(n->a))
	{ // the parentheses of !(__sign(v) < 0) make no value
		GenCond(g, n->a, trueLabel, falseLabel, trueFalls);
		return;
	}
	if(IsSignTest(n))
	{                                                                         // __sign(v) < 0 and the like: the jump tests the sign of the value itself (jcc 2..5)
		static const int ccTrue[4] = {4, 3, 5, 2}, ccFalse[4] = {2, 5, 3, 4}; // <= >= < > : the code, its complement
		GenExpr(g, n->a->items[0]);
		if(trueFalls)
		{
			Emit(g, "push_code_off L%d", falseLabel);
			Emit(g, "jcc %d", ccFalse[n->op - 0x32]);
		}
		else
		{
			Emit(g, "push_code_off L%d", trueLabel);
			Emit(g, "jcc %d", ccTrue[n->op - 0x32]);
		}
		return;
	}
	GenExpr(g, n);
	if(trueFalls)
	{
		Emit(g, "push_code_off L%d", falseLabel);
		Emit(g, "jcc 1");
	}
	else
	{
		Emit(g, "push_code_off L%d", trueLabel);
		Emit(g, "jcc 0");
	}
}

static void GenStatements(Gen_t* g, Node_t* block);

static int IsJumpStatement(const Node_t* s)
{
	if(!s)
		return 0;
	if(s->kind == N_BLOCK)
		return 0;
	return s->kind == N_BREAK || s->kind == N_CONTINUE || s->kind == N_GOTO || (s->kind == N_RETURN && !s->a);
}

static void GenJump(Gen_t* g, Node_t* s)
{
	switch(s->kind)
	{
		case N_BREAK:
			if(!g->depth)
				GenError(g, s->line, "break outside a loop");
			else
				EmitJump(g, g->breakLabel[g->depth - 1]);
			break;
		case N_CONTINUE:
			if(!g->depth)
				GenError(g, s->line, "continue outside a loop");
			else
				EmitJump(g, g->continueLabel[g->depth - 1]);
			break;
		case N_GOTO: EmitJump(g, NamedLabel(g, s->text, s->line)); break;
		case N_RETURN: EmitJump(g, g->exitLabel); break;
		default: break;
	}
}

static void EmitAsmLines(Gen_t* g, const char* text, int line)
{
	const char* p = text;
	while(*p)
	{
		const char* e = strchr(p, '\n');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		const char* s = p;
		size_t k = 0;
		while(k < n && isspace((unsigned char)s[k]))
			k++;
		if(k < n)
		{
			// `push_code_addr "literal"`: the literal goes into the string table here
			if(n - k > 15 && strncmp(s + k, "push_code_addr ", 15) == 0 && s[k + 15] == '"')
			{
				uint8_t* bytes;
				uint32_t len;
				const char* next;
				const char* err = Asm_ReadStringLiteral(s + k + 15, s + n, &bytes, &len, &next);
				if(err)
					GenError(g, line, "in __asm: %s", err);
				else
				{
					Emit(g, "push_code_addr str_%d", InternString(g, bytes, len));
					free(bytes);
				}
			}
			else
				fprintf(g->out, "%.*s\n", (int)(n - k), s + k);
		}
		if(!e)
			break;
		p = e + 1;
	}
}

static void GenStatement(Gen_t* g, Node_t* s)
{
	switch(s->kind)
	{
		case N_BLOCK: GenStatements(g, s); break;
		case N_EXPR: GenExpr(g, s->a); break;
		case N_DECL:
			if(s->a)
			{ // int x = v: the value, the address, store
				Var_t* v;
				GenExpr(g, s->a);
				if(VarAddress(g, s->text, s->line, &v))
					Emit(g, "store %d", v->elem == 1 ? 0 : v->elem == 2 ? 1
																		: 2);
			}
			break;
		case N_IF:
		{
			int elseLabel = NewLabel(g), endLabel = NewLabel(g), thenLabel = NewLabel(g);
			if(!s->c && s->num && IsJumpStatement(s->b))
			{ // if (c) break; - the direct form: the condition jumps where the statement would
				int target;
				switch(s->b->kind)
				{
					case N_BREAK: target = g->depth ? g->breakLabel[g->depth - 1] : -1; break;
					case N_CONTINUE: target = g->depth ? g->continueLabel[g->depth - 1] : -1; break;
					case N_GOTO: target = NamedLabel(g, s->b->text, s->b->line); break;
					default: target = g->exitLabel; break;
				}
				if(target < 0)
				{
					GenError(g, s->line, "break or continue outside a loop");
					break;
				}
				GenCond(g, s->a, target, elseLabel, 0);
				PlaceLabel(g, elseLabel);
				break;
			}
			GenCond(g, s->a, thenLabel, elseLabel, 1);
			PlaceLabel(g, thenLabel);
			GenStatement(g, s->b);
			if(s->c)
			{
				EmitJump(g, endLabel);
				PlaceLabel(g, elseLabel);
				GenStatement(g, s->c);
			}
			else
				PlaceLabel(g, elseLabel);
			PlaceLabel(g, endLabel);
			break;
		}
		case N_WHILE:
		{
			int head = NewLabel(g), end = NewLabel(g), body = NewLabel(g);
			PlaceLabel(g, head);
			if(s->a)
				GenCond(g, s->a, body, end, 1);
			PlaceLabel(g, body);
			if(g->depth >= 64)
			{
				GenError(g, s->line, "loops nested too deep");
				break;
			}
			g->breakLabel[g->depth] = end;
			g->continueLabel[g->depth] = head;
			g->depth++;
			GenStatement(g, s->b);
			g->depth--;
			EmitJump(g, head);
			PlaceLabel(g, end);
			break;
		}
		case N_FOR:
		{
			int head = NewLabel(g), end = NewLabel(g), body = NewLabel(g), incr = NewLabel(g);
			if(s->a)
				GenExpr(g, s->a);
			PlaceLabel(g, head);
			if(s->b)
				GenCond(g, s->b, body, end, 1);
			PlaceLabel(g, body);
			if(g->depth >= 64)
			{
				GenError(g, s->line, "loops nested too deep");
				break;
			}
			g->breakLabel[g->depth] = end;
			g->continueLabel[g->depth] = s->c ? incr : head;
			g->depth++;
			GenStatement(g, s->items[0]);
			g->depth--;
			PlaceLabel(g, incr);
			if(s->c)
				GenExpr(g, s->c);
			EmitJump(g, head);
			PlaceLabel(g, end);
			break;
		}
		case N_DOWHILE:
		{
			int head = NewLabel(g), end = NewLabel(g), cond = NewLabel(g);
			PlaceLabel(g, head);
			if(g->depth >= 64)
			{
				GenError(g, s->line, "loops nested too deep");
				break;
			}
			g->breakLabel[g->depth] = end;
			g->continueLabel[g->depth] = cond;
			g->depth++;
			GenStatement(g, s->b);
			g->depth--;
			PlaceLabel(g, cond);
			GenCond(g, s->a, head, end, 0);
			PlaceLabel(g, end);
			break;
		}
		case N_BREAK:
		case N_CONTINUE:
		case N_GOTO: GenJump(g, s); break;
		case N_RETURN:
			if(s->a)
				GenExpr(g, s->a);
			EmitJump(g, g->exitLabel);
			break;
		case N_LABEL: PlaceLabel(g, NamedLabel(g, s->text, s->line)); break;
		case N_ASM: EmitAsmLines(g, (const char*)s->bytes, s->line); break;
		case N_INLINE:
		{
			uint32_t i;
			char* list;
			size_t o = 0;
			GenExpr(g, s->a);
			list = (char*)malloc(s->len * 6 + 16);
			for(i = 0; i < s->len; i++)
				o += (size_t)sprintf(list + o, "%s0x%02x", i ? ", " : "", s->bytes[i]);
			Emit(g, "store_inline %s", list);
			free(list);
			break;
		}
		default: GenError(g, s->line, "not a statement"); break;
	}
}

static void GenStatements(Gen_t* g, Node_t* block)
{
	int i;
	for(i = 0; i < block->count; i++)
		GenStatement(g, block->items[i]);
}

static void GenFunction(Gen_t* g, Func_t* f)
{
	int i;
	g->func = f;
	g->namedCount = 0;
	g->depth = 0;
	fprintf(g->out, "%s:\n", f->name);
	if(f->isAsm)
	{
		EmitAsmLines(g, (const char*)f->asmNode->bytes, f->asmNode->line);
		return;
	}
	g->exitLabel = NewLabel(g);
	Emit(g, "push_fp");
	EmitPush(g, f->frame);
	Emit(g, "add");
	Emit(g, "set_fp");
	for(i = 0; i < f->params; i++)
	{
		Emit(g, "push_local_addr 0x%x", (unsigned)(f->frame - f->vars[i].off));
		Emit(g, "store 2");
	}
	GenStatements(g, f->body);
	PlaceLabel(g, g->exitLabel);
	Emit(g, "push_fp");
	EmitPush(g, f->frame);
	Emit(g, "sub");
	Emit(g, "set_fp");
	Emit(g, "ret");
}

void Bpc_Compile(const char* text, size_t len, const char* fileName, EngineGen_t gen, FILE* log, BpcResult_t* out)
{
	Parser_t p;
	Unit_t unit;
	Gen_t g;
	FILE* mem;
	char* listing;
	long n;
	int i;
	AsmResult_t ar;
	memset(out, 0, sizeof *out);
	memset(&p, 0, sizeof p);
	memset(&unit, 0, sizeof unit);
	memset(&g, 0, sizeof g);
	unit.gen = gen;
	p.lex.p = text;
	p.lex.end = text + len;
	p.lex.line = 1;
	p.lex.log = log ? log : stderr;
	p.lex.fileName = fileName;
	p.unit = &unit;
	ParseUnit(&p);
	out->errors = p.lex.errors;
	if(out->errors)
	{
		FreeUnit(&unit);
		return;
	}
	mem = tmpfile();
	if(!mem)
	{
		out->errors = 1;
		FreeUnit(&unit);
		return;
	}
	g.u = &unit;
	g.lex = &p.lex;
	g.out = mem;
	fprintf(mem, ".engine %s\n", unit.gen == GEN_COUNT ? "any" : kGenNames[unit.gen]);
	if(unit.program)
		fputs(unit.program, mem);
	else
	{
		for(i = 0; i < unit.funcCount; i++)
			GenFunction(&g, unit.funcs[i]);
		fprintf(mem, "__codeend:\n");  // where the code ends, for the decompiler's comparison
		fprintf(mem, "\t.align 16\n"); // the code is padded even when no string follows
		for(i = 0; i < g.strings; i++)
		{
			fprintf(mem, "str_%d:\n\t.str ", i);
			Asm_WriteStringLiteral(mem, g.strTab[i].bytes, g.strTab[i].len);
			fputc('\n', mem);
		}
		if(g.strings)
			fprintf(mem, "\t.align 16\n");
	}
	out->errors = g.errors;
	for(i = 0; i < g.strings; i++)
		free(g.strTab[i].bytes);
	free(g.strTab);
	if(out->errors)
	{
		fclose(mem);
		FreeUnit(&unit);
		return;
	}
	n = ftell(mem);
	fseek(mem, 0, SEEK_SET);
	listing = (char*)malloc((size_t)n + 1);
	if(fread(listing, 1, (size_t)n, mem) != (size_t)n)
		n = 0;
	listing[n] = 0;
	fclose(mem);
	Asm_Assemble(listing, (size_t)n, unit.gen, p.lex.log, &ar);
	free(listing);
	out->errors = ar.errors;
	out->warnings = ar.warnings;
	out->file = ar.file;
	out->size = ar.size;
	out->labels = ar.labels;
	out->labelCount = ar.labelCount;
	FreeUnit(&unit);
}
