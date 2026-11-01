#!/usr/bin/env python3
"""Plain C to a ParaMesh program (X-translator). Standard library only.

Every for loop in main() is looked at. One that can be shown safe to split becomes a PM_TASK: the
pool runs it, and its arrays and malloc() memory move to pm_malloc(). The rest of the program stays
on the laptop that started it. A loop that cannot be split stays too, and the loops inside it are
tried instead. This reads C with patterns, not a parser, so a loop it cannot understand stays:
a wrong split would be a wrong answer with no error, and a loop left alone is only slower.

    tools/playground/translate.py in.c > out.c
"""
import re
import sys

WORD = r"(?:const|static|volatile|unsigned|signed|long|short|int|char|float|double|bool|size_t|ssize_t|u?int(?:8|16|32|64)_t)"
DECLARATION = re.compile(rf"(?<![\w(,])((?:{WORD}\b\s*)+)(?=[\w*])")
DECLARATOR = re.compile(r"^\s*(\*+)?\s*(\w+)\s*((?:\[[^\]]*\])*)\s*(?:=(.*))?$", re.S)
LOOP_INDEX = re.compile(rf"\bfor\s*\(\s*((?:{WORD}\b\s*)+)(\w+)\s*=")
UPDATE = r"\s*(?:=(?!=)|[-+*/%&|^]=|<<=|>>=|\+\+|--)"
MATH = set("""sqrt cbrt pow exp log log2 log10 sin cos tan asin acos atan atan2 sinh cosh tanh fabs
              floor ceil round fmod hypot abs labs llabs sqrtf powf fabsf""".split())
KNOWN = set("""for if else while do switch case default return sizeof continue break struct void
               const static unsigned signed long short int char float double bool size_t ssize_t
               int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t NULL
               true false""".split())
UNSAFE = re.compile(r"\b(printf|fprintf|sprintf|puts|putchar|getchar|fgets|scanf|rand|srand|time|clock|malloc|calloc|realloc|free|exit|abort)\s*\(")
HEADER = """#include <paramesh.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static void pm_must(int status) {
    if (status != PM_OK) {
        fprintf(stderr, "paramesh: %s\\n", pm_strerror(status));
        exit(1);
    }
}
static void* pm_shared(size_t bytes) {
    void* memory = pm_malloc(bytes);
    if (memory == NULL) {
        fprintf(stderr, "paramesh: this program needs more shared memory than the job was given\\n");
        exit(1);
    }
    return memory;
}
static void* pm_zeroed(size_t count, size_t size) { return pm_shared(count * size); }
"""


DEFAULT_REGION = 512 << 20  # a split program's size must be given, or it asks for the full 4 GB


class Stay(Exception):
    """This loop stays on the laptop that started the program, for this reason."""


def mask(source):
    """The source with comments, strings and characters blanked, offsets unchanged."""
    out = list(source)
    at = 0
    while at < len(source):
        two = source[at:at + 2]
        if two in ("//", "/*"):
            end = source.find("\n" if two == "//" else "*/", at)
            end = len(source) if end < 0 else end + (0 if two == "//" else 2)
        elif source[at] in "\"'":
            end = at + 1
            while end < len(source) and source[end] not in (source[at], "\n"):
                end += 2 if source[end] == "\\" else 1
            end = min(end + 1, len(source))
        else:
            at += 1
            continue
        for i in range(at, end):
            if out[i] != "\n":
                out[i] = " "
        at = end
    return "".join(out)


def closing(text, at):
    """The offset of the bracket that closes the one at `at`."""
    pairs = {"(": ")", "{": "}", "[": "]"}
    opening, close, depth = text[at], pairs[text[at]], 0
    for i in range(at, len(text)):
        depth += (text[i] == opening) - (text[i] == close)
        if depth == 0:
            return i
    raise Stay("a bracket is never closed")


def split_top(text, separator):
    parts, depth, begin = [], 0, 0
    for i, c in enumerate(text):
        depth += (c in "([{") - (c in ")]}")
        if c == separator and depth == 0:
            parts.append(text[begin:i])
            begin = i + 1
    return parts + [text[begin:]]


def line_of(text, at):
    return text.count("\n", 0, at) + 1


def words(text):
    return set(re.findall(r"\b[A-Za-z_]\w*\b", text))


class Variable:
    def __init__(self, name, base, stars, dims, statement, constant=False):
        self.name, self.base, self.stars, self.dims, self.statement = name, base, stars, dims, statement
        self.constant = constant

    def pointer(self):
        return f"{self.base} (*{self.name}){self.dims[self.dims.index(']') + 1:]}"


class Statement:
    """One declaration: where it is, and the declarators in it."""
    def __init__(self, begin, end, base, parts):
        self.begin, self.end, self.base, self.parts = begin, end, base, parts
        self.rewrite = {}


def declarations(masked, begin, end):
    found = []
    at = begin
    while True:
        match = DECLARATION.search(masked, at, end)
        if match is None:
            return found
        stop, depth = match.end(), 0
        while stop < end and (depth > 0 or masked[stop] not in ";{)"):
            depth += (masked[stop] in "([") - (masked[stop] in ")]")
            stop += 1
        at = match.end()
        if stop >= end or masked[stop] != ";":
            continue
        base = " ".join(w for w in match[1].split() if w not in ("const", "static", "volatile"))
        statement = Statement(match.start(), stop + 1, base, [])
        for part in split_top(masked[match.end():stop], ","):
            found_part = DECLARATOR.match(part)
            if found_part is None:
                break
            statement.parts.append(Variable(found_part[2], base, len(found_part[1] or ""),
                                            found_part[3].replace(" ", ""), statement, "const" in match[1]))
        else:
            found.append(statement)
            at = stop + 1


def outside_inner_loops(body):
    """The body with inner loops and switches blanked: a break left in it ends the marked loop."""
    while True:
        match = re.search(r"\b(?:for|while|switch)\s*\(|\bdo\s*\{", body)
        if match is None:
            return body
        at = match.end() - 1
        at = closing(body, at) + 1 if body[at] == "(" else at
        while at < len(body) and body[at].isspace():
            at += 1
        if at < len(body) and body[at] == "{":
            end = closing(body, at) + 1
        else:
            end = body.find(";", at) + 1 or len(body)
        body = body[:match.start()] + " " * (end - match.start()) + body[end:]


class Loop:
    pass


class Program:
    def __init__(self, source):
        self.source, self.masked = source, mask(source)
        main = re.search(r"\bint\s+main\s*\(([^)]*)\)\s*\{", self.masked)
        if main is None:
            raise Stay("no int main(...) { ... }")
        self.main = main
        self.open = main.end() - 1
        self.close = closing(self.masked, self.open)
        self.text = self.masked[self.open:self.close]
        self.globals = [s for s in declarations(self.masked, 0, main.start())
                        if self.depth(s.begin) == 0]
        self.locals = declarations(self.masked, self.open + 1, self.close)
        for found in LOOP_INDEX.finditer(self.masked, self.open, self.close):
            base = " ".join(w for w in found[1].split() if w not in ("const", "static"))
            self.locals.append(Statement(found.start(), found.start(), base, []))
            self.locals[-1].parts.append(Variable(found[2], base, 0, "", self.locals[-1]))
        self.locals.sort(key=lambda s: s.begin)
        blanked = list(self.masked[:main.start()])
        for statement in self.globals:
            blanked[statement.begin:statement.end] = " " * (statement.end - statement.begin)
        self.before = "".join(blanked)
        self.outside = self.before + self.masked[self.close:]
        self.macros = {m[1]: m[2] for m in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+(\w+)(.*)$",
                                                       self.masked, re.M)}
        self.functions = {}
        for found in re.finditer(r"^[\w \t*]+?\b(\w+)\s*\(([^;{}]*)\)\s*\{", self.before + " " * 8 +
                                 self.masked[self.close:], re.M):
            text = self.before + " " * 8 + self.masked[self.close:]
            self.functions[found[1]] = (found[2], text[found.end():closing(text, found.end() - 1)])
        self.shared_arrays, self.cells, self.done = {}, [], []

    def depth(self, at):
        return self.masked.count("{", 0, at) - self.masked.count("}", 0, at)

    def seen(self, at):
        return {v.name: v for s in self.globals + self.locals if s in self.globals or s.begin < at
                for v in s.parts}

    def line(self, at):
        return line_of(self.masked, at)


def parse_loop(masked, begin):
    loop = Loop()
    loop.begin = begin
    head = re.compile(r"for\s*\(").match(masked, begin)
    close = closing(masked, head.end() - 1)
    parts = split_top(masked[head.end():close], ";")
    at = close + 1
    while at < len(masked) and masked[at].isspace():
        at += 1
    if masked[at:at + 1] == "{":
        end = closing(masked, at)
        loop.body, loop.end = (at + 1, end), end + 1
    else:
        end, depth = at, 0
        while end < len(masked) and (depth > 0 or masked[end] != ";"):
            depth += (masked[end] in "([{") - (masked[end] in ")]}")
            end += 1
        loop.body, loop.end = (at, end + 1), end + 1
    form = "it is not of the form for (i = first; i < last; i++)"
    if len(parts) != 3:
        raise Stay(form)
    start = re.match(r"^\s*(.*?\b)?\s*(\w+)\s*=(?!=)(.*)$", parts[0], re.S)
    if start is None:
        raise Stay(form)
    loop.index, loop.first, loop.index_type = start[2], start[3].strip(), (start[1] or "").strip()
    name = re.escape(loop.index)
    limit = re.match(rf"^\s*{name}\s*(<=?)\s*(.*?)\s*$", parts[1], re.S)
    step = re.match(rf"^\s*(?:{name}\s*\+\+|\+\+\s*{name}|{name}\s*\+=\s*1)\s*$", parts[2])
    if limit is None or step is None:
        raise Stay(form)
    loop.last = limit[2] if limit[1] == "<" else f"({limit[2]}) + 1"
    return loop


def pure(program, name, trail=()):
    """Whether a function of this file can run anywhere, at the same time, with the same result."""
    if name in MATH:
        return True
    if name in trail or name not in program.functions:
        return False
    parameters, body = program.functions[name]
    for parameter in split_top(parameters, ","):
        if re.search(r"[*\[]", parameter) and "const" not in parameter:
            return False
    changing = {v.name for s in program.globals for v in s.parts if not v.constant}
    if UNSAFE.search(body) or re.search(r"\bstatic\b", body) or words(body) & changing:
        return False
    return all(pure(program, called, trail + (name,))
               for called in calls(body) if called not in KNOWN)


def calls(body):
    return set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", body))


def as_reduction(body, name):
    """The operator of a total: `name` is only ever added to, or only ever multiplied. Or None."""
    uses = len(re.findall(rf"\b{name}\b", body))
    plus = re.findall(rf"\b{name}\s*(?:\+=|-=|\+\+|--)|(?:\+\+|--)\s*{name}\b", body)
    times = re.findall(rf"\b{name}\s*\*=", body)
    # x = x * 2 + i is neither: what follows must bind tighter than the operator, or as tight.
    plus_long = re.findall(rf"\b{name}\s*=(?!=)\s*{name}\s*[-+](?:[^;<>&|^?=]*);", body)
    times_long = re.findall(rf"\b{name}\s*=(?!=)\s*{name}\s*\*(?:[^;+\-/%<>&|^?=]*);", body)
    if re.search(rf"\b{name}\s*=(?!=)", body) and not plus_long + times_long:
        return None
    if plus + plus_long and not times + times_long and uses == len(plus) + 2 * len(plus_long):
        return "+"
    if times + times_long and not plus + plus_long and uses == len(times) + 2 * len(times_long):
        return "*"
    return None


def scratch(program, loop, body, name):
    """Whether the loop sets `name` before using it, each time round, and nothing reads it after."""
    first = re.search(rf"\b{name}\b", body)
    plain = re.compile(rf"\s*=(?!=)").match(body, first.end())
    if plain is None or re.search(rf"\b{name}\b", re.compile(r"[^;]*").match(body, plain.end())[0]):
        return False
    return body.count("{", 0, first.start()) == body.count("}", 0, first.start()) and \
        unused_after(program, loop, name)


def unused_after(program, loop, name):
    after = re.search(rf"\b{name}\b", program.masked[loop.end:program.close])
    if after is None:
        return True
    rest = program.masked[loop.end + after.end():program.close]
    return re.match(r"\s*=(?!=)", rest) is not None


def study(program, loop):
    """Works out what the loop shares. Raises Stay if it cannot be shown safe."""
    masked, (begin, end) = program.masked, loop.body
    body, index = masked[begin:end], loop.index
    known = program.seen(loop.begin)
    if not loop.index_type and index not in known:
        raise Stay(f"{index} is not declared in a way the translator can read")
    found = re.search(r"\b(return|goto)\b", body)
    if found:
        raise Stay(f"it uses {found[1]}")
    if re.search(r"\bbreak\b", outside_inner_loops(body)):
        raise Stay("it can stop early with break, so its steps cannot run at the same time")
    found = UNSAFE.search(body)
    if found:
        raise Stay(f"it calls {found[1]}(), which has to happen in order on this laptop")
    if re.search(r"\bstruct\b|->", body):
        raise Stay("it uses a struct, which the translator does not read")
    if re.search(r"(?<![\w)\]&\s])\s*&(?!&)\s*[A-Za-z_]|(?:^|[(,=;{])\s*&(?!&)\s*[A-Za-z_]", body):
        raise Stay("it takes an address, so what it changes cannot be seen")
    for called in sorted(calls(body) - KNOWN):
        if called in program.macros:
            if UNSAFE.search(program.macros[called]):
                raise Stay(f"its macro {called} has side effects")
        elif not pure(program, called):
            raise Stay(f"it calls {called}(), which may have side effects")
    if re.search(rf"(?:\b{index}{UPDATE}|(?:\+\+|--)\s*\b{index}\b)", body) and \
            index not in re.findall(r"\bfor\s*\(\s*(?:\w+\s+)*(\w+)\s*=", body):
        raise Stay(f"it changes {index} inside the loop")
    inner = set(re.findall(r"\bfor\s*\(\s*(\w+)\s*=", body))
    declared_inside = {v.name for s in declarations(body, 0, len(body)) for v in s.parts} | \
        {m[2] for m in LOOP_INDEX.finditer(body)}
    inner |= declared_inside
    loop.reductions, loop.privates, loop.captured, loop.data = {}, set(), [], None
    for name in sorted(inner - declared_inside - {index}):
        if name not in known:
            raise Stay(f"{name} is not declared in a way the translator can read")
        if not unused_after(program, loop, name):
            raise Stay(f"{name} is read after the loop")
        loop.privates.add(name)
    unknown = words(body) - set(known) - KNOWN - inner - {index} - calls(body) - set(program.macros)
    if unknown:
        raise Stay(f"it uses {sorted(unknown)[0]}, which the translator cannot read")
    for name in sorted(words(body) & set(known) - inner - {index}):
        variable = known[name]
        if variable.dims or variable.stars:
            study_array(program, loop, body, variable)
        elif re.search(rf"(?:\b{name}{UPDATE}|(?:\+\+|--)\s*\b{name}\b)", body):
            op = as_reduction(body, name)
            if op:
                loop.reductions[name] = op
            elif scratch(program, loop, body, name):
                loop.privates.add(name)
            else:
                raise Stay(f"every step changes {name}, and the next step would need the result")
        else:
            loop.captured.append(name)


def study_array(program, loop, body, v):
    name, index = v.name, loop.index
    if v.stars > 1 or (v.stars and v.dims):
        raise Stay(f"{name} is an array of pointers, which cannot cross between laptops")
    if re.search(rf"(?<![\w\])\s])\s*\*\s*{name}\b", body):
        raise Stay(f"it writes through *{name}")
    writes = re.search(rf"\b{name}\s*(?:\[[^\]]*\])+{UPDATE}|(?:\+\+|--)\s*\b{name}\s*\[", body)
    if writes:
        for use in re.finditer(rf"\b{name}\s*\[([^\]]*)\]", body):
            if use[1].strip() != index:
                raise Stay(f"{name} is written, and {name}[{use[1].strip()}] means one step depends "
                           "on another, or two steps write the same place")
        for use in re.finditer(rf"\b{name}\b(?!\s*\[)", body):
            raise Stay(f"{name} is used as a whole")
        if loop.data is None:
            loop.data = name
    can_share(program, loop, v)
    loop.captured.append(name)


def can_share(program, loop, v):
    name = v.name
    if re.search(rf"\b{name}\b", program.outside):
        raise Stay(f"{name} is also used outside main()")
    text = program.text
    if v.dims:
        if re.search(rf"\bsizeof\s*\(?\s*{name}\b", text):
            raise Stay(f"sizeof {name} would change")
        part = next(p for p in split_top(program.masked[v.statement.begin:v.statement.end - 1], ",")
                    if re.search(rf"\b{name}\b", p))
        if "=" in part:
            raise Stay(f"{name} has an initial list; fill it in a loop instead")
        if v.statement in program.locals and program.depth(v.statement.begin) != program.depth(program.open) + 1:
            raise Stay(f"{name} is declared inside a block")
    else:
        sets = list(re.finditer(rf"\b{name}\s*=(?!=)", text))
        alloc = re.search(rf"\b{name}\s*=\s*(?:\([^)]*\)\s*)?(?:malloc|calloc)\s*\(", text)
        if alloc is None or len(sets) != 1:
            raise Stay(f"{name} must get its memory once, straight from malloc() or calloc()")
        if program.depth(program.open + alloc.start()) != program.depth(program.open) + 1:
            raise Stay(f"{name} is allocated inside a block")
        if re.search(rf"[=(,]\s*{name}\s*[;,)+]", text) and re.search(rf"=\s*{name}\s*[;+]", text):
            raise Stay(f"another pointer is set from {name}")


def generate(program, number, loop):
    source, masked = program.source, program.masked
    begin, end = loop.body
    known = program.seen(loop.begin)
    name = f"pm_loop_{number}"
    fields, setup, unpack, pointers = ["long long pm_from;"], [], [], []
    needed = list(loop.captured)
    for captured in list(needed):  # a[n][m]: the task needs m to know the shape
        for dim in words(known[captured].dims) & set(known):
            if dim not in needed:
                needed.append(dim)
    for captured in needed:
        v = known[captured]
        if v.dims or v.stars:
            fields.append(f"void* {v.name};")
            local = v.pointer() if v.dims else f"{v.base}* {v.name}"
            pointers.append(f"{local} = pm_a->{v.name};")
        else:
            fields.append(f"{v.base} {v.name};")
            unpack.append(f"{v.base} {v.name} = pm_a->{v.name};")
        setup.append(f"pm_a.{v.name} = {v.name};")
    finish, after, top = [], [], []
    for reduced, op in loop.reductions.items():
        base, cell, lock = known[reduced].base, f"pm_cell_{number}_{reduced}", f"pm_lock_{number}_{reduced}"
        top += [f"static {base}* {cell};", f"static pm_lock_t {lock};"]
        program.cells += [f"{cell} = pm_shared(sizeof({base}));", f"{lock} = pm_lock_create();"]
        fields += [f"{base}* {reduced};", f"pm_lock_t pm_lock_{reduced};"]
        setup += [f"pm_a.{reduced} = {cell};", f"*{cell} = {reduced};", f"pm_a.pm_lock_{reduced} = {lock};"]
        unpack.append(f"{base} {reduced} = {0 if op == '+' else 1};")
        finish += [f"pm_must(pm_lock(pm_a->pm_lock_{reduced}));", f"*pm_a->{reduced} {op}= {reduced};",
                   f"pm_must(pm_unlock(pm_a->pm_lock_{reduced}));"]
        after.append(f"{reduced} = *{cell};")
    for private in sorted(loop.privates):
        unpack.append(f"{known[private].base} {private};")
    kind = loop.index_type or known[loop.index].base
    kind = " ".join(w for w in kind.split() if w not in ("const", "static"))
    task = [*top, f"struct {name}_args {{", *fields, "};", f"PM_TASK({name}) {{",
            f"const struct {name}_args* pm_a = arg;", *unpack, *pointers,
            f"for ({kind} {loop.index} = ({kind})(pm_a->pm_from + (long long)lo); "
            f"{loop.index} < ({kind})(pm_a->pm_from + (long long)hi); {loop.index}++) {{",
            f'#line {line_of(masked, begin)} "main.c"', source[begin:end], "}", *finish, "}\n"]
    data = f'&{loop.data}[pm_a.pm_from], sizeof({loop.data}[0])' if loop.data else None
    call = (f'pm_must(pm_parallel_for_data("{name}", 0, pm_count, 0, &pm_a, sizeof pm_a, {data}));'
            if data else f'pm_must(pm_parallel_for("{name}", 0, pm_count, 0, &pm_a, sizeof pm_a));')
    site = [f"{{ struct {name}_args pm_a; memset(&pm_a, 0, sizeof pm_a);",
            f"pm_a.pm_from = {loop.first};", f"long long pm_count = ({loop.last}) - pm_a.pm_from;",
            *setup, "if (pm_count > 0) {", call, "pm_must(pm_wait_all());", *after, "}"]
    if not loop.index_type:
        site.append(f"{loop.index} = pm_a.pm_from + (pm_count > 0 ? pm_count : 0);")
    site.append("}")
    return "\n".join(task), "\n".join(site)


def edits_for_memory(program):
    """What moves the shared arrays and pointers to pm_malloc()."""
    edits, start_up = [], []
    text = program.text
    statements = {}
    for name, v in sorted(program.shared_arrays.items()):
        if v.dims:
            statements.setdefault(id(v.statement), (v.statement, set()))[1].add(name)
            continue
        alloc = re.search(rf"\b{name}\s*=\s*(?:\([^)]*\)\s*)?(malloc|calloc)\s*\(", text)
        at = program.open + alloc.start(1)
        edits.append((at, at + len(alloc[1]), "pm_shared" if alloc[1] == "malloc" else "pm_zeroed"))
        for freed in re.finditer(rf"\bfree\s*\(\s*{name}\s*\)\s*;", text):
            edits.append((program.open + freed.start(), program.open + freed.end(), ";"))
    for statement, names in statements.values():
        parts = split_top(program.source[statement.begin:statement.end - 1], ",")
        parts[0] = parts[0][len(re.match(rf"\s*(?:{WORD}\b\s*)+", parts[0])[0]):]
        out = []
        for v, part in zip(statement.parts, parts):
            size = f"sizeof({v.base}{v.dims})"
            if v.name not in names:
                out.append(f"{v.base} {part.strip()};")
            elif statement in program.globals:
                out.append(f"static {v.pointer()};")
                start_up.append(f"{v.name} = pm_shared({size});")
            else:
                out.append(f"{v.pointer()} = pm_shared({size});")
        edits.append((statement.begin, statement.end, " ".join(out)))
    return edits, start_up


def try_loops(program, begin, end, notes):
    """Each outermost loop in the range; one that stays has the loops inside it tried."""
    masked = program.masked
    at = begin
    while True:
        found = re.compile(r"\bfor\s*\(").search(masked, at, end)
        if found is None:
            return
        try:
            loop = parse_loop(masked, found.start())
        except Stay as stay:
            notes.append(f"line {program.line(found.start())} stays on this laptop: {stay}")
            at = found.end()
            continue
        try:
            before = dict(program.shared_arrays)
            study(program, loop)
            known = program.seen(loop.begin)
            for name in loop.captured:
                if known[name].dims or known[name].stars:
                    program.shared_arrays[name] = known[name]
            program.done.append(loop)
            note = f"line {program.line(found.start())} runs on the pool"
            floats = [n for n in loop.reductions if known[n].base in ("double", "float", "long double")]
            if floats:
                note += f" ({floats[0]} adds up in any order, so its last digits can differ between runs)"
            updated = [n for n in loop.captured if re.search(rf"\b{n}\s*(?:\[[^\]]*\])+\s*(?:[-+*/%]=|\+\+|--)",
                                                              masked[loop.body[0]:loop.body[1]])]
            if updated:
                note += f" (if a laptop leaves mid-run, its part of {updated[0]} is counted twice)"
            notes.append(note)
            at = loop.end
        except Stay as stay:
            program.shared_arrays = before
            notes.append(f"line {program.line(found.start())} stays on this laptop: {stay}")
            try_loops(program, loop.body[0], loop.body[1], notes)
            at = loop.end


def translate(source, region_bytes=DEFAULT_REGION):
    """Returns the program to compile and notes, one for each loop, saying where it runs."""
    if re.search(r"\bpm_init\s*\(", mask(source)):
        return source, []
    try:
        program = Program(source)
    except Stay as stay:
        return source, [f"nothing was split: {stay}"]
    notes = []
    try_loops(program, program.open, program.close, notes)
    if not program.done:
        return source, notes
    masked = program.masked
    edits, tasks = [], []
    for number, loop in enumerate(program.done, 1):
        task, site = generate(program, number, loop)
        tasks.append(task)
        edits.append((loop.begin, loop.end, f'{site}\n#line {line_of(masked, loop.end)} "main.c"\n'))
    memory, start_up = edits_for_memory(program)
    edits += memory
    for found in re.finditer(r"\breturn\b([^;]*);", program.text):
        at = program.open + found.start()
        value = source[at + 6:at + found.end() - found.start() - 1].strip() or "0"
        edits.append((at, program.open + found.end(),
                      f"{{ int pm_code = ({value}); pm_must(pm_finalize()); return pm_code; }}"))
    edits.append((program.close, program.close, "pm_must(pm_finalize());\nreturn 0;\n"))
    main = program.main
    parameters = main[1].strip()
    named = re.match(r"^int\s+(\w+)\s*,\s*char\s*\*\s*\*\s*(\w+)$", parameters) or \
        re.match(r"^int\s+(\w+)\s*,\s*char\s*\*\s*(\w+)\s*\[\s*\]$", parameters)
    if parameters not in ("", "void") and named is None:
        return source, notes + ["nothing was split: main needs (void) or (int argc, char** argv)"]
    if re.search(r"\bexit\s*\(", program.text):
        return source, notes + ["nothing was split: main() calls exit(); use return"]
    argc, argv = (named[1], named[2]) if named else ("argc", "argv")
    if named is None:
        edits.append((main.start(1), main.end(1), "int argc, char** argv"))
    start = [f"const pm_config pm_config_ = {{.region_bytes = {region_bytes}ULL}};",
             f"pm_must(pm_init(&{argc}, &{argv}, &pm_config_));", *start_up, *program.cells]
    edits.append((program.open + 1, program.open + 1, "\n" + "\n".join(start) + "\n"))
    task_text = "\n".join(tasks)
    edits.append((main.start(), main.start(), f'{task_text}\n#line {line_of(masked, main.start())} "main.c"\n'))
    out = source
    for begin, end, text in sorted(edits, key=lambda e: (e[0], e[1]), reverse=True):
        out = out[:begin] + text + out[end:]
    return f'{HEADER}#include <string.h>\n#line 1 "main.c"\n{out}', notes


def readable(program):
    """The program to show a person: the line markers are for the compiler."""
    return re.sub(r'^#line \d+ "main\.c"\n', "", program, flags=re.M)


def main():
    with open(sys.argv[1]) as file:
        text, notes = translate(file.read())
    print(text, end="")
    for note in notes:
        print(f"note: {note}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
