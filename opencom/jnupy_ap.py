### This file is jnupy auto parser ###
from __future__ import print_function
import collections
import hashlib
import sys
import os
import re

if __name__ != "__main__" or len(sys.argv) != 2:
    exit("usage: ./jnupy_ap.py jnupy.c")

# how about use compiled jar?

def build_parse_call(func, matcher="match"):
    regexp = re.compile(r"{}\((.*?)\)".format(re.escape(func)))
    def parse(pattern):
        res = regexp.match(pattern)
        if not res:
            return None, None, None, pattern, ""
        
        args = tuple(map(str.strip, res.group(1).split(",")))
        return res.group(), func, args, pattern[:res.start()], pattern[res.end() + 1:]
    
    return parse

def build_call(func, *args):
    return "{}({})".format(func, ", ".join(args))


JNUPY_AP = "JNUPY_AP"
JNUPY_AP_DEFINE = "#define {}(...)".format(JNUPY_AP)

JNUPY_CLASS = "JNUPY_CLASS"
JNUPY_METHOD = "JNUPY_METHOD"
JNUPY_FIELD = "JNUPY_FIELD"

JNUPY_AP_CALL = build_parse_call(JNUPY_AP)
JNUPY_CLASS_CALL = build_parse_call(JNUPY_CLASS)
JNUPY_METHOD_CALL = build_parse_call(JNUPY_METHOD)
JNUPY_FIELD_CALL = build_parse_call(JNUPY_FIELD)

HASH_LEVEL = 4

try:
    unicode
except NameError:
    StringTypes = (str,)
else:
    StringTypes = (unicode, str,)

class FileControl():
    def __init__(self, lines):
        self.lines = lines
        self.ref = {i:i for i in range(len(lines))}
    
    def __getitem__(self, x):
        getline = self.lines.__getitem__
        lines = []
        for _, ref in self._getsource(x):
            if ref is not None:
                lines.append(getline(ref))
        
        if not isinstance(x, slice):
            if not lines:
                raise KeyError(x)
            return lines[0]
        else:
            return lines

    def __delitem__(self, x):
        for src, ref in self._getsource(x):
            if ref is not None:
                self.ref[src] = None
                self.lines[ref] = None

    def __setitem__(self, x, lines):
        if isinstance(lines, StringTypes):
            lines = [lines]

        for _, ref in self._getsource(x):
            if ref is None:
                raise KeyError
            self.lines[ref] = lines
            break
    
    #TODO: insert method?
    
    def __contains__(self, x):
        for _, ref in self._getsource(x):
            if ref is None:
                return False
                        
        return True

    def _getsource(self, x):
        getlineno = self.ref.get
        if not isinstance(x, slice):
            ref_lineno = getlineno(x)
            if ref_lineno is None:
                raise KeyError(x)
            return ((x, ref_lineno),)
        else:
            linenos = []
            start = x.start if x.start is not None else 0
            end = x.stop if x.stop is not None else len(self.ref)
            step = x.step if x.step is not None else 1
            i = start
            while i < end:
                ref_lineno = getlineno(i)
                # ref_lineno can be None
                linenos.append((i, ref_lineno))
                i += step
            return linenos

    def getvalue(self):
        result = []
        for line in self.lines:
            if line is None:
                continue
            
            if isinstance(line, StringTypes):
                result.append(line)
            else:
                result.extend(line)

        return "\n".join(result)

config = {}
ref = {}
href = set()

# AP TAG
REF = "REF"
LOAD = "LOAD"
EXPORT = "EXPORT"

# AP BLOCK
START = "START"
END = "END"

# AP VIRTUAL CONFIG
TAB = "TAB" # with START

# AP TYPE
CLASS = "CLASS"
METHOD = "METHOD"
FIELD = "FIELD"

def block_assign(fc, tag, data):
    try:
        start = config[tag, START]
        end = config[tag, END]
    except KeyError as e:
        raise ValueError("config[{}] block are not exists.".format(tag))
    
    size = len(range(start, end))
    if size <= 1:
        raise ValueError("config[{}] block are invaild. (size = {})".format(tag, size))
    
    del fc[start + 2:end]
    fc[start + 1] = data
    
    return True

with open(sys.argv[1]) as fp:
    lines = fp.read().splitlines()

IS_JNUPY = False
IS_EXPORT = False

for lineno, rawline in enumerate(lines):
    line = rawline.rstrip()
    if line == JNUPY_AP_DEFINE:
        IS_JNUPY = True
        continue
    
    if not IS_JNUPY:
        continue
    
    matched, _, args, _, _ = JNUPY_AP_CALL(line.strip())
    if matched:
        config[args] = lineno
        
        if args[-1] == START:
            config[args[:-1] + (TAB,)] = rawline[:len(rawline) - len(rawline.lstrip())]

        if args[0] == EXPORT:
            assert not IS_EXPORT
            IS_EXPORT = True

    #define JNUPY_CLASS(name, id) _JNUPY_REF_ID(id)
    #define JNUPY_METHOD(class_, name, id) _JNUPY_REF_ID(id)
    #define JNUPY_FIELD(class_, name, id) _JNUPY_REF_ID(id)
    if not IS_EXPORT:
        continue
    
    for ref_type, parse_call, argnum in (
            (CLASS, JNUPY_CLASS_CALL, 1),
            (METHOD, JNUPY_METHOD_CALL, 2),
            (FIELD, JNUPY_FIELD_CALL, 2),
        ):
        
        lefted = line
        result = []
        while True:
            matched, func, args, prefix, postfix = parse_call(lefted)
            result.append(prefix)
            lefted = postfix
            
            if not matched:
                break
            
            assert argnum <= len(args)
            if ref_type == CLASS:
                vname, = args[:1]
                tag = ref_type, (vname,)
            elif ref_type in (METHOD, FIELD):
                class_, vname, vtype, = args[:3]
                if (CLASS, (class_,)) not in ref:
                    raise ValueError("class {} are not used".format(class_))

                tag = ref_type, (class_, vname, vtype,)
            else:
                assert False
            
            hash_value = ref.get(tag)
            if hash_value is None:
                for idx in range(256):
                    hash_value = hashlib.md5(repr(tag + (idx,)).encode()).hexdigest()[:HASH_LEVEL]
                    if hash_value not in href:
                        href.add(hash_value)
                        break
                else:
                    raise RuntimeError("hash crash!")

                ref[tag] = hash_value
            
            args = map(str, args[:argnum] + (hash_value,))
            
            result.append(build_call(func, *args))
            
        result.append(lefted)
        line = "".join(result)

    lines[lineno] = line

fc = FileControl(lines)

#define JNUPY_JCLASS(a, b) b
#define JNUPY_JMETHOD(a, b) b
#define JNUPY_JFIELD(a, b) b
#define JNUPY_CLASS(name, id) _JNUPY_REF_ID(id)
#define JNUPY_METHOD(class_, name, id) _JNUPY_REF_ID(id)
#define JNUPY_FIELD(class_, name, id) _JNUPY_REF_ID(id)

#define _JNUPY_REF(vtype, id, default) STATIC vtype _JNUPY_REF_ID(id) = default;
#define JNUPY_REF_CLASS(id) _JNUPY_REF(jclass, id, NULL)
#define JNUPY_REF_METHOD(id) _JNUPY_REF(jmethodID, id, 0)
#define JNUPY_REF_FIELD(id) _JNUPY_REF(jfieldID, id, 0)

#define JNUPY_LOAD_CLASS(name, id) \
#    _JNUPY_LOAD(id, referenceclass(env, (name)))
#define JNUPY_LOAD_METHOD(cls, name, type, id) \
#    _JNUPY_LOAD(id, (*env)->GetMethodID(env, cls, name, type))
#define JNUPY_LOAD_FIELD(cls, name, type, id) \
#    _JNUPY_LOAD(id, (*env)->GetFieldID(env, cls, name, type))

"""
	if (!(luadebug_class = referenceclass(env, "com/naef/jnlua/LuaState$LuaDebug"))
			|| !(luadebug_init_id = (*env)->GetMethodID(env, luadebug_class, "<init>", "(JZ)V"))
			|| !(luadebug_field_id = (*env)->GetFieldID(env, luadebug_class, "luaDebug", "J"))) {
"""

ref_block = []
tab = config[REF, TAB]
out = ref_block.append
for (ref_type, info), hash_value in ref.items():
    out(tab + "#define _jnupy_REF_{} _jnupy_ref_{}".format(hash_value, hash_value))

for (ref_type, info), hash_value in ref.items():
    out(tab + build_call("JNUPY_REF_" + ref_type, hash_value))

block_assign(fc, REF, ref_block)

load_block = []
tab = config[LOAD, TAB]
out = load_block.append
for (ref_type, info), hash_value in ref.items():
    if ref_type == CLASS:
        args = info[0], hash_value
    else:
        args = info[0], info[1], info[2], ref[CLASS, (info[0],)], hash_value
    
    out(tab + build_call("JNUPY_LOAD_" + ref_type, *args))

block_assign(fc, LOAD, load_block)
 
with open(sys.argv[1], 'w') as fp:
    fp.write(fc.getvalue())

sys.exit(0)
