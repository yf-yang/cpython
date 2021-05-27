#include <stdbool.h>

#include "Python.h"
#include "code.h"
#include "opcode.h"
#include "structmember.h"         // PyMemberDef
#include "pycore_code.h"          // _PyOpcache
#include "pycore_interp.h"        // PyInterpreterState.co_extra_freefuncs
#include "pycore_pystate.h"       // _PyInterpreterState_GET()
#include "pycore_tuple.h"         // _PyTuple_ITEMS()
#include "clinic/codeobject.c.h"


/******************
 * generic helpers
 ******************/

/* all_name_chars(s): true iff s matches [a-zA-Z0-9_]* */
static int
all_name_chars(PyObject *o)
{
    const unsigned char *s, *e;

    if (!PyUnicode_IS_ASCII(o))
        return 0;

    s = PyUnicode_1BYTE_DATA(o);
    e = s + PyUnicode_GET_LENGTH(o);
    for (; s != e; s++) {
        if (!Py_ISALNUM(*s) && *s != '_')
            return 0;
    }
    return 1;
}

static int
intern_strings(PyObject *tuple)
{
    Py_ssize_t i;

    for (i = PyTuple_GET_SIZE(tuple); --i >= 0; ) {
        PyObject *v = PyTuple_GET_ITEM(tuple, i);
        if (v == NULL || !PyUnicode_CheckExact(v)) {
            PyErr_SetString(PyExc_SystemError,
                            "non-string found in code slot");
            return -1;
        }
        PyUnicode_InternInPlace(&_PyTuple_ITEMS(tuple)[i]);
    }
    return 0;
}

/* Intern selected string constants */
static int
intern_string_constants(PyObject *tuple, int *modified)
{
    for (Py_ssize_t i = PyTuple_GET_SIZE(tuple); --i >= 0; ) {
        PyObject *v = PyTuple_GET_ITEM(tuple, i);
        if (PyUnicode_CheckExact(v)) {
            if (PyUnicode_READY(v) == -1) {
                return -1;
            }

            if (all_name_chars(v)) {
                PyObject *w = v;
                PyUnicode_InternInPlace(&v);
                if (w != v) {
                    PyTuple_SET_ITEM(tuple, i, v);
                    if (modified) {
                        *modified = 1;
                    }
                }
            }
        }
        else if (PyTuple_CheckExact(v)) {
            if (intern_string_constants(v, NULL) < 0) {
                return -1;
            }
        }
        else if (PyFrozenSet_CheckExact(v)) {
            PyObject *w = v;
            PyObject *tmp = PySequence_Tuple(v);
            if (tmp == NULL) {
                return -1;
            }
            int tmp_modified = 0;
            if (intern_string_constants(tmp, &tmp_modified) < 0) {
                Py_DECREF(tmp);
                return -1;
            }
            if (tmp_modified) {
                v = PyFrozenSet_New(tmp);
                if (v == NULL) {
                    Py_DECREF(tmp);
                    return -1;
                }

                PyTuple_SET_ITEM(tuple, i, v);
                Py_DECREF(w);
                if (modified) {
                    *modified = 1;
                }
            }
            Py_DECREF(tmp);
        }
    }
    return 0;
}

/* Return a shallow copy of a tuple that is
   guaranteed to contain exact strings, by converting string subclasses
   to exact strings and complaining if a non-string is found. */
static PyObject*
validate_and_copy_tuple(PyObject *tup)
{
    PyObject *newtuple;
    PyObject *item;
    Py_ssize_t i, len;

    len = PyTuple_GET_SIZE(tup);
    newtuple = PyTuple_New(len);
    if (newtuple == NULL)
        return NULL;

    for (i = 0; i < len; i++) {
        item = PyTuple_GET_ITEM(tup, i);
        if (PyUnicode_CheckExact(item)) {
            Py_INCREF(item);
        }
        else if (!PyUnicode_Check(item)) {
            PyErr_Format(
                PyExc_TypeError,
                "name tuples must contain only "
                "strings, not '%.500s'",
                Py_TYPE(item)->tp_name);
            Py_DECREF(newtuple);
            return NULL;
        }
        else {
            item = _PyUnicode_Copy(item);
            if (item == NULL) {
                Py_DECREF(newtuple);
                return NULL;
            }
        }
        PyTuple_SET_ITEM(newtuple, i, item);
    }

    return newtuple;
}


/******************
 * _PyCode_New()
 ******************/

int
_PyCode_Validate(struct _PyCodeConstructor *con)
{
    /* Check argument types */
    if (con->argcount < con->posonlyargcount || con->posonlyargcount < 0 ||
        con->kwonlyargcount < 0 ||
        con->stacksize < 0 || con->flags < 0 ||
        con->code == NULL || !PyBytes_Check(con->code) ||
        con->consts == NULL || !PyTuple_Check(con->consts) ||
        con->names == NULL || !PyTuple_Check(con->names) ||
        con->varnames == NULL || !PyTuple_Check(con->varnames) ||
        con->freevars == NULL || !PyTuple_Check(con->freevars) ||
        con->cellvars == NULL || !PyTuple_Check(con->cellvars) ||
        con->name == NULL || !PyUnicode_Check(con->name) ||
        con->filename == NULL || !PyUnicode_Check(con->filename) ||
        con->linetable == NULL || !PyBytes_Check(con->linetable) ||
        con->exceptiontable == NULL || !PyBytes_Check(con->exceptiontable)
        ) {
        PyErr_BadInternalCall();
        return -1;
    }

    /* Make sure that code is indexable with an int, this is
       a long running assumption in ceval.c and many parts of
       the interpreter. */
    if (PyBytes_GET_SIZE(con->code) > INT_MAX) {
        PyErr_SetString(PyExc_OverflowError, "co_code larger than INT_MAX");
        return -1;
    }

    /* Ensure that the co_varnames has enough names to cover the arg counts.
     * Note that totalargs = nlocals - nplainlocals.  We check nplainlocals
     * here to avoid the possibility of overflow (however remote). */
    int nplainlocals = (int)PyTuple_GET_SIZE(con->varnames) -
                       con->argcount -
                       con->kwonlyargcount -
                       ((con->flags & CO_VARARGS) != 0) -
                       ((con->flags & CO_VARKEYWORDS) != 0);
    if (nplainlocals < 0) {
        PyErr_SetString(PyExc_ValueError, "code: varnames is too small");
        return -1;
    }

    return 0;
}

static void
init_code(PyCodeObject *co, struct _PyCodeConstructor *con)
{
    Py_INCREF(con->filename);
    co->co_filename = con->filename;
    Py_INCREF(con->name);
    co->co_name = con->name;
    co->co_flags = con->flags;

    Py_INCREF(con->code);
    co->co_code = con->code;
    co->co_firstlineno = con->firstlineno;
    Py_INCREF(con->linetable);
    co->co_linetable = con->linetable;

    Py_INCREF(con->consts);
    co->co_consts = con->consts;
    Py_INCREF(con->names);
    co->co_names = con->names;

    Py_INCREF(con->varnames);
    co->co_varnames = con->varnames;
    Py_INCREF(con->cellvars);
    co->co_cellvars = con->cellvars;
    Py_INCREF(con->freevars);
    co->co_freevars = con->freevars;

    co->co_argcount = con->argcount;
    co->co_posonlyargcount = con->posonlyargcount;
    co->co_kwonlyargcount = con->kwonlyargcount;

    co->co_stacksize = con->stacksize;

    Py_INCREF(con->exceptiontable);
    co->co_exceptiontable = con->exceptiontable;

    /* derived values */
    co->co_cell2arg = NULL;  // This will be set soon.
    co->co_nlocals = (int)PyTuple_GET_SIZE(con->varnames);
    co->co_ncellvars = (int)PyTuple_GET_SIZE(con->cellvars);
    co->co_nfreevars = (int)PyTuple_GET_SIZE(con->freevars);
    co->co_nlocalsplus = co->co_nlocals + co->co_ncellvars + co->co_nfreevars;

    /* not set */
    co->co_weakreflist = NULL;
    co->co_extra = NULL;
    co->co_opcache_map = NULL;
    co->co_opcache = NULL;
    co->co_opcache_flag = 0;
    co->co_opcache_size = 0;
}

/* The caller is responsible for ensuring that the given data is valid. */

PyCodeObject *
_PyCode_New(struct _PyCodeConstructor *con)
{
    /* Ensure that strings are ready Unicode string */
    if (PyUnicode_READY(con->name) < 0) {
        return NULL;
    }
    if (PyUnicode_READY(con->filename) < 0) {
        return NULL;
    }

    if (intern_strings(con->names) < 0) {
        return NULL;
    }
    if (intern_string_constants(con->consts, NULL) < 0) {
        return NULL;
    }
    if (intern_strings(con->varnames) < 0) {
        return NULL;
    }
    if (intern_strings(con->freevars) < 0) {
        return NULL;
    }
    if (intern_strings(con->cellvars) < 0) {
        return NULL;
    }

    /* Check for any inner or outer closure references */
    int ncellvars = (int)PyTuple_GET_SIZE(con->cellvars);
    if (!ncellvars && !PyTuple_GET_SIZE(con->freevars)) {
        con->flags |= CO_NOFREE;
    } else {
        con->flags &= ~CO_NOFREE;
    }

    PyCodeObject *co = PyObject_New(PyCodeObject, &PyCode_Type);
    if (co == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    init_code(co, con);

    /* Create mapping between cells and arguments if needed. */
    if (ncellvars) {
        int totalargs = co->co_argcount +
                        co->co_kwonlyargcount +
                        ((co->co_flags & CO_VARARGS) != 0) +
                        ((co->co_flags & CO_VARKEYWORDS) != 0);
        assert(totalargs <= co->co_nlocals);
        /* Find cells which are also arguments. */
        for (int i = 0; i < ncellvars; i++) {
            PyObject *cellname = PyTuple_GET_ITEM(co->co_cellvars, i);
            for (int j = 0; j < totalargs; j++) {
                PyObject *argname = PyTuple_GET_ITEM(co->co_varnames, j);
                int cmp = PyUnicode_Compare(cellname, argname);
                if (cmp == -1 && PyErr_Occurred()) {
                    Py_DECREF(co);
                    return NULL;
                }
                if (cmp == 0) {
                    if (co->co_cell2arg == NULL) {
                        co->co_cell2arg = PyMem_NEW(int, ncellvars);
                        if (co->co_cell2arg == NULL) {
                            Py_DECREF(co);
                            PyErr_NoMemory();
                            return NULL;
                        }
                        for (int k = 0; k < ncellvars; k++) {
                            co->co_cell2arg[k] = CO_CELL_NOT_AN_ARG;
                        }
                    }
                    co->co_cell2arg[i] = j;
                    // Go to the next cell name.
                    break;
                }
            }
        }
    }

    return co;
}


/******************
 * the legacy "constructors"
 ******************/

PyCodeObject *
PyCode_NewWithPosOnlyArgs(int argcount, int posonlyargcount, int kwonlyargcount,
                          int nlocals, int stacksize, int flags,
                          PyObject *code, PyObject *consts, PyObject *names,
                          PyObject *varnames, PyObject *freevars, PyObject *cellvars,
                          PyObject *filename, PyObject *name, int firstlineno,
                          PyObject *linetable, PyObject *exceptiontable)
{
    struct _PyCodeConstructor con = {
        .filename = filename,
        .name = name,
        .flags = flags,

        .code = code,
        .firstlineno = firstlineno,
        .linetable = linetable,

        .consts = consts,
        .names = names,

        .varnames = varnames,
        .cellvars = cellvars,
        .freevars = freevars,

        .argcount = argcount,
        .posonlyargcount = posonlyargcount,
        .kwonlyargcount = kwonlyargcount,

        .stacksize = stacksize,

        .exceptiontable = exceptiontable,
    };
    if (_PyCode_Validate(&con) < 0) {
        return NULL;
    }

    if (nlocals != PyTuple_GET_SIZE(varnames)) {
        PyErr_SetString(PyExc_ValueError,
                        "code: co_nlocals != len(co_varnames)");
        return NULL;
    }

    return _PyCode_New(&con);
}

PyCodeObject *
PyCode_New(int argcount, int kwonlyargcount,
           int nlocals, int stacksize, int flags,
           PyObject *code, PyObject *consts, PyObject *names,
           PyObject *varnames, PyObject *freevars, PyObject *cellvars,
           PyObject *filename, PyObject *name, int firstlineno,
           PyObject *linetable, PyObject *exceptiontable)
{
    return PyCode_NewWithPosOnlyArgs(argcount, 0, kwonlyargcount, nlocals,
                                     stacksize, flags, code, consts, names,
                                     varnames, freevars, cellvars, filename,
                                     name, firstlineno, linetable, exceptiontable);
}

PyCodeObject *
PyCode_NewEmpty(const char *filename, const char *funcname, int firstlineno)
{
    PyObject *emptystring = NULL;
    PyObject *nulltuple = NULL;
    PyObject *filename_ob = NULL;
    PyObject *funcname_ob = NULL;
    PyCodeObject *result = NULL;

    emptystring = PyBytes_FromString("");
    if (emptystring == NULL) {
        goto failed;
    }
    nulltuple = PyTuple_New(0);
    if (nulltuple == NULL) {
        goto failed;
    }
    funcname_ob = PyUnicode_FromString(funcname);
    if (funcname_ob == NULL) {
        goto failed;
    }
    filename_ob = PyUnicode_DecodeFSDefault(filename);
    if (filename_ob == NULL) {
        goto failed;
    }

    struct _PyCodeConstructor con = {
        .filename = filename_ob,
        .name = funcname_ob,
        .code = emptystring,
        .firstlineno = firstlineno,
        .linetable = emptystring,
        .consts = nulltuple,
        .names = nulltuple,
        .varnames = nulltuple,
        .cellvars = nulltuple,
        .freevars = nulltuple,
        .exceptiontable = emptystring,
    };
    result = _PyCode_New(&con);

failed:
    Py_XDECREF(emptystring);
    Py_XDECREF(nulltuple);
    Py_XDECREF(funcname_ob);
    Py_XDECREF(filename_ob);
    return result;
}


/******************
 * the line table (co_linetable)
 ******************/

/* Use co_linetable to compute the line number from a bytecode index, addrq.  See
   lnotab_notes.txt for the details of the lnotab representation.
*/

int
PyCode_Addr2Line(PyCodeObject *co, int addrq)
{
    if (addrq < 0) {
        return co->co_firstlineno;
    }
    assert(addrq >= 0 && addrq < PyBytes_GET_SIZE(co->co_code));
    PyCodeAddressRange bounds;
    _PyCode_InitAddressRange(co, &bounds);
    return _PyCode_CheckLineNumber(addrq, &bounds);
}

void
PyLineTable_InitAddressRange(char *linetable, Py_ssize_t length, int firstlineno, PyCodeAddressRange *range)
{
    range->opaque.lo_next = linetable;
    range->opaque.limit = range->opaque.lo_next + length;
    range->ar_start = -1;
    range->ar_end = 0;
    range->opaque.computed_line = firstlineno;
    range->ar_line = -1;
}

int
_PyCode_InitAddressRange(PyCodeObject* co, PyCodeAddressRange *bounds)
{
    char *linetable = PyBytes_AS_STRING(co->co_linetable);
    Py_ssize_t length = PyBytes_GET_SIZE(co->co_linetable);
    PyLineTable_InitAddressRange(linetable, length, co->co_firstlineno, bounds);
    return bounds->ar_line;
}

/* Update *bounds to describe the first and one-past-the-last instructions in
   the same line as lasti.  Return the number of that line, or -1 if lasti is out of bounds. */
int
_PyCode_CheckLineNumber(int lasti, PyCodeAddressRange *bounds)
{
    while (bounds->ar_end <= lasti) {
        if (!PyLineTable_NextAddressRange(bounds)) {
            return -1;
        }
    }
    while (bounds->ar_start > lasti) {
        if (!PyLineTable_PreviousAddressRange(bounds)) {
            return -1;
        }
    }
    return bounds->ar_line;
}

static void
retreat(PyCodeAddressRange *bounds)
{
    int ldelta = ((signed char *)bounds->opaque.lo_next)[-1];
    if (ldelta == -128) {
        ldelta = 0;
    }
    bounds->opaque.computed_line -= ldelta;
    bounds->opaque.lo_next -= 2;
    bounds->ar_end = bounds->ar_start;
    bounds->ar_start -= ((unsigned char *)bounds->opaque.lo_next)[-2];
    ldelta = ((signed char *)bounds->opaque.lo_next)[-1];
    if (ldelta == -128) {
        bounds->ar_line = -1;
    }
    else {
        bounds->ar_line = bounds->opaque.computed_line;
    }
}

static void
advance(PyCodeAddressRange *bounds)
{
    bounds->ar_start = bounds->ar_end;
    int delta = ((unsigned char *)bounds->opaque.lo_next)[0];
    bounds->ar_end += delta;
    int ldelta = ((signed char *)bounds->opaque.lo_next)[1];
    bounds->opaque.lo_next += 2;
    if (ldelta == -128) {
        bounds->ar_line = -1;
    }
    else {
        bounds->opaque.computed_line += ldelta;
        bounds->ar_line = bounds->opaque.computed_line;
    }
}

static inline int
at_end(PyCodeAddressRange *bounds) {
    return bounds->opaque.lo_next >= bounds->opaque.limit;
}

int
PyLineTable_PreviousAddressRange(PyCodeAddressRange *range)
{
    if (range->ar_start <= 0) {
        return 0;
    }
    retreat(range);
    while (range->ar_start == range->ar_end) {
        assert(range->ar_start > 0);
        retreat(range);
    }
    return 1;
}

int
PyLineTable_NextAddressRange(PyCodeAddressRange *range)
{
    if (at_end(range)) {
        return 0;
    }
    advance(range);
    while (range->ar_start == range->ar_end) {
        assert(!at_end(range));
        advance(range);
    }
    return 1;
}

static int
emit_pair(PyObject **bytes, int *offset, int a, int b)
{
    Py_ssize_t len = PyBytes_GET_SIZE(*bytes);
    if (*offset + 2 >= len) {
        if (_PyBytes_Resize(bytes, len * 2) < 0)
            return 0;
    }
    unsigned char *lnotab = (unsigned char *) PyBytes_AS_STRING(*bytes);
    lnotab += *offset;
    *lnotab++ = a;
    *lnotab++ = b;
    *offset += 2;
    return 1;
}

static int
emit_delta(PyObject **bytes, int bdelta, int ldelta, int *offset)
{
    while (bdelta > 255) {
        if (!emit_pair(bytes, offset, 255, 0)) {
            return 0;
        }
        bdelta -= 255;
    }
    while (ldelta > 127) {
        if (!emit_pair(bytes, offset, bdelta, 127)) {
            return 0;
        }
        bdelta = 0;
        ldelta -= 127;
    }
    while (ldelta < -128) {
        if (!emit_pair(bytes, offset, bdelta, -128)) {
            return 0;
        }
        bdelta = 0;
        ldelta += 128;
    }
    return emit_pair(bytes, offset, bdelta, ldelta);
}

static PyObject *
decode_linetable(PyCodeObject *code)
{
    PyCodeAddressRange bounds;
    PyObject *bytes;
    int table_offset = 0;
    int code_offset = 0;
    int line = code->co_firstlineno;
    bytes = PyBytes_FromStringAndSize(NULL, 64);
    if (bytes == NULL) {
        return NULL;
    }
    _PyCode_InitAddressRange(code, &bounds);
    while (PyLineTable_NextAddressRange(&bounds)) {
        if (bounds.opaque.computed_line != line) {
            int bdelta = bounds.ar_start - code_offset;
            int ldelta = bounds.opaque.computed_line - line;
            if (!emit_delta(&bytes, bdelta, ldelta, &table_offset)) {
                Py_DECREF(bytes);
                return NULL;
            }
            code_offset = bounds.ar_start;
            line = bounds.opaque.computed_line;
        }
    }
    _PyBytes_Resize(&bytes, table_offset);
    return bytes;
}


typedef struct {
    PyObject_HEAD
    PyCodeObject *li_code;
    PyCodeAddressRange li_line;
    char *li_end;
} lineiterator;


static void
lineiter_dealloc(lineiterator *li)
{
    Py_DECREF(li->li_code);
    Py_TYPE(li)->tp_free(li);
}

static PyObject *
lineiter_next(lineiterator *li)
{
    PyCodeAddressRange *bounds = &li->li_line;
    if (!PyLineTable_NextAddressRange(bounds)) {
        return NULL;
    }
    PyObject *start = NULL;
    PyObject *end = NULL;
    PyObject *line = NULL;
    PyObject *result = PyTuple_New(3);
    start = PyLong_FromLong(bounds->ar_start);
    end = PyLong_FromLong(bounds->ar_end);
    if (bounds->ar_line < 0) {
        Py_INCREF(Py_None);
        line = Py_None;
    }
    else {
        line = PyLong_FromLong(bounds->ar_line);
    }
    if (result == NULL || start == NULL || end == NULL || line == NULL) {
        goto error;
    }
    PyTuple_SET_ITEM(result, 0, start);
    PyTuple_SET_ITEM(result, 1, end);
    PyTuple_SET_ITEM(result, 2, line);
    return result;
error:
    Py_XDECREF(start);
    Py_XDECREF(end);
    Py_XDECREF(line);
    Py_XDECREF(result);
    return result;
}

static PyTypeObject LineIterator = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "line_iterator",                    /* tp_name */
    sizeof(lineiterator),               /* tp_basicsize */
    0,                                  /* tp_itemsize */
    /* methods */
    (destructor)lineiter_dealloc,       /* tp_dealloc */
    0,                                  /* tp_vectorcall_offset */
    0,                                  /* tp_getattr */
    0,                                  /* tp_setattr */
    0,                                  /* tp_as_async */
    0,                                  /* tp_repr */
    0,                                  /* tp_as_number */
    0,                                  /* tp_as_sequence */
    0,                                  /* tp_as_mapping */
    0,                                  /* tp_hash */
    0,                                  /* tp_call */
    0,                                  /* tp_str */
    0,                                  /* tp_getattro */
    0,                                  /* tp_setattro */
    0,                                  /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,       /* tp_flags */
    0,                                  /* tp_doc */
    0,                                  /* tp_traverse */
    0,                                  /* tp_clear */
    0,                                  /* tp_richcompare */
    0,                                  /* tp_weaklistoffset */
    PyObject_SelfIter,                  /* tp_iter */
    (iternextfunc)lineiter_next,        /* tp_iternext */
    0,                                  /* tp_methods */
    0,                                  /* tp_members */
    0,                                  /* tp_getset */
    0,                                  /* tp_base */
    0,                                  /* tp_dict */
    0,                                  /* tp_descr_get */
    0,                                  /* tp_descr_set */
    0,                                  /* tp_dictoffset */
    0,                                  /* tp_init */
    0,                                  /* tp_alloc */
    0,                                  /* tp_new */
    PyObject_Del,                       /* tp_free */
};

static lineiterator *
new_linesiterator(PyCodeObject *code)
{
    lineiterator *li = (lineiterator *)PyType_GenericAlloc(&LineIterator, 0);
    if (li == NULL) {
        return NULL;
    }
    Py_INCREF(code);
    li->li_code = code;
    _PyCode_InitAddressRange(code, &li->li_line);
    return li;
}


/******************
 * the opcache
 ******************/

int
_PyCode_InitOpcache(PyCodeObject *co)
{
    Py_ssize_t co_size = PyBytes_Size(co->co_code) / sizeof(_Py_CODEUNIT);
    co->co_opcache_map = (unsigned char *)PyMem_Calloc(co_size, 1);
    if (co->co_opcache_map == NULL) {
        return -1;
    }

    _Py_CODEUNIT *opcodes = (_Py_CODEUNIT*)PyBytes_AS_STRING(co->co_code);
    Py_ssize_t opts = 0;

    for (Py_ssize_t i = 0; i < co_size;) {
        unsigned char opcode = _Py_OPCODE(opcodes[i]);
        i++;  // 'i' is now aligned to (next_instr - first_instr)

        // TODO: LOAD_METHOD
        if (opcode == LOAD_GLOBAL || opcode == LOAD_ATTR) {
            opts++;
            co->co_opcache_map[i] = (unsigned char)opts;
            if (opts > 254) {
                break;
            }
        }
    }

    if (opts) {
        co->co_opcache = (_PyOpcache *)PyMem_Calloc(opts, sizeof(_PyOpcache));
        if (co->co_opcache == NULL) {
            PyMem_Free(co->co_opcache_map);
            return -1;
        }
    }
    else {
        PyMem_Free(co->co_opcache_map);
        co->co_opcache_map = NULL;
        co->co_opcache = NULL;
    }

    co->co_opcache_size = (unsigned char)opts;
    return 0;
}


/******************
 * "extra" frame eval info (see PEP 523)
 ******************/

/* Holder for co_extra information */
typedef struct {
    Py_ssize_t ce_size;
    void *ce_extras[1];
} _PyCodeObjectExtra;


int
_PyCode_GetExtra(PyObject *code, Py_ssize_t index, void **extra)
{
    if (!PyCode_Check(code)) {
        PyErr_BadInternalCall();
        return -1;
    }

    PyCodeObject *o = (PyCodeObject*) code;
    _PyCodeObjectExtra *co_extra = (_PyCodeObjectExtra*) o->co_extra;

    if (co_extra == NULL || co_extra->ce_size <= index) {
        *extra = NULL;
        return 0;
    }

    *extra = co_extra->ce_extras[index];
    return 0;
}


int
_PyCode_SetExtra(PyObject *code, Py_ssize_t index, void *extra)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();

    if (!PyCode_Check(code) || index < 0 ||
            index >= interp->co_extra_user_count) {
        PyErr_BadInternalCall();
        return -1;
    }

    PyCodeObject *o = (PyCodeObject*) code;
    _PyCodeObjectExtra *co_extra = (_PyCodeObjectExtra *) o->co_extra;

    if (co_extra == NULL || co_extra->ce_size <= index) {
        Py_ssize_t i = (co_extra == NULL ? 0 : co_extra->ce_size);
        co_extra = PyMem_Realloc(
                co_extra,
                sizeof(_PyCodeObjectExtra) +
                (interp->co_extra_user_count-1) * sizeof(void*));
        if (co_extra == NULL) {
            return -1;
        }
        for (; i < interp->co_extra_user_count; i++) {
            co_extra->ce_extras[i] = NULL;
        }
        co_extra->ce_size = interp->co_extra_user_count;
        o->co_extra = co_extra;
    }

    if (co_extra->ce_extras[index] != NULL) {
        freefunc free = interp->co_extra_freefuncs[index];
        if (free != NULL) {
            free(co_extra->ce_extras[index]);
        }
    }

    co_extra->ce_extras[index] = extra;
    return 0;
}


/******************
 * PyCode_Type
 ******************/

/*[clinic input]
class code "PyCodeObject *" "&PyCode_Type"
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=78aa5d576683bb4b]*/

/*[clinic input]
@classmethod
code.__new__ as code_new

    argcount: int
    posonlyargcount: int
    kwonlyargcount: int
    nlocals: int
    stacksize: int
    flags: int
    codestring as code: object(subclass_of="&PyBytes_Type")
    constants as consts: object(subclass_of="&PyTuple_Type")
    names: object(subclass_of="&PyTuple_Type")
    varnames: object(subclass_of="&PyTuple_Type")
    filename: unicode
    name: unicode
    firstlineno: int
    linetable: object(subclass_of="&PyBytes_Type")
    exceptiontable: object(subclass_of="&PyBytes_Type")
    freevars: object(subclass_of="&PyTuple_Type", c_default="NULL") = ()
    cellvars: object(subclass_of="&PyTuple_Type", c_default="NULL") = ()
    /

Create a code object.  Not for the faint of heart.
[clinic start generated code]*/

static PyObject *
code_new_impl(PyTypeObject *type, int argcount, int posonlyargcount,
              int kwonlyargcount, int nlocals, int stacksize, int flags,
              PyObject *code, PyObject *consts, PyObject *names,
              PyObject *varnames, PyObject *filename, PyObject *name,
              int firstlineno, PyObject *linetable, PyObject *exceptiontable,
              PyObject *freevars, PyObject *cellvars)
/*[clinic end generated code: output=a3899259c3b4cace input=f823c686da4b3a03]*/
{
    PyObject *co = NULL;
    PyObject *ournames = NULL;
    PyObject *ourvarnames = NULL;
    PyObject *ourfreevars = NULL;
    PyObject *ourcellvars = NULL;

    if (PySys_Audit("code.__new__", "OOOiiiiii",
                    code, filename, name, argcount, posonlyargcount,
                    kwonlyargcount, nlocals, stacksize, flags) < 0) {
        goto cleanup;
    }

    if (argcount < 0) {
        PyErr_SetString(
            PyExc_ValueError,
            "code: argcount must not be negative");
        goto cleanup;
    }

    if (posonlyargcount < 0) {
        PyErr_SetString(
            PyExc_ValueError,
            "code: posonlyargcount must not be negative");
        goto cleanup;
    }

    if (kwonlyargcount < 0) {
        PyErr_SetString(
            PyExc_ValueError,
            "code: kwonlyargcount must not be negative");
        goto cleanup;
    }
    if (nlocals < 0) {
        PyErr_SetString(
            PyExc_ValueError,
            "code: nlocals must not be negative");
        goto cleanup;
    }

    ournames = validate_and_copy_tuple(names);
    if (ournames == NULL)
        goto cleanup;
    ourvarnames = validate_and_copy_tuple(varnames);
    if (ourvarnames == NULL)
        goto cleanup;
    if (freevars)
        ourfreevars = validate_and_copy_tuple(freevars);
    else
        ourfreevars = PyTuple_New(0);
    if (ourfreevars == NULL)
        goto cleanup;
    if (cellvars)
        ourcellvars = validate_and_copy_tuple(cellvars);
    else
        ourcellvars = PyTuple_New(0);
    if (ourcellvars == NULL)
        goto cleanup;

    co = (PyObject *)PyCode_NewWithPosOnlyArgs(argcount, posonlyargcount,
                                               kwonlyargcount,
                                               nlocals, stacksize, flags,
                                               code, consts, ournames,
                                               ourvarnames, ourfreevars,
                                               ourcellvars, filename,
                                               name, firstlineno, linetable,
                                               exceptiontable
                                              );
  cleanup:
    Py_XDECREF(ournames);
    Py_XDECREF(ourvarnames);
    Py_XDECREF(ourfreevars);
    Py_XDECREF(ourcellvars);
    return co;
}

static void
code_dealloc(PyCodeObject *co)
{
    if (co->co_opcache != NULL) {
        PyMem_Free(co->co_opcache);
    }
    if (co->co_opcache_map != NULL) {
        PyMem_Free(co->co_opcache_map);
    }
    co->co_opcache_flag = 0;
    co->co_opcache_size = 0;

    if (co->co_extra != NULL) {
        PyInterpreterState *interp = _PyInterpreterState_GET();
        _PyCodeObjectExtra *co_extra = co->co_extra;

        for (Py_ssize_t i = 0; i < co_extra->ce_size; i++) {
            freefunc free_extra = interp->co_extra_freefuncs[i];

            if (free_extra != NULL) {
                free_extra(co_extra->ce_extras[i]);
            }
        }

        PyMem_Free(co_extra);
    }

    Py_XDECREF(co->co_code);
    Py_XDECREF(co->co_consts);
    Py_XDECREF(co->co_names);
    Py_XDECREF(co->co_varnames);
    Py_XDECREF(co->co_freevars);
    Py_XDECREF(co->co_cellvars);
    Py_XDECREF(co->co_filename);
    Py_XDECREF(co->co_name);
    Py_XDECREF(co->co_linetable);
    Py_XDECREF(co->co_exceptiontable);
    if (co->co_cell2arg != NULL)
        PyMem_Free(co->co_cell2arg);
    if (co->co_weakreflist != NULL)
        PyObject_ClearWeakRefs((PyObject*)co);
    PyObject_Free(co);
}

static PyObject *
code_repr(PyCodeObject *co)
{
    int lineno;
    if (co->co_firstlineno != 0)
        lineno = co->co_firstlineno;
    else
        lineno = -1;
    if (co->co_filename && PyUnicode_Check(co->co_filename)) {
        return PyUnicode_FromFormat(
            "<code object %U at %p, file \"%U\", line %d>",
            co->co_name, co, co->co_filename, lineno);
    } else {
        return PyUnicode_FromFormat(
            "<code object %U at %p, file ???, line %d>",
            co->co_name, co, lineno);
    }
}

static PyObject *
code_richcompare(PyObject *self, PyObject *other, int op)
{
    PyCodeObject *co, *cp;
    int eq;
    PyObject *consts1, *consts2;
    PyObject *res;

    if ((op != Py_EQ && op != Py_NE) ||
        !PyCode_Check(self) ||
        !PyCode_Check(other)) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    co = (PyCodeObject *)self;
    cp = (PyCodeObject *)other;

    eq = PyObject_RichCompareBool(co->co_name, cp->co_name, Py_EQ);
    if (!eq) goto unequal;
    eq = co->co_argcount == cp->co_argcount;
    if (!eq) goto unequal;
    eq = co->co_posonlyargcount == cp->co_posonlyargcount;
    if (!eq) goto unequal;
    eq = co->co_kwonlyargcount == cp->co_kwonlyargcount;
    if (!eq) goto unequal;
    eq = co->co_nlocals == cp->co_nlocals;
    if (!eq) goto unequal;
    eq = co->co_flags == cp->co_flags;
    if (!eq) goto unequal;
    eq = co->co_firstlineno == cp->co_firstlineno;
    if (!eq) goto unequal;
    eq = PyObject_RichCompareBool(co->co_code, cp->co_code, Py_EQ);
    if (eq <= 0) goto unequal;

    /* compare constants */
    consts1 = _PyCode_ConstantKey(co->co_consts);
    if (!consts1)
        return NULL;
    consts2 = _PyCode_ConstantKey(cp->co_consts);
    if (!consts2) {
        Py_DECREF(consts1);
        return NULL;
    }
    eq = PyObject_RichCompareBool(consts1, consts2, Py_EQ);
    Py_DECREF(consts1);
    Py_DECREF(consts2);
    if (eq <= 0) goto unequal;

    eq = PyObject_RichCompareBool(co->co_names, cp->co_names, Py_EQ);
    if (eq <= 0) goto unequal;
    eq = PyObject_RichCompareBool(co->co_varnames, cp->co_varnames, Py_EQ);
    if (eq <= 0) goto unequal;
    eq = PyObject_RichCompareBool(co->co_freevars, cp->co_freevars, Py_EQ);
    if (eq <= 0) goto unequal;
    eq = PyObject_RichCompareBool(co->co_cellvars, cp->co_cellvars, Py_EQ);
    if (eq <= 0) goto unequal;

    if (op == Py_EQ)
        res = Py_True;
    else
        res = Py_False;
    goto done;

  unequal:
    if (eq < 0)
        return NULL;
    if (op == Py_NE)
        res = Py_True;
    else
        res = Py_False;

  done:
    Py_INCREF(res);
    return res;
}

static Py_hash_t
code_hash(PyCodeObject *co)
{
    Py_hash_t h, h0, h1, h2, h3, h4, h5, h6;
    h0 = PyObject_Hash(co->co_name);
    if (h0 == -1) return -1;
    h1 = PyObject_Hash(co->co_code);
    if (h1 == -1) return -1;
    h2 = PyObject_Hash(co->co_consts);
    if (h2 == -1) return -1;
    h3 = PyObject_Hash(co->co_names);
    if (h3 == -1) return -1;
    h4 = PyObject_Hash(co->co_varnames);
    if (h4 == -1) return -1;
    h5 = PyObject_Hash(co->co_freevars);
    if (h5 == -1) return -1;
    h6 = PyObject_Hash(co->co_cellvars);
    if (h6 == -1) return -1;
    h = h0 ^ h1 ^ h2 ^ h3 ^ h4 ^ h5 ^ h6 ^
        co->co_argcount ^ co->co_posonlyargcount ^ co->co_kwonlyargcount ^
        co->co_nlocals ^ co->co_flags;
    if (h == -1) h = -2;
    return h;
}


#define OFF(x) offsetof(PyCodeObject, x)

static PyMemberDef code_memberlist[] = {
    {"co_argcount",     T_INT,          OFF(co_argcount),        READONLY},
    {"co_posonlyargcount",      T_INT,  OFF(co_posonlyargcount), READONLY},
    {"co_kwonlyargcount",       T_INT,  OFF(co_kwonlyargcount),  READONLY},
    {"co_nlocals",      T_INT,          OFF(co_nlocals),         READONLY},
    {"co_stacksize",T_INT,              OFF(co_stacksize),       READONLY},
    {"co_flags",        T_INT,          OFF(co_flags),           READONLY},
    {"co_code",         T_OBJECT,       OFF(co_code),            READONLY},
    {"co_consts",       T_OBJECT,       OFF(co_consts),          READONLY},
    {"co_names",        T_OBJECT,       OFF(co_names),           READONLY},
    {"co_varnames",     T_OBJECT,       OFF(co_varnames),        READONLY},
    {"co_freevars",     T_OBJECT,       OFF(co_freevars),        READONLY},
    {"co_cellvars",     T_OBJECT,       OFF(co_cellvars),        READONLY},
    {"co_filename",     T_OBJECT,       OFF(co_filename),        READONLY},
    {"co_name",         T_OBJECT,       OFF(co_name),            READONLY},
    {"co_firstlineno",  T_INT,          OFF(co_firstlineno),     READONLY},
    {"co_linetable",    T_OBJECT,       OFF(co_linetable),       READONLY},
    {"co_exceptiontable",    T_OBJECT,  OFF(co_exceptiontable),  READONLY},
    {NULL}      /* Sentinel */
};



static PyObject *
code_getlnotab(PyCodeObject *code, void *closure)
{
    return decode_linetable(code);
}

static PyGetSetDef code_getsetlist[] = {
    {"co_lnotab",    (getter)code_getlnotab, NULL, NULL},
    {0}
};


static PyObject *
code_sizeof(PyCodeObject *co, PyObject *Py_UNUSED(args))
{
    Py_ssize_t res = _PyObject_SIZE(Py_TYPE(co));
    _PyCodeObjectExtra *co_extra = (_PyCodeObjectExtra*) co->co_extra;

    if (co->co_cell2arg != NULL && co->co_cellvars != NULL) {
        res += co->co_ncellvars * sizeof(Py_ssize_t);
    }
    if (co_extra != NULL) {
        res += sizeof(_PyCodeObjectExtra) +
               (co_extra->ce_size-1) * sizeof(co_extra->ce_extras[0]);
    }
    if (co->co_opcache != NULL) {
        assert(co->co_opcache_map != NULL);
        // co_opcache_map
        res += PyBytes_GET_SIZE(co->co_code) / sizeof(_Py_CODEUNIT);
        // co_opcache
        res += co->co_opcache_size * sizeof(_PyOpcache);
    }
    return PyLong_FromSsize_t(res);
}

static PyObject *
code_linesiterator(PyCodeObject *code, PyObject *Py_UNUSED(args))
{
    return (PyObject *)new_linesiterator(code);
}

/*[clinic input]
code.replace

    *
    co_argcount: int(c_default="self->co_argcount") = -1
    co_posonlyargcount: int(c_default="self->co_posonlyargcount") = -1
    co_kwonlyargcount: int(c_default="self->co_kwonlyargcount") = -1
    co_nlocals: int(c_default="self->co_nlocals") = -1
    co_stacksize: int(c_default="self->co_stacksize") = -1
    co_flags: int(c_default="self->co_flags") = -1
    co_firstlineno: int(c_default="self->co_firstlineno") = -1
    co_code: PyBytesObject(c_default="(PyBytesObject *)self->co_code") = None
    co_consts: object(subclass_of="&PyTuple_Type", c_default="self->co_consts") = None
    co_names: object(subclass_of="&PyTuple_Type", c_default="self->co_names") = None
    co_varnames: object(subclass_of="&PyTuple_Type", c_default="self->co_varnames") = None
    co_freevars: object(subclass_of="&PyTuple_Type", c_default="self->co_freevars") = None
    co_cellvars: object(subclass_of="&PyTuple_Type", c_default="self->co_cellvars") = None
    co_filename: unicode(c_default="self->co_filename") = None
    co_name: unicode(c_default="self->co_name") = None
    co_linetable: PyBytesObject(c_default="(PyBytesObject *)self->co_linetable") = None
    co_exceptiontable: PyBytesObject(c_default="(PyBytesObject *)self->co_exceptiontable") = None

Return a copy of the code object with new values for the specified fields.
[clinic start generated code]*/

static PyObject *
code_replace_impl(PyCodeObject *self, int co_argcount,
                  int co_posonlyargcount, int co_kwonlyargcount,
                  int co_nlocals, int co_stacksize, int co_flags,
                  int co_firstlineno, PyBytesObject *co_code,
                  PyObject *co_consts, PyObject *co_names,
                  PyObject *co_varnames, PyObject *co_freevars,
                  PyObject *co_cellvars, PyObject *co_filename,
                  PyObject *co_name, PyBytesObject *co_linetable,
                  PyBytesObject *co_exceptiontable)
/*[clinic end generated code: output=80957472b7f78ed6 input=38376b1193efbbae]*/
{
#define CHECK_INT_ARG(ARG) \
        if (ARG < 0) { \
            PyErr_SetString(PyExc_ValueError, \
                            #ARG " must be a positive integer"); \
            return NULL; \
        }

    CHECK_INT_ARG(co_argcount);
    CHECK_INT_ARG(co_posonlyargcount);
    CHECK_INT_ARG(co_kwonlyargcount);
    CHECK_INT_ARG(co_nlocals);
    CHECK_INT_ARG(co_stacksize);
    CHECK_INT_ARG(co_flags);
    CHECK_INT_ARG(co_firstlineno);

#undef CHECK_INT_ARG

    if (PySys_Audit("code.__new__", "OOOiiiiii",
                    co_code, co_filename, co_name, co_argcount,
                    co_posonlyargcount, co_kwonlyargcount, co_nlocals,
                    co_stacksize, co_flags) < 0) {
        return NULL;
    }

    return (PyObject *)PyCode_NewWithPosOnlyArgs(
        co_argcount, co_posonlyargcount, co_kwonlyargcount, co_nlocals,
        co_stacksize, co_flags, (PyObject*)co_code, co_consts, co_names,
        co_varnames, co_freevars, co_cellvars, co_filename, co_name,
        co_firstlineno, (PyObject*)co_linetable, (PyObject*)co_exceptiontable);
}

/* XXX code objects need to participate in GC? */

static struct PyMethodDef code_methods[] = {
    {"__sizeof__", (PyCFunction)code_sizeof, METH_NOARGS},
    {"co_lines", (PyCFunction)code_linesiterator, METH_NOARGS},
    CODE_REPLACE_METHODDEF
    {NULL, NULL}                /* sentinel */
};


PyTypeObject PyCode_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "code",
    sizeof(PyCodeObject),
    0,
    (destructor)code_dealloc,           /* tp_dealloc */
    0,                                  /* tp_vectorcall_offset */
    0,                                  /* tp_getattr */
    0,                                  /* tp_setattr */
    0,                                  /* tp_as_async */
    (reprfunc)code_repr,                /* tp_repr */
    0,                                  /* tp_as_number */
    0,                                  /* tp_as_sequence */
    0,                                  /* tp_as_mapping */
    (hashfunc)code_hash,                /* tp_hash */
    0,                                  /* tp_call */
    0,                                  /* tp_str */
    PyObject_GenericGetAttr,            /* tp_getattro */
    0,                                  /* tp_setattro */
    0,                                  /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                 /* tp_flags */
    code_new__doc__,                    /* tp_doc */
    0,                                  /* tp_traverse */
    0,                                  /* tp_clear */
    code_richcompare,                   /* tp_richcompare */
    offsetof(PyCodeObject, co_weakreflist),     /* tp_weaklistoffset */
    0,                                  /* tp_iter */
    0,                                  /* tp_iternext */
    code_methods,                       /* tp_methods */
    code_memberlist,                    /* tp_members */
    code_getsetlist,                    /* tp_getset */
    0,                                  /* tp_base */
    0,                                  /* tp_dict */
    0,                                  /* tp_descr_get */
    0,                                  /* tp_descr_set */
    0,                                  /* tp_dictoffset */
    0,                                  /* tp_init */
    0,                                  /* tp_alloc */
    code_new,                           /* tp_new */
};


/******************
 * other API
 ******************/

PyObject*
_PyCode_ConstantKey(PyObject *op)
{
    PyObject *key;

    /* Py_None and Py_Ellipsis are singletons. */
    if (op == Py_None || op == Py_Ellipsis
       || PyLong_CheckExact(op)
       || PyUnicode_CheckExact(op)
          /* code_richcompare() uses _PyCode_ConstantKey() internally */
       || PyCode_Check(op))
    {
        /* Objects of these types are always different from object of other
         * type and from tuples. */
        Py_INCREF(op);
        key = op;
    }
    else if (PyBool_Check(op) || PyBytes_CheckExact(op)) {
        /* Make booleans different from integers 0 and 1.
         * Avoid BytesWarning from comparing bytes with strings. */
        key = PyTuple_Pack(2, Py_TYPE(op), op);
    }
    else if (PyFloat_CheckExact(op)) {
        double d = PyFloat_AS_DOUBLE(op);
        /* all we need is to make the tuple different in either the 0.0
         * or -0.0 case from all others, just to avoid the "coercion".
         */
        if (d == 0.0 && copysign(1.0, d) < 0.0)
            key = PyTuple_Pack(3, Py_TYPE(op), op, Py_None);
        else
            key = PyTuple_Pack(2, Py_TYPE(op), op);
    }
    else if (PyComplex_CheckExact(op)) {
        Py_complex z;
        int real_negzero, imag_negzero;
        /* For the complex case we must make complex(x, 0.)
           different from complex(x, -0.) and complex(0., y)
           different from complex(-0., y), for any x and y.
           All four complex zeros must be distinguished.*/
        z = PyComplex_AsCComplex(op);
        real_negzero = z.real == 0.0 && copysign(1.0, z.real) < 0.0;
        imag_negzero = z.imag == 0.0 && copysign(1.0, z.imag) < 0.0;
        /* use True, False and None singleton as tags for the real and imag
         * sign, to make tuples different */
        if (real_negzero && imag_negzero) {
            key = PyTuple_Pack(3, Py_TYPE(op), op, Py_True);
        }
        else if (imag_negzero) {
            key = PyTuple_Pack(3, Py_TYPE(op), op, Py_False);
        }
        else if (real_negzero) {
            key = PyTuple_Pack(3, Py_TYPE(op), op, Py_None);
        }
        else {
            key = PyTuple_Pack(2, Py_TYPE(op), op);
        }
    }
    else if (PyTuple_CheckExact(op)) {
        Py_ssize_t i, len;
        PyObject *tuple;

        len = PyTuple_GET_SIZE(op);
        tuple = PyTuple_New(len);
        if (tuple == NULL)
            return NULL;

        for (i=0; i < len; i++) {
            PyObject *item, *item_key;

            item = PyTuple_GET_ITEM(op, i);
            item_key = _PyCode_ConstantKey(item);
            if (item_key == NULL) {
                Py_DECREF(tuple);
                return NULL;
            }

            PyTuple_SET_ITEM(tuple, i, item_key);
        }

        key = PyTuple_Pack(2, tuple, op);
        Py_DECREF(tuple);
    }
    else if (PyFrozenSet_CheckExact(op)) {
        Py_ssize_t pos = 0;
        PyObject *item;
        Py_hash_t hash;
        Py_ssize_t i, len;
        PyObject *tuple, *set;

        len = PySet_GET_SIZE(op);
        tuple = PyTuple_New(len);
        if (tuple == NULL)
            return NULL;

        i = 0;
        while (_PySet_NextEntry(op, &pos, &item, &hash)) {
            PyObject *item_key;

            item_key = _PyCode_ConstantKey(item);
            if (item_key == NULL) {
                Py_DECREF(tuple);
                return NULL;
            }

            assert(i < len);
            PyTuple_SET_ITEM(tuple, i, item_key);
            i++;
        }
        set = PyFrozenSet_New(tuple);
        Py_DECREF(tuple);
        if (set == NULL)
            return NULL;

        key = PyTuple_Pack(2, set, op);
        Py_DECREF(set);
        return key;
    }
    else {
        /* for other types, use the object identifier as a unique identifier
         * to ensure that they are seen as unequal. */
        PyObject *obj_id = PyLong_FromVoidPtr(op);
        if (obj_id == NULL)
            return NULL;

        key = PyTuple_Pack(2, obj_id, op);
        Py_DECREF(obj_id);
    }
    return key;
}
