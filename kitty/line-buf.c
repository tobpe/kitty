/*
 * line-buf.c
 * Copyright (C) 2016 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "data-types.h"
#include <structmember.h>

static inline void
clear_chars_to(LineBuf* linebuf, index_type y, char_type ch) {
    char_type *chars = linebuf->chars + linebuf->xnum * y;
    for (index_type i = 0; i < linebuf->xnum; i++) chars[i] = (1 << ATTRS_SHIFT) | ch;
}

void linebuf_clear(LineBuf *self, char_type ch) {
    memset(self->buf, 0, self->block_size * CELL_SIZE);
    memset(self->continued_map, 0, self->ynum * sizeof(bool));
    for (index_type i = 0; i < self->ynum; i++) {
        clear_chars_to(self, i, ch);
        self->line_map[i] = i;
    }
}

static PyObject*
clear(LineBuf *self) {
#define clear_doc "Clear all lines in this LineBuf"
    linebuf_clear(self, ' ');
    Py_RETURN_NONE;
}

static PyObject *
new(PyTypeObject *type, PyObject *args, PyObject UNUSED *kwds) {
    LineBuf *self;
    unsigned int xnum = 1, ynum = 1;

    if (!PyArg_ParseTuple(args, "II", &ynum, &xnum)) return NULL;

    if (xnum > 5000 || ynum > 50000) {
        PyErr_SetString(PyExc_ValueError, "Number of rows or columns is too large.");
        return NULL;
    }

    if (xnum * ynum == 0) {
        PyErr_SetString(PyExc_ValueError, "Cannot create an empty LineBuf");
        return NULL;
    }

    self = (LineBuf *)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->xnum = xnum;
        self->ynum = ynum;
        self->block_size = xnum * ynum;
        self->buf = PyMem_Calloc(xnum * ynum, CELL_SIZE);
        self->line_map = PyMem_Calloc(ynum, sizeof(index_type));
        self->scratch = PyMem_Calloc(ynum, sizeof(index_type));
        self->continued_map = PyMem_Calloc(ynum, sizeof(bool));
        self->line = alloc_line();
        if (self->buf == NULL || self->line_map == NULL || self->scratch == NULL || self->continued_map == NULL || self->line == NULL) {
            PyErr_NoMemory();
            PyMem_Free(self->buf); PyMem_Free(self->line_map); PyMem_Free(self->continued_map); Py_CLEAR(self->line);
            Py_CLEAR(self);
        } else {
            self->chars = (char_type*)self->buf;
            self->fg_colors = (color_type*)(self->chars + self->block_size);
            self->bg_colors = (color_type*)(self->fg_colors + self->block_size);
            self->decoration_fg = (color_type*)(self->bg_colors + self->block_size);
            self->combining_chars = (combining_type*)(self->decoration_fg + self->block_size);
            self->line->xnum = xnum;
            for(index_type i = 0; i < ynum; i++) {
                self->line_map[i] = i;
                clear_chars_to(self, i, ' ');
            }
        }
    }

    return (PyObject*)self;
}

static void
dealloc(LineBuf* self) {
    PyMem_Free(self->buf);
    PyMem_Free(self->line_map); 
    PyMem_Free(self->continued_map); 
    PyMem_Free(self->scratch);
    Py_CLEAR(self->line);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

#define INIT_LINE(lb, l, ynum) \
    (l)->chars           = (lb)->chars + (ynum) * (lb)->xnum; \
    (l)->fg_colors          = (lb)->fg_colors + (ynum) * (lb)->xnum; \
    (l)->bg_colors          = (lb)->bg_colors + (ynum) * (lb)->xnum; \
    (l)->decoration_fg   = (lb)->decoration_fg + (ynum) * (lb)->xnum; \
    (l)->combining_chars = (lb)->combining_chars + (ynum) * (lb)->xnum;

void linebuf_init_line(LineBuf *self, index_type idx) {
    self->line->ynum = idx;
    self->line->xnum = self->xnum;
    self->line->continued = self->continued_map[idx];
    INIT_LINE(self, self->line, self->line_map[idx]);
}

static PyObject*
line(LineBuf *self, PyObject *y) {
#define line_doc      "Return the specified line as a Line object. Note the Line Object is a live view into the underlying buffer. And only a single line object can be used at a time."
    unsigned long idx = PyLong_AsUnsignedLong(y);
    if (idx >= self->ynum) {
        PyErr_SetString(PyExc_IndexError, "Line number too large");
        return NULL;
    }
    linebuf_init_line(self, idx);
    Py_INCREF(self->line);
    return (PyObject*)self->line;
}

unsigned int linebuf_char_width_at(LineBuf *self, index_type x, index_type y) {
    char_type *chars = self->chars + self->line_map[y] * self->xnum;
    return (chars[x] >> ATTRS_SHIFT) & WIDTH_MASK;
}

void linebuf_set_attribute(LineBuf *self, unsigned int shift, unsigned int val) {
    char_type mask;
    for (index_type y = 0; y < self->ynum; y++) {
        SET_ATTRIBUTE(self->chars + y * self->xnum, shift, val);
    }
}

static PyObject*
set_attribute(LineBuf *self, PyObject *args) {
#define set_attribute_doc "set_attribute(which, val) -> Set the attribute on all cells in the line."
    unsigned int shift, val;
    if (!PyArg_ParseTuple(args, "II", &shift, &val)) return NULL;
    if (shift < DECORATION_SHIFT || shift > STRIKE_SHIFT) { PyErr_SetString(PyExc_ValueError, "Unknown attribute"); return NULL; }
    linebuf_set_attribute(self, shift, val);
    Py_RETURN_NONE;
}

static PyObject*
set_continued(LineBuf *self, PyObject *args) {
#define set_continued_doc "set_continued(y, val) -> Set the continued values for the specified line."
    unsigned int y;
    int val;
    if (!PyArg_ParseTuple(args, "Ip", &y, &val)) return NULL;
    if (y >= self->ynum) { PyErr_SetString(PyExc_ValueError, "Out of bounds."); return NULL; }
    self->continued_map[y] = val & 1;
    Py_RETURN_NONE;
}

static inline int
allocate_line_storage(Line *line, bool initialize) {
    if (initialize) {
        line->chars = PyMem_Calloc(line->xnum, sizeof(char_type));
        line->fg_colors = PyMem_Calloc(line->xnum, sizeof(color_type));
        line->bg_colors = PyMem_Calloc(line->xnum, sizeof(color_type));
        line->decoration_fg = PyMem_Calloc(line->xnum, sizeof(color_type));
        line->combining_chars = PyMem_Calloc(line->xnum, sizeof(combining_type));
        for (index_type i = 0; i < line->xnum; i++) line->chars[i] = (1 << ATTRS_SHIFT) | 32;
    } else {
        line->chars = PyMem_Malloc(line->xnum * sizeof(char_type));
        line->fg_colors = PyMem_Malloc(line->xnum * sizeof(color_type));
        line->bg_colors = PyMem_Malloc(line->xnum * sizeof(color_type));
        line->decoration_fg = PyMem_Malloc(line->xnum * sizeof(color_type));
        line->combining_chars = PyMem_Malloc(line->xnum * sizeof(combining_type));
    }
    if (line->chars == NULL || line->fg_colors == NULL || line->bg_colors == NULL || line->decoration_fg == NULL || line->combining_chars == NULL) {
        PyMem_Free(line->chars); line->chars = NULL;
        PyMem_Free(line->fg_colors); line->fg_colors = NULL;
        PyMem_Free(line->bg_colors); line->bg_colors = NULL;
        PyMem_Free(line->decoration_fg); line->decoration_fg = NULL;
        PyMem_Free(line->combining_chars); line->combining_chars = NULL;
        PyErr_NoMemory();
        return 0;
    }
    line->needs_free = 1;
    return 1;
}

static inline PyObject* create_line_copy_inner(LineBuf* self, index_type y) {
    Line src, *line;
    line = alloc_line();
    if (line == NULL) return PyErr_NoMemory();
    src.xnum = self->xnum; line->xnum = self->xnum;
    if (!allocate_line_storage(line, 0)) { Py_CLEAR(line); return PyErr_NoMemory(); }
    line->ynum = y;
    line->continued = self->continued_map[y];
    INIT_LINE(self, &src, self->line_map[y]);
    COPY_LINE(&src, line);
    return (PyObject*)line;
}

static PyObject*
create_line_copy(LineBuf *self, PyObject *ynum) {
#define create_line_copy_doc "Create a new Line object that is a copy of the line at ynum. Note that this line has its own copy of the data and does not refer to the data in the LineBuf."
    index_type y = (index_type)PyLong_AsUnsignedLong(ynum);
    if (y >= self->ynum) { PyErr_SetString(PyExc_ValueError, "Out of bounds"); return NULL; }
    return create_line_copy_inner(self, y);
}

static PyObject*
copy_line_to(LineBuf *self, PyObject *args) {
#define copy_line_to_doc "Copy the line at ynum to the provided line object."
    unsigned int y;
    Line src, *dest;
    if (!PyArg_ParseTuple(args, "IO!", &y, &Line_Type, &dest)) return NULL;
    src.xnum = self->xnum; dest->xnum = self->xnum;
    dest->ynum = y;
    dest->continued = self->continued_map[y];
    INIT_LINE(self, &src, self->line_map[y]);
    COPY_LINE(&src, dest);
    Py_RETURN_NONE;
}

void linebuf_clear_line(LineBuf *self, index_type y) {
    Line l;
    INIT_LINE(self, &l, self->line_map[y]);
    CLEAR_LINE(&l, self->xnum);
    self->continued_map[y] = 0;
}

static PyObject*
clear_line(LineBuf *self, PyObject *val) {
#define clear_line_doc "clear_line(y) -> Clear the specified line"
    index_type y = (index_type)PyLong_AsUnsignedLong(val);
    if (y >= self->ynum) { PyErr_SetString(PyExc_ValueError, "Out of bounds"); return NULL; }
    linebuf_clear_line(self, y);
    Py_RETURN_NONE;
}

void linebuf_index(LineBuf* self, index_type top, index_type bottom) {
    if (top >= self->ynum - 1 || bottom >= self->ynum || bottom <= top) return;
    index_type old_top = self->line_map[top];
    bool old_cont = self->continued_map[top];
    for (index_type i = top; i < bottom; i++) {
        self->line_map[i] = self->line_map[i + 1];
        self->continued_map[i] = self->continued_map[i + 1];
    }
    self->line_map[bottom] = old_top;
    self->continued_map[bottom] = old_cont;
}

static PyObject*
index(LineBuf *self, PyObject *args) {
#define index_doc "index(top, bottom) -> Scroll all lines in the range [top, bottom] by one upwards. After scrolling, bottom will be top."
    unsigned int top, bottom;
    if (!PyArg_ParseTuple(args, "II", &top, &bottom)) return NULL;
    linebuf_index(self, top, bottom);
    Py_RETURN_NONE;
}

void linebuf_reverse_index(LineBuf *self, index_type top, index_type bottom) {
    if (top >= self->ynum - 1 || bottom >= self->ynum || bottom <= top) return;
    index_type old_bottom = self->line_map[bottom];
    bool old_cont = self->continued_map[bottom];
    for (index_type i = bottom; i > top; i--) {
        self->line_map[i] = self->line_map[i - 1];
        self->continued_map[i] = self->continued_map[i - 1];
    }
    self->line_map[top] = old_bottom;
    self->continued_map[top] = old_cont;
}

static PyObject*
reverse_index(LineBuf *self, PyObject *args) {
#define reverse_index_doc "reverse_index(top, bottom) -> Scroll all lines in the range [top, bottom] by one down. After scrolling, top will be bottom."
    unsigned int top, bottom;
    if (!PyArg_ParseTuple(args, "II", &top, &bottom)) return NULL;
    linebuf_reverse_index(self, top, bottom);
    Py_RETURN_NONE;
}


static PyObject*
is_continued(LineBuf *self, PyObject *val) {
#define is_continued_doc "is_continued(y) -> Whether the line y is continued or not"
    unsigned long y = PyLong_AsUnsignedLong(val);
    if (y >= self->ynum) { PyErr_SetString(PyExc_ValueError, "Out of bounds."); return NULL; }
    if (self->continued_map[y]) { Py_RETURN_TRUE; }
    Py_RETURN_FALSE;
}

void linebuf_insert_lines(LineBuf *self, unsigned int num, unsigned int y, unsigned int bottom) {
    index_type i;
    if (y >= self->ynum || y > bottom || bottom >= self->ynum) return;
    index_type ylimit = bottom + 1;
    num = MIN(ylimit - y, num);
    if (num > 0) {
        for (i = ylimit - num; i < ylimit; i++) {
            self->scratch[i] = self->line_map[i];
        }
        for (i = ylimit - 1; i >= y + num; i--) {
            self->line_map[i] = self->line_map[i - num];
            self->continued_map[i] = self->continued_map[i - num];
        }
        if (y + num < self->ynum) self->continued_map[y + num] = 0;
        for (i = 0; i < num; i++) {
            self->line_map[y + i] = self->scratch[ylimit - num + i];
        }
        Line l;
        for (i = y; i < y + num; i++) {
            INIT_LINE(self, &l, self->line_map[i]);
            CLEAR_LINE(&l, self->xnum);
            self->continued_map[i] = 0;
        }
    }
}

static PyObject*
insert_lines(LineBuf *self, PyObject *args) {
#define insert_lines_doc "insert_lines(num, y, bottom) -> Insert num blank lines at y, only changing lines in the range [y, bottom]."
    unsigned int y, num, bottom;
    if (!PyArg_ParseTuple(args, "III", &num, &y, &bottom)) return NULL;
    linebuf_insert_lines(self, num, y, bottom);
    Py_RETURN_NONE;
}

void 
linebuf_delete_lines(LineBuf *self, index_type num, index_type y, index_type bottom) {
    index_type i;
    index_type ylimit = bottom + 1;
    num = MIN(bottom + 1 - y, num);
    if (y >= self->ynum || y > bottom || bottom >= self->ynum || num < 1) return;
    for (i = y; i < y + num; i++) {
        self->scratch[i] = self->line_map[i];
    }
    for (i = y; i < ylimit && i + num < self->ynum; i++) {
        self->line_map[i] = self->line_map[i + num];
        self->continued_map[i] = self->continued_map[i + num];
    }
    self->continued_map[y] = 0;
    for (i = 0; i < num; i++) {
        self->line_map[ylimit - num + i] = self->scratch[y + i];
    }
    Line l;
    for (i = ylimit - num; i < ylimit; i++) {
        INIT_LINE(self, &l, self->line_map[i]);
        CLEAR_LINE(&l, self->xnum);
        self->continued_map[i] = 0;
    }
}
 
static PyObject*
delete_lines(LineBuf *self, PyObject *args) {
#define delete_lines_doc "delete_lines(num, y, bottom) -> Delete num blank lines at y, only changing lines in the range [y, bottom]."
    unsigned int y, num, bottom;
    if (!PyArg_ParseTuple(args, "III", &num, &y, &bottom)) return NULL;
    linebuf_delete_lines(self, num, y, bottom);
    Py_RETURN_NONE;
}
 
static PyObject*
as_ansi(LineBuf *self, PyObject *callback) {
#define as_ansi_doc "as_ansi(callback) -> The contents of this buffer as ANSI escaped text. callback is called with each successive line."
    static Py_UCS4 t[5120];
    Line l = {.xnum=self->xnum};
    for(index_type i = 0; i < self->ynum; i++) {
        l.continued = (i < self->ynum - 1) ? self->continued_map[i+1] : self->continued_map[i];
        INIT_LINE(self, (&l), self->line_map[i]);
        index_type num = line_as_ansi(&l, t, 5120);
        if (!(l.continued) && num < 5119) t[num++] = 10; // 10 = \n
        PyObject *ans = PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, t, num);
        if (ans == NULL) return PyErr_NoMemory();
        PyObject *ret = PyObject_CallFunctionObjArgs(callback, ans, NULL);
        Py_CLEAR(ans);
        if (ret == NULL) return NULL;
        Py_CLEAR(ret);
    }
    Py_RETURN_NONE;
}

// Boilerplate {{{
static PyObject*
copy_old(LineBuf *self, PyObject *y);
#define copy_old_doc "Copy the contents of the specified LineBuf to this LineBuf. Both must have the same number of columns, but the number of lines can be different, in which case the bottom lines are copied."

static PyObject*
rewrap(LineBuf *self, PyObject *args);
#define rewrap_doc "rewrap(new_screen) -> Fill up new screen (which can have different size to this screen) with as much of the contents of this screen as will fit. Return lines that overflow."

static PyMethodDef methods[] = {
    METHOD(line, METH_O)
    METHOD(clear_line, METH_O)
    METHOD(copy_old, METH_O)
    METHOD(copy_line_to, METH_VARARGS)
    METHOD(create_line_copy, METH_O)
    METHOD(rewrap, METH_VARARGS)
    METHOD(clear, METH_NOARGS)
    METHOD(as_ansi, METH_O)
    METHOD(set_attribute, METH_VARARGS)
    METHOD(set_continued, METH_VARARGS)
    METHOD(index, METH_VARARGS)
    METHOD(reverse_index, METH_VARARGS)
    METHOD(insert_lines, METH_VARARGS)
    METHOD(delete_lines, METH_VARARGS)
    METHOD(is_continued, METH_O)
    {NULL, NULL, 0, NULL}  /* Sentinel */
};

static PyMemberDef members[] = {
    {"xnum", T_UINT, offsetof(LineBuf, xnum), READONLY, "xnum"},
    {"ynum", T_UINT, offsetof(LineBuf, ynum), READONLY, "ynum"},
    {NULL}  /* Sentinel */
};

PyTypeObject LineBuf_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "fast_data_types.LineBuf",
    .tp_basicsize = sizeof(LineBuf),
    .tp_dealloc = (destructor)dealloc, 
    .tp_flags = Py_TPFLAGS_DEFAULT,        
    .tp_doc = "Line buffers",
    .tp_methods = methods,
    .tp_members = members,            
    .tp_new = new
};

INIT_TYPE(LineBuf)
// }}}

static PyObject*
copy_old(LineBuf *self, PyObject *y) {
    if (!PyObject_TypeCheck(y, &LineBuf_Type)) { PyErr_SetString(PyExc_TypeError, "Not a LineBuf object"); return NULL; }
    LineBuf *other = (LineBuf*)y;
    if (other->xnum != self->xnum) { PyErr_SetString(PyExc_ValueError, "LineBuf has a different number of columns"); return NULL; }
    Line sl = {{0}}, ol = {{0}};
    sl.xnum = self->xnum; ol.xnum = other->xnum;

    for (index_type i = 0; i < MIN(self->ynum, other->ynum); i++) {
        index_type s = self->ynum - 1 - i, o = other->ynum - 1 - i;
        self->continued_map[s] = other->continued_map[o];
        s = self->line_map[s]; o = other->line_map[o];
        INIT_LINE(self, &sl, s); INIT_LINE(other, &ol, o);
        COPY_LINE(&ol, &sl);
    }
    Py_RETURN_NONE;
}

#include "rewrap.h"

void 
linebuf_rewrap(LineBuf *self, LineBuf *other, int *cursor_y_out, HistoryBuf *historybuf) {
    index_type first, i;
    bool is_empty = true;

    // Fast path
    if (other->xnum == self->xnum && other->ynum == self->ynum) {
        Py_BEGIN_ALLOW_THREADS;
        memcpy(other->line_map, self->line_map, sizeof(index_type) * self->ynum);
        memcpy(other->continued_map, self->continued_map, sizeof(bool) * self->ynum);
        memcpy(other->buf, self->buf, self->xnum * self->ynum * CELL_SIZE);
        Py_END_ALLOW_THREADS;
        return;
    }

    // Find the first line that contains some content
    Py_BEGIN_ALLOW_THREADS;
    for (first = self->ynum - 1; true; first--) {
        char_type *chars = self->chars + self->xnum * first;
        for(i = 0; i < self->xnum; i++) {
            if ((chars[i] & CHAR_MASK) != 32) { is_empty = false; break; }
        }
        if (!is_empty || !first) break;
    }
    Py_END_ALLOW_THREADS;

    if (first == 0) { *cursor_y_out = 0; return; }  // All lines are empty

    rewrap_inner(self, other, first + 1, historybuf);
    *cursor_y_out = other->line->ynum;
}

static PyObject*
rewrap(LineBuf *self, PyObject *args) {
    LineBuf* other;
    HistoryBuf *historybuf;
    int cursor_y = -1;

    if (!PyArg_ParseTuple(args, "O!O!", &LineBuf_Type, &other, &HistoryBuf_Type, &historybuf)) return NULL;
    linebuf_rewrap(self, other, &cursor_y, historybuf);

    return Py_BuildValue("i", cursor_y);
}

LineBuf *alloc_linebuf(unsigned int lines, unsigned int columns) {
    return (LineBuf*)new(&LineBuf_Type, Py_BuildValue("II", lines, columns), NULL);
}
