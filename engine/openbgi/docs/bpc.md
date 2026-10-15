# The program decompiler and compiler

The `._bp` programs - the system programs of `sysprg.arc`, the boot
program `ipl._bp` of `system.arc`, the `autoload.arc` entries - were
written in a C-like language and compiled by Buriko's own compiler to the
stack bytecode the engine runs (`docs/vm.md`).  `src/bpc/` recovers that
source from a program and compiles such source back, so that a system
program can be edited - its texts (the window titles, the messages of the
configuration and save dialogs) translated, a behaviour changed - and put
back into the game.  The assembler and disassembler (`docs/asm.md`) are
the layer below: the decompiler starts from the disassembler's analysis
of a file, the compiler produces a listing and assembles it.

`bin/bpasm` (`make tools`) is the command:

    bpasm -D [-e ENGINE] [--no-verify] [-v] [-o OUT.bpc] (FILE._bp | --arc ARCHIVE NAME)
    bpasm -C [-e ENGINE] SOURCE.bpc -o OUT._bp
    bpasm -R [-e ENGINE] [-v] (FILE._bp... | --arc ARCHIVE [NAME...])

`-D` decompiles one program (or one entry of an archive), `-C` compiles a
source file, `-R` round-trips programs (decompile, compile, compare; the
module's acceptance test).  `-e` is the engine profile (`bgi
--list-engines`); the source records it in its `#engine` line, so `-C`
needs it only when that line is missing.  `-v` makes `-D` and `-R` say
why a function could not be written as source, `-v -v` traces the
statements.

Every program of the 21 games (1856 programs of their `system.arc`,
`sysprg.arc` and `autoload.arc`) decompiles to source and compiles back
to the identical bytes, and none of their functions needed an `__asm`
block; `bpasm -R --arc sysprg.arc` prints `N programs, 0 failed`.
`tests/bpc.c` checks the shapes and the round trip on small sources.

## Editing a system program

    bpasm -l sysprg.arc                                # the entries
    bpasm -D -e 1.553 --arc sysprg.arc title._bp -o title.bpc
    (edit title.bpc)
    bpasm -C title.bpc -o title._bp
    bgiscn --pack sysprg.arc -o new.arc title._bp=title._bp
    (replace sysprg.arc with new.arc in the game folder)

`bgiscn --pack` (`docs/scenario.md`) repacks any archive.  The decompiler
verifies its own output - every function is compiled and compared with
the original bytes before it is written - so an unedited source always
compiles to the file it came from, and what the compiler accepts is
exactly what the original compiler's output can express.  Text is in
string literals as Shift-JIS (the source file is Shift-JIS as the
original was, with `\` escapes for what is not printable); a literal that
does not fit the window it is drawn in is the editor's problem, as it was
for the original programmers.

A sample, the ruby (reading) table of Heptagram, `kana._bp`:

    // kana._bp - decompiled by bpasm (docs/bpc.md)
    #engine 1.553

    main(p1) { int l20[8];
        *(int*)l20 = {sub_0031, sub_010c, sub_01bf, sub_01e1};
        (*(int*)(l20 + (p1 << 2)))();
    }

    sub_0031(p1, p2) { int l108; int l104; int l100[64];
        l108 = 0x19fe8 + 0x17ae8;
        l104 = 0;
        do {
            if (*(char*)(l108 + 0) == 0) goto L_00b8;
            if (streq(p2, l108 + 0)) goto L_00d5;
            l108 = l108 + 1 * 64;
            l104 = l104 + 1;
        } while (l104 < 4);
        sprintf(l100, "読み仮名は「%d」個までしか登録できません", 4);
        debug_msg(l100);
        return;
    L_00b8:
        *(int*)(0x19fe8 + 0x17ae4) = *(int*)(0x19fe8 + 0x17ae4) + 1;
    L_00d5:
        gfx1.ruby_set(p2, p1);
        memclr(l108, 64);
        strcpy(l108 + 0, p2);
        strcpy(l108 + 32, p1);
    }

The names are the decompiler's: `main` is the function at offset 0 (the
one the engine calls), `sub_XXXX` a function by its offset, `pN` the N-th
parameter, `lXX` a local by its frame address (`push_local_addr 0xXX`),
`L_XXXX` a label by its offset.  Any of them can be renamed in the
source; nothing depends on the spelling.  The original compiler did no
constant folding, so `0x19fe8 + 0x17ae8` is what the source said (a
global's address through two macros, by the look of it) and `1 * 64`
too; folding them would change the bytes.

## The source

A source file is a list of functions, each `name(params) { body }`; the
parameters are untyped words.  Declarations - `int x;`, `char buf[64];`,
`short s;`, `int t[4] = {1, 2, 3, 4};`, `int n = f();` - may stand
anywhere a statement may, and lay out the frame in the order they are
written; the decompiler writes them on the function's first line.  There
are no global declarations: a program addresses its globals and the
engine's memory numerically, `*(int*)1208`, `*(short*)(128 + 160)`,
`*(char*)(base + i)` with `int`, `short` and `char` the 4-, 2- and 1-byte
accesses (`load 2` / `load 1` / `load 0`).  `&x` is a local's address,
and an array's name is its address, as in C.

The statements are those of C: `if` / `else`, `while`, `do` / `while`,
`for`, `break`, `continue`, `return [v]`, `goto` and labels, blocks, `;`,
expression statements.  The expressions are C's, with `+ - * / % & | ^
<< >> == != < <= > >= && || ! ~` and unary `-`, `c ? a : b`, assignment
`=`, calls, and `>>>` for the logical shift (`>>` is the arithmetic
one).  Numbers are decimal or hex; `'c'` is a character; a string
literal is a `push_code_addr` of a copy in the file's string table (every
occurrence its own copy, as the original compiler did it).

Calls: a function of the program by name, `sub_0031(a, b)`; a value as
a function, `(*(int*)(l20 + 4))(x)` or `l2c(1, 0)` for a local holding a
code address (what `sys.load_program` returns); an engine instruction by
its listing name, `gfx0.sprite_create()`, `sys.yield()`, `strcpy(dst,
src)`, `sprintf(buf, "%d", n)` (`docs/asm.md` for the names; the
instruction's result is its value, an instruction that pushes nothing is
a statement).  An instruction without a name is `gfx1.0xf6(...)` by its
number, a base opcode without one `op.0x77(...)`, a script-defined one
("FF xx", a program a scenario registered) `user.0x41(...)`.

The differences from C are what the original compiler's output can and
cannot say.  A parenthesized condition is a value: `if ((a || b) && c)`
computes `a || b` with `lor` and tests the result, while `if (a || b && c)`
is a chain of jumps; the decompiler writes the parentheses it finds
and no others, and `a && b` as an operand of `&&` is never
parenthesized.  `__sign(v) < 0` (`<= 0`, `> 0`, `>= 0`) in a condition
tests the sign of `v` with the jump itself (`jcc 2` .. `jcc 5`), which
the old programs' counted loops did: `do { .. } while (__sign(i - 4) < 0);`
is not `i - 4 < 0`, which would push a `lt`.  A hex number of eight
digits, `0x00000000`, is pushed as 32 bits whatever its value.  `x <- v`
is the store that drops the value (`store`), where `x = v` keeps it on
the stack (`store_keep`) - every expression statement leaves its value
there, the machine's stack being a ring that is never cleared - and
`__pop` is a value an earlier statement left: `l4 <- __pop;` takes one
into a variable, `f(a, __pop)` passes one on, `if (__pop)` tests one.
`__inline(addr, "bytes")` is `store_inline`.  A brace-less `if (c) break;`
(`continue;`, `goto L;`, `return;`) jumps straight from the condition
(the direct form), while `if (c) { break; }` is a block with a jump in
it: the two compile differently, and the decompiler writes the form it
finds.  A `switch` there is not; the original dispatch is written as it
was compiled, a chain of `if (x == k) goto L_k;`.

`#engine NAME` names the engine profile (the opcode set).  `__asm { ..
}` holds listing lines where source cannot say what the bytes do (the
decompiler writes a function so when it could not express it or when its
source did not compile back to the same bytes; a string literal may stand
in such a block as `push_code_addr "text"`), and `__program { .. }` the
whole program as a listing.  Comments are `//` and `/* */`.

## The shapes of the original compiler

The decompiler relies on the original compiler's fixed way of compiling
each construct, and the compiler reproduces it; `src/bpc/bpc_internal.h`
lists the shapes.  A function is `push_fp; push F; add; set_fp`, then
the parameters popped into the frame (the first parameter popped first,
lying highest), the body, and one exit `push_fp; push F; sub; set_fp;
ret`; `return v` pushes v and jumps to the exit.  A function's arguments
are pushed right to left, an instruction's left to right, `sprintf`'s
conversions right to left before the destination and the format.  An
assignment is address, value, `store_keep`, except that a call's value
and a declaration's initializer are value, address, `store`.
Immediates are `push_i8` up to 127 and `push_i16` up to 32767; a
negative constant is the positive one negated (`not; push_i8 1; add`).
Conditions are chains of `jcc`: `a && b` jumps to the false target when
a is false, `a || b` to the true target when a is true, `!x` swaps the
targets, and a leaf jumps to whichever target is not the fall-through.
`if` jumps past the then part, `else` is a `jmp` over it, `while` tests
at the head and jumps back from the end, `do` tests at the end, `for` is
a `while` with its step before the back jump.  The compiler emitted dead
code as written (statements after a `return`, a loop's back jump after a
`break`), so does this one.

What the decompiler cannot settle it leaves to the source: locals are
typed from how they are accessed (a 4-byte load makes an `int`), sized
from the gap to the next local, and arrays when their address is taken
or they are not accessed at all (`char __pad[8]` covers a frame area
nothing refers to); a jump into a loop from outside, or into the
condition of a `do` from a nested loop, makes that loop labels and
gotos; a jump to the very next instruction (an empty `if (c) {}`, a
`goto` to the following statement) is written as the decompiler reads
it.  Everything is checked by compiling and comparing: the output is the
original's bytes or an `__asm` block.

## The API (inc/bgi/bpc.h)

`Bpc_Decompile(file, size, options, out)` writes the source of a program
file (header included) and returns the number of functions it had to
write as `__asm` blocks; `options.gen` is the engine generation,
`options.noVerify` skips the compile-and-compare, `options.verbose`
reports on stderr.  `Bpc_Compile(text, len, name, gen, log, result)`
compiles source text into a program file (`result.file`, `result.size`;
`result.errors` counts the messages written to `log` as `name:line:
error: ...`) and lists the labels of the listing it assembled
(`result.labels`: every function by its name, `__codeend` where the code
ends).  `Dis_Analyze` (`asm.h`) is the analysis the decompiler starts
from: the code / data map of a file with its labels.
