#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <structmember.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>


typedef struct {
    PyObject *key;
    PyObject *value;
    int occupied;
    int deleted;
} HashEntry;

typedef struct {
    PyObject_HEAD
    HashEntry *table;
    Py_ssize_t size;
    Py_ssize_t used;
    Py_ssize_t fill;
} LyraObject;

static int dictresize(LyraObject *mp, Py_ssize_t minsize);

static uint64_t fnv1a_hash(const char *str, size_t len) {
    uint64_t hash = 14695981039346656037ULL; // FNV offset basis kek
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)str[i];
        hash *= 1099511628211ULL; // FNV prime lol
    }
    return hash;
}

static Py_ssize_t lookdict(LyraObject *mp, PyObject *key, int *found_existing) {
    Py_hash_t hash;
    Py_ssize_t i, perturb;
    Py_ssize_t mask;
    HashEntry *ep;
    PyObject *startkey;
    Py_ssize_t freeslot = -1;
    int cmp;

    hash = PyObject_Hash(key);
    if (hash == -1)
        return -1;

    mask = mp->size - 1;
    i = hash & mask;
    ep = &mp->table[i];
    *found_existing = 0;

    if (!ep->occupied)
        return i;

    if (ep->deleted) {
        freeslot = i;
    }
    else {
        startkey = ep->key;
        cmp = PyObject_RichCompareBool(startkey, key, Py_EQ);
        if (cmp > 0) {
            *found_existing = 1;
            return i;
        }
        else if (cmp < 0)
            return -1;
    }

    perturb = hash;
    while (1) {
        i = ((i << 2) + i + perturb + 1) & mask;
        ep = &mp->table[i];
        
        if (!ep->occupied) {
            return freeslot >= 0 ? freeslot : i;
        }
        
        if (ep->deleted) {
            if (freeslot < 0)
                freeslot = i;
        }
        else {
            cmp = PyObject_RichCompareBool(ep->key, key, Py_EQ);
            if (cmp > 0) {
                *found_existing = 1;
                return i;
            }
            else if (cmp < 0)
                return -1;
        }

        perturb >>= 5;
    }

    return -1;
}

static int dictresize(LyraObject *mp, Py_ssize_t minsize) {
    Py_ssize_t newsize;
    HashEntry *oldtable, *newtable;
    Py_ssize_t i;
    int found;

    for (newsize = 8; newsize < minsize && newsize > 0; newsize <<= 1);
    if (newsize <= 0) {
        PyErr_NoMemory();
        return -1;
    }

    newtable = (HashEntry *)calloc(newsize, sizeof(HashEntry));
    if (newtable == NULL) {
        PyErr_NoMemory();
        return -1;
    }

    oldtable = mp->table;
    Py_ssize_t oldsize = mp->size;

    mp->table = newtable;
    mp->size = newsize;
    mp->fill = 0;
    mp->used = 0;

    if (oldtable != NULL) {
        for (i = 0; i < oldsize; i++) {
            if (oldtable[i].occupied && !oldtable[i].deleted) {
                PyObject *key = oldtable[i].key;
                PyObject *value = oldtable[i].value;

                int found_existing = 0;
                Py_ssize_t index = lookdict(mp, key, &found_existing);
                if (index == -1) {
                    free(oldtable);
                    return -1;
                }

                newtable[index].key = key;
                newtable[index].value = value;
                newtable[index].occupied = 1;
                newtable[index].deleted = 0;

                mp->used++;
                mp->fill++;

                Py_INCREF(key);
                Py_INCREF(value);
            }
        }

        for (i = 0; i < oldsize; i++) {
            if (oldtable[i].occupied) {
                Py_DECREF(oldtable[i].key);
                Py_DECREF(oldtable[i].value);
            }
        }
        free(oldtable);
    }
    
    return 0;
}

static int Lyra_init(LyraObject *self, PyObject *args, PyObject *kwds) {
    self->table = NULL;
    self->size = 0;
    self->used = 0;
    self->fill = 0;

    return dictresize(self, 8);
}

static void Lyra_dealloc(LyraObject *self) {
    Py_ssize_t i;

    if (self->table) {
        for (i = 0; i < self->size; i++) {
            if (self->table[i].occupied) {
                Py_XDECREF(self->table[i].key);
                Py_XDECREF(self->table[i].value);
            }
        }
        free(self->table);
    }
    
    Py_TYPE(self)->tp_free((PyObject *) self);
}

static PyObject* Lyra_set(LyraObject *self, PyObject *args) {
    PyObject *key;
    PyObject *value;
    int found_existing;
    Py_ssize_t index;

    if (!PyArg_ParseTuple(args, "OO", &key, &value)) {
        return NULL;
    }

    if (PyObject_Hash(key) == -1) {
        PyErr_SetString(PyExc_TypeError, "No way...unhashable type! GTFO!");
        return NULL;
    }

    if (self->fill * 3 >= self->size * 2) {
        if (dictresize(self, self->size * 2) != 0) {
            return NULL;
        }
    }

    index = lookdict(self, key, &found_existing);
    if (index == -1) {
        return NULL;
    }

    if (found_existing) {
        Py_DECREF(self->table[index].value);
        self->table[index].value = value;
        Py_INCREF(value);
    } else {
        if (self->table[index].occupied) {
            Py_XDECREF(self->table[index].key);
            Py_XDECREF(self->table[index].value);
        } else {
            self->fill++;
        }
        
        self->table[index].key = key;
        self->table[index].value = value;
        self->table[index].occupied = 1;
        self->table[index].deleted = 0;
        self->used++;
        
        Py_INCREF(key);
        Py_INCREF(value);
    }
    
    Py_RETURN_NONE;
}

static PyObject* Lyra_get(LyraObject *self, PyObject *args) {
    PyObject *key;
    PyObject *default_value = Py_None;
    int found_existing;
    Py_ssize_t index;

    if (!PyArg_ParseTuple(args, "O|O", &key, &default_value)) {
        return NULL;
    }

    index = lookdict(self, key, &found_existing);
    if (index == -1) {
        return NULL;
    }

    if (found_existing) {
        PyObject *result = self->table[index].value;
        Py_INCREF(result);
        return result;
    }

    Py_INCREF(default_value);
    return default_value;
}

static PyObject* Lyra_delete(LyraObject *self, PyObject *args) {
    PyObject *key;
    int found_existing;
    Py_ssize_t index;

    if (!PyArg_ParseTuple(args, "O", &key)) {
        return NULL;
    }

    index = lookdict(self, key, &found_existing);
    if (index == -1) {
        return NULL;
    }

    if (found_existing) {
        Py_DECREF(self->table[index].key);
        Py_DECREF(self->table[index].value);
        self->table[index].deleted = 1;
        self->used--;
        Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
}

static PyObject* Lyra_contains(LyraObject *self, PyObject *args) {
    PyObject *key;
    int found_existing;

    if (!PyArg_ParseTuple(args, "O", &key)) {
        return NULL;
    }

    if (lookdict(self, key, &found_existing) == -1) {
        return NULL;
    }
    
    if (found_existing) {
        Py_RETURN_TRUE;
    } else {
        Py_RETURN_FALSE;
    }
}

static PyObject* Lyra_len(LyraObject *self, PyObject *Py_UNUSED(ignored)) {
    return PyLong_FromSsize_t(self->used);
}

static PyObject* Lyra_clear(LyraObject *self, PyObject *Py_UNUSED(ignored)) {
    Py_ssize_t i;

    if (self->table) {
        for (i = 0; i < self->size; i++) {
            if (self->table[i].occupied) {
                Py_XDECREF(self->table[i].key);
                Py_XDECREF(self->table[i].value);
                self->table[i].occupied = 0;
                self->table[i].deleted = 0;
            }
        }
    }
    
    self->used = 0;
    self->fill = 0;
    
    Py_RETURN_NONE;
}

static PyObject* Lyra_keys(LyraObject *self, PyObject *Py_UNUSED(ignored)) {
    PyObject *list = PyList_New(self->used);
    if (!list) {
        return NULL;
    }
    
    Py_ssize_t pos = 0;
    for (Py_ssize_t i = 0; i < self->size; i++) {
        if (self->table[i].occupied && !self->table[i].deleted) {
            PyObject *key = self->table[i].key;
            Py_INCREF(key);
            PyList_SET_ITEM(list, pos++, key);
        }
    }
    
    return list;
}

static PyObject* Lyra_values(LyraObject *self, PyObject *Py_UNUSED(ignored)) {
    PyObject *list = PyList_New(self->used);
    if (!list) {
        return NULL;
    }
    
    Py_ssize_t pos = 0;
    for (Py_ssize_t i = 0; i < self->size; i++) {
        if (self->table[i].occupied && !self->table[i].deleted) {
            PyObject *value = self->table[i].value;
            Py_INCREF(value);
            PyList_SET_ITEM(list, pos++, value);
        }
    }
    
    return list;
}

static PyObject* Lyra_items(LyraObject *self, PyObject *Py_UNUSED(ignored)) {
    PyObject *list = PyList_New(self->used);
    if (!list) {
        return NULL;
    }
    
    Py_ssize_t pos = 0;
    for (Py_ssize_t i = 0; i < self->size; i++) {
        if (self->table[i].occupied && !self->table[i].deleted) {
            PyObject *key = self->table[i].key;
            PyObject *value = self->table[i].value;
            PyObject *tuple = PyTuple_New(2);
            
            if (!tuple) {
                Py_DECREF(list);
                return NULL;
            }
            
            Py_INCREF(key);
            Py_INCREF(value);
            PyTuple_SET_ITEM(tuple, 0, key);
            PyTuple_SET_ITEM(tuple, 1, value);
            PyList_SET_ITEM(list, pos++, tuple);
        }
    }
    
    return list;
}

static PyMethodDef Lyra_methods[] = {
    {"set", (PyCFunction) Lyra_set, METH_VARARGS, "set"},
    {"get", (PyCFunction) Lyra_get, METH_VARARGS, "get"},
    {"delete", (PyCFunction) Lyra_delete, METH_VARARGS, "delete"},
    {"contains", (PyCFunction) Lyra_contains, METH_VARARGS, "contains"},
    {"len", (PyCFunction) Lyra_len, METH_NOARGS, "len"},
    {"clear", (PyCFunction) Lyra_clear, METH_NOARGS, "clear"},
    {"keys", (PyCFunction) Lyra_keys, METH_NOARGS, "keys"},
    {"values", (PyCFunction) Lyra_values, METH_NOARGS, "values"},
    {"items", (PyCFunction) Lyra_items, METH_NOARGS, "items"},
    {NULL, NULL, 0, NULL}
};

static PyTypeObject LyraType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "lyra.Lyra",
    .tp_doc = "i don't know",
    .tp_basicsize = sizeof(LyraObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc) Lyra_init,
    .tp_dealloc = (destructor) Lyra_dealloc,
    .tp_methods = Lyra_methods,
};

static PyMethodDef Lyra_module_methods[] = {
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef lyramodule = {
    PyModuleDef_HEAD_INIT,
    "lyra",
    "lyra",
    -1,
    Lyra_module_methods
};

PyMODINIT_FUNC PyInit_lyra(void) {
    PyObject *m;

    if (PyType_Ready(&LyraType) < 0)
        return NULL;

    m = PyModule_Create(&lyramodule);
    if (m == NULL)
        return NULL;

    Py_INCREF(&LyraType);
    if (PyModule_AddObject(m, "Lyra", (PyObject *) &LyraType) < 0) {
        Py_DECREF(&LyraType);
        Py_DECREF(m);
        return NULL;
    }

    return m;
}
