#!/usr/bin/env python3
"""Сверка JIT с интерпретатором на случайных программах (разработка, не часть набора).

Три ошибки кодогенерации JIT за один день (2026-11-09: `s += f(x,7) + f(x,18)` → 0; `gf_sel(p[0], q[0], b)` без записи; `hex_digit` в узле VM)
находились случайно.  у JIT (2022-11-03 10:52) — гарантия
здесь одна: тот же результат, что у интерпретатора. Генератор собирает программы из конструкций, на которых JIT уже ошибался (циклы по
range(константа), вложенные, вызовы с литеральными аргументами, изменяемые параметры с аргументом-местом, массивы массивов, локальные функции,
break/continue); данные — от args.size (рантайм), чтобы разбор ничего не свернул. Запуск: tools/jit_fuzz.py [число] [зерно] [путь к vish].
Расхождение — программа сохраняется в out/fuzz/ (минимизировать вручную) и код выхода 1.
"""
import os, random, subprocess, sys

REDUCE = len(sys.argv) > 2 and sys.argv[1] == "--reduce"
N = int(sys.argv[1]) if len(sys.argv) > 1 and not REDUCE else 100
SEED = int(sys.argv[2]) if len(sys.argv) > 2 and not REDUCE else 1
VISH = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(__file__), "..", "out", "vish")
OUT = os.path.join(os.path.dirname(__file__), "..", "out", "fuzz")
os.makedirs(OUT, exist_ok=True)

M = "0xffffffff"

class gen:
    def __init__(self, rnd):
        self.r = rnd
        self.vars = []   # скалярные переменные в области
        self.loop = []   # имена переменных циклов
        self.fresh = 0
        self.helpers = []  # локальные функции в области
        self.nested = True   # использовать массивы массивов p, q (JIT берёт такие функции не целиком — режим без них проверяет сами функции)

    def name(self, p):
        self.fresh += 1
        return f"{p}{self.fresh}"

    def idx(self):
        r = self.r
        if self.loop and r.random() < 0.6:
            v = r.choice(self.loop)
            c = r.choice(["", " + 1", " + 3", " + 5"])
            return f"(({v}{c}) & 15)"
        return str(r.randrange(0, 16))

    def atom(self):
        r = self.r
        k = r.random()
        if k < 0.30:
            return f"w[{self.idx()}]"
        if k < 0.45 and self.vars:
            return r.choice(self.vars)
        if k < 0.55 and self.loop:
            return r.choice(self.loop)
        if k < 0.65 and self.nested:
            return f"p[{r.randrange(0, 2)}][{r.randrange(0, 4)}]"
        return str(r.randrange(0, 200))

    def expr(self, d=0):
        r = self.r
        if d >= 3 or r.random() < 0.25:
            return self.atom()
        k = r.random()
        a = self.expr(d + 1)
        b = self.expr(d + 1)
        if k < 0.20:
            return f"(({a} + {b}) & {M})"
        if k < 0.35:
            return f"(({a} - {b}) & {M})"
        if k < 0.45:
            return f"(({a} * {r.randrange(2, 9)}) & {M})"
        if k < 0.55:
            return f"({a} ^ {b})"
        if k < 0.62:
            return f"({a} & {b})"
        if k < 0.68:
            return f"({a} | {b})"
        if k < 0.74:
            return f"(({a} << {r.randrange(0, 9)}) & {M})"
        if k < 0.80:
            return f"({a} >> {r.randrange(0, 9)})"
        if k < 0.88:
            return f"rot(({a} & {M}), {r.randrange(1, 31)})"
        if k < 0.93:
            return f"mix({a}, {b})"
        if k < 0.97 and self.helpers:
            return f"{r.choice(self.helpers)}({a})"
        return f"twice({a})"

    def stmt(self, depth):
        r = self.r
        k = r.random()
        pad = "  " * (depth + 1)
        if k < 0.28 and self.vars:
            v = r.choice(self.vars)
            op = r.choice(["+=", "^=", "|="])
            e = self.expr()
            if op == "+=":
                return f"{pad}{v} = ({v} + {e}) & {M};\n"
            if op == "^=":
                return f"{pad}{v} = {v} ^ {e};\n"
            return f"{pad}{v} = {v} | {e};\n"
        if k < 0.40:
            return f"{pad}w[{self.idx()}] = {self.expr()};\n"
        if k < 0.48:
            return f"{pad}bump(w, {self.idx()}, {self.expr()});\n"
        if k < 0.54 and self.nested:
            return f"{pad}bump(p[{r.randrange(0, 2)}], {r.randrange(0, 4)}, {self.expr()});\n"
        if k < 0.60 and self.nested:
            return f"{pad}swap2(p[{r.randrange(0, 2)}], q[{r.randrange(0, 2)}], {r.randrange(0, 2)});\n"
        if k < 0.64:
            return f"{pad}swap2(w, w2, {r.randrange(0, 2)});\n"
        if k < 0.68 and depth < 3:
            h = self.name("h")
            body = self.expr()
            self.helpers.append(h)
            return f"{pad}{h}(i64 y) => (((y * {r.randrange(2, 9)}) + {r.randrange(0, 5)}) & {M});\n"
        if k < 0.80 and depth < 3:
            v = self.name("t")
            lim = r.choice([str(r.randrange(2, 24)), "n", "w.size", "(w.size - 2)"])
            self.loop.append(v)
            saved = list(self.helpers)   # локальные функции вложенного блока снаружи не видны
            body = "".join(self.stmt(depth + 1) for _ in range(r.randrange(1, 4)))
            self.helpers = saved
            if r.random() < 0.15:
                body += f"{pad}  if(({v} & 3) == 1) continue;\n"
            if r.random() < 0.10:
                body += f"{pad}  if(({v} & 7) == 5) break;\n"
            self.loop.pop()
            return f"{pad}for({v} : range({lim})) {{\n{body}{pad}}}\n"
        if k < 0.84 and depth < 3:   # while со счётчиком (счётчик — не в vars: тело его не трогает)
            v = self.name("c")
            lim = r.randrange(2, 12)
            self.loop.append(v)
            saved = list(self.helpers)
            body = "".join(self.stmt(depth + 1) for _ in range(r.randrange(1, 3)))
            self.helpers = saved
            self.loop.pop()
            return f"{pad}{v} = 0;\n{pad}while({v} < {lim}) {{\n{body}{pad}  {v} = {v} + 1;\n{pad}}}\n"
        if k < 0.88 and depth < 3:
            c = f"({self.expr()} & 1) == 0"
            saved = list(self.helpers)
            a = "".join(self.stmt(depth + 1) for _ in range(r.randrange(1, 3)))
            self.helpers = list(saved)
            b = "".join(self.stmt(depth + 1) for _ in range(r.randrange(1, 3)))
            self.helpers = saved
            return f"{pad}if({c}) {{\n{a}{pad}}} else {{\n{b}{pad}}}\n"
        if k < 0.92 and depth == 0:   # ранний возврат на верхнем уровне
            return f"{pad}if(({self.expr()} & 7) == 3) return ({self.expr()} & {M});\n"
        if self.vars:
            v = r.choice(self.vars)
            return f"{pad}{v} = {self.expr()};\n"
        return f"{pad}w[{self.idx()}] = {self.expr()};\n"

    def function(self, fname):
        r = self.r
        self.vars = ["s", "u"]
        self.loop = []
        self.helpers = []
        body = "".join(self.stmt(0) for _ in range(r.randrange(3, 8)))
        return (f"{fname}(w, w2, p, q, i64 n) {{\n  s = 1;\n  u = 2;\n{body}"
                f"  (s + (u * 3)) & {M}\n}}\n")

PRE = """rot(i64 x, i64 n) => ((x >> n) | (x << (32 - n))) & 0xffffffff;
mix(i64 a, i64 b) => ((a * 31) ^ (b + 7)) & 0xffffffff;
twice(i64 x) => (x + x) & 0xffffffff;
bump(arr, i64 i, i64 v) {
  arr[i] = (arr[i] + v) & 0xffffffff;
}
swap2(a, b, i64 sel) {
  c = -sel;
  for(i : range(4)) {
    t = c & (a[i] ^ b[i]);
    a[i] = a[i] ^ t;
    b[i] = b[i] ^ t;
  }
}
mk(i64 seed) {
  a = array{};
  for(k : range(4)) a.push((seed * (k + 3)) & 0xffff);
  a
}
sum_of(w, p, q) {
  s = 0;
  for(i : range(w.size)) s = (s + (w[i] * (i + 1))) & 0xffffffff;
  for(i : range(2)) for(j : range(4)) s = (s + (((p[i][j] ^ q[i][j]) * (i + j + 2)) & 0xffffffff)) & 0xffffffff;
  s
}
"""

def program(seed):
    rnd = random.Random(seed)
    g = gen(rnd)
    g.nested = rnd.random() < 0.4
    nf = rnd.randrange(2, 5)
    fs = [g.function(f"f{i}") for i in range(nf)]
    main = ["main() {\n", "  n = args.size + 11;\n", "  w = array{};\n", "  w2 = array{};\n",
            "  for(k : range(16)) {\n    w.push(((k * 7) + args.size) & 0xffff);\n    w2.push(((k * 5) + (args.size * 3)) & 0xffff);\n  }\n",
            "  p = array{mk(args.size + 3), mk(args.size + 5)};\n", "  q = array{mk(args.size + 7), mk(args.size + 9)};\n"]
    for i in range(nf):
        main.append(f"  print(f{i}(w, w2, p, q, n), sum_of(w, p, q), sum_of(w2, q, p));\n")
    main.append("}\n")
    return PRE + "".join(fs) + "".join(main)


PRE2 = """struct pt { i64 x; i64 y; }
bumpx(pt q, i64 v) {
  q.x = (q.x + v) & 0xffffffff;
}
"""

class sgen:
    """Строки и структуры (код редактора — в основном такой): s[k], push, ==, size, поля структуры, изменяемый параметр-структура."""
    def __init__(self, rnd):
        self.r = rnd
        self.loop = []
        self.fresh = 0
    def idx(self):
        if self.loop and self.r.random() < 0.6:
            return f"({self.r.choice(self.loop)} % s.size)"
        return f"({self.r.randrange(0, 9)} % s.size)"
    def atom(self):
        r = self.r
        k = r.random()
        if k < 0.30:
            return f"s[{self.idx()}]"
        if k < 0.45:
            return "r"
        if k < 0.55 and self.loop:
            return r.choice(self.loop)
        if k < 0.62:
            return "q.x"
        if k < 0.68:
            return "q.y"
        if k < 0.74:
            return "t.size"
        return str(r.randrange(0, 200))
    def expr(self, d=0):
        r = self.r
        if d >= 3 or r.random() < 0.3:
            return self.atom()
        a = self.expr(d + 1)
        b = self.expr(d + 1)
        k = r.random()
        if k < 0.3:
            return f"(({a} + {b}) & {M})"
        if k < 0.45:
            return f"({a} ^ {b})"
        if k < 0.6:
            return f"(({a} * {r.randrange(2, 9)}) & {M})"
        if k < 0.75:
            return f"({a} & {b})"
        return f"({a} >> {r.randrange(0, 7)})"
    def stmt(self, depth):
        r = self.r
        pad = "  " * (depth + 1)
        k = r.random()
        if k < 0.20:
            return f"{pad}r = (r + {self.expr()}) & {M};\n"
        if k < 0.34:
            return f"{pad}t.push(text_byte((({self.expr()}) & 25) + 65));\n"
        if k < 0.42:
            return f"{pad}bumpx(q, {self.expr()});\n"
        if k < 0.48:
            return f"{pad}q.y = {self.expr()};\n"
        if k < 0.54:
            return f"{pad}if(t == s) r = (r + 1) & {M};\n"
        if k < 0.60:
            return f"{pad}r = (r + (q.x ^ q.y)) & {M};\n"
        if k < 0.66:
            return f"{pad}q = pt{{r, {self.expr()}}};\n"
        if k < 0.84 and depth < 3:
            v = f"k{self.fresh}"
            self.fresh += 1
            lim = r.choice([str(r.randrange(2, 20)), "s.size", "n", "(s.size + 2)"])
            self.loop.append(v)
            body = "".join(self.stmt(depth + 1) for _ in range(r.randrange(1, 4)))
            if r.random() < 0.15:
                body += f"{pad}  if(({v} & 3) == 1) continue;\n"
            self.loop.pop()
            return f"{pad}for({v} : range({lim})) {{\n{body}{pad}}}\n"
        if k < 0.92 and depth < 3:
            a = "".join(self.stmt(depth + 1) for _ in range(r.randrange(1, 3)))
            return f"{pad}if(({self.expr()} & 1) == 0) {{\n{a}{pad}}}\n"
        return f"{pad}r = ({self.expr()}) & {M};\n"
    def function(self, name):
        body = "".join(self.stmt(0) for _ in range(self.r.randrange(3, 8)))
        return (f"{name}(str s, i64 n) {{\n  r = 3;\n  t = \"\";\n  q = pt{{n, 7}};\n{body}"
                f"  (((r + t.size) + q.x) + (q.y & 255)) & {M}\n}}\n")

def program2(seed):
    rnd = random.Random(seed)
    g = sgen(rnd)
    nf = rnd.randrange(2, 5)
    fs = [g.function(f"sf{i}") for i in range(nf)]
    main = ["main() {\n", "  n = args.size + 5;\n", "  s = \"\";\n",
            "  for(k : range(args.size + 7)) s.push(text_byte(65 + ((k * 7) % 26)));\n"]
    for i in range(nf):
        main.append(f"  print(sf{i}(s, n), sf{i}(s + \"xyz\", n + 1));\n")
    main.append("}\n")
    return PRE + PRE2 + "".join(fs) + "".join(main)

def run(path, jit):
    cmd = [VISH, "run"] + (["--jit"] if jit else []) + [path, "a", "b"]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "timeout"

def differs(src, tmp):
    open(tmp, "w").write(src)
    a = run(tmp, False)
    if a[0] != 0 or "()" in a[1]:
        return False   # интерпретатор программу отверг или вывел unit (пустая функция) — вырожденная, не то расхождение
    return a != run(tmp, True)

def reduce(path):
    """Построчный ddmin: убирать куски строк, пока расхождение сохраняется."""
    tmp = path + ".red.vish"
    lines = open(path).read().split("\n")
    if not differs("\n".join(lines), tmp):
        print("расхождения нет"); return
    size = max(1, len(lines) // 2)
    while size >= 1:
        i = 0
        changed = False
        while i < len(lines):
            cand = lines[:i] + lines[i + size:]
            if differs("\n".join(cand), tmp):
                lines = cand
                changed = True
            else:
                i += size
        if not changed:
            size //= 2
    # second phase: bracketed subexpressions of the string - constant, while the discrepancy persists
    def groups(line):
        st = []
        res = []
        for i, c in enumerate(line):
            if c == "(":
                st.append(i)
            elif c == ")" and st:
                j = st.pop()
                res.append((j, i + 1))
        return sorted(res, key=lambda g: g[0] - g[1])   # крупные сначала
    progress = True
    while progress:
        progress = False
        for li in range(len(lines)):
            if lines[li].lstrip().startswith(("rot(", "mix(", "twice(", "main(", "mk(", "sum_of(", "swap2(", "bump(")):
                continue
            for (a0, b0) in groups(lines[li]):
                if b0 - a0 <= 3:
                    continue
                for repl in ("(1)", "(w[1])"):
                    cand_line = lines[li][:a0] + repl + lines[li][b0:]
                    cand = lines[:li] + [cand_line] + lines[li + 1:]
                    if differs("\n".join(cand), tmp):
                        lines = cand
                        progress = True
                        break
                if progress:
                    break
            if progress:
                break
    out = path + ".min.vish"
    open(out, "w").write("\n".join(lines))
    print("минимизировано:", out, len(lines), "строк")

if len(sys.argv) > 2 and sys.argv[1] == "--reduce":
    reduce(sys.argv[2])
    sys.exit(0)

bad = 0
rejected = 0
first_rej = None
for k in range(N):
    seed = SEED * 100000 + k
    src = program2(seed) if k % 2 else program(seed)   # нечётные — строки и структуры
    path = os.path.join(OUT, f"p{seed}.vish")
    open(path, "w").write(src)
    a = run(path, False)
    b = run(path, True)
    if a[0] != 0 and b[0] != 0 and a[2].strip() == b[2].strip():
        rejected += 1   # оба одинаково отвергли программу (ошибка генератора) — не расхождение
        if first_rej is None:
            first_rej = (path, a[2].strip()[:300])
        else:
            os.remove(path)
        continue
    if a != b:
        bad += 1
        print(f"РАСХОЖДЕНИЕ seed={seed}: {path}")
        print("  интерпретатор:", a[0], a[1].strip()[:200], a[2].strip()[:200])
        print("  JIT:          ", b[0], b[1].strip()[:200], b[2].strip()[:200])
    else:
        os.remove(path)
print(f"программ {N}, отвергнуто обоими {rejected}, расхождений {bad}")
if first_rej: print("пример отказа:", first_rej[0], "\n ", first_rej[1])
sys.exit(1 if bad else 0)
