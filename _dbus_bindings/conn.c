/* Implementation of the _dbus_bindings Connection type, a Python wrapper
 * for DBusConnection. See also conn-methods-impl.h.
 *
 * Copyright (C) 2006 Collabora Ltd. <http://www.collabora.co.uk/>
 *
 * Licensed under the Academic Free License version 2.1
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "dbus_bindings-internal.h"
#include "conn-internal.h"

/* Connection definition ============================================ */

PyDoc_STRVAR(Connection_tp_doc,
"A D-Bus connection.\n\n"
"Connection(address: str, mainloop=None) -> Connection\n"
);

/* D-Bus Connection user data slot, containing an owned reference to either
 * the Connection, or a weakref to the Connection.
 */
static dbus_int32_t _connection_python_slot;

/* C API for main-loop hooks ======================================== */

/* Return a borrowed reference to the DBusConnection which underlies this
 * Connection. */
DBusConnection *
DBusPyConnection_BorrowDBusConnection(PyObject *self)
{
    DBusConnection *dbc;

    if (!DBusPyConnection_Check(self)) {
        PyErr_SetString(PyExc_TypeError, "A dbus.Connection is required");
        return NULL;
    }
    dbc = ((Connection *)self)->conn;
    if (!dbc) {
        PyErr_SetString(PyExc_RuntimeError, "Connection is in an invalid "
                        "state: no DBusConnection");
        return NULL;
    }
    return dbc;
}

/* Internal C API =================================================== */

/* Pass a message through a handler. */
DBusHandlerResult
DBusPyConnection_HandleMessage(Connection *conn,
                               PyObject *msg,
                               PyObject *callable)
{
    PyObject *obj = PyObject_CallFunctionObjArgs(callable, conn, msg,
                                                 NULL);
    if (obj == Py_None) {
        DBG("%p: OK, handler %p returned None", conn, callable);
        Py_DECREF(obj);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    else if (obj == Py_NotImplemented) {
        DBG("%p: handler %p returned NotImplemented, continuing",
            conn, callable);
        Py_DECREF(obj);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    else if (!obj) {
        if (PyErr_ExceptionMatches(PyExc_MemoryError)) {
            DBG_EXC("%p: handler %p caused OOM", conn, callable);
            PyErr_Clear();
            return DBUS_HANDLER_RESULT_NEED_MEMORY;
        }
        DBG_EXC("%p: handler %p raised exception", conn, callable);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    else {
        long i = PyInt_AsLong(obj);
        DBG("%p: handler %p returned %ld", conn, callable, i);
        Py_DECREF(obj);
        if (i == -1 && PyErr_Occurred()) {
            PyErr_SetString(PyExc_TypeError, "Return from D-Bus message "
                            "handler callback should be None, "
                            "NotImplemented or integer");
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        else if (i == DBUS_HANDLER_RESULT_HANDLED ||
            i == DBUS_HANDLER_RESULT_NOT_YET_HANDLED ||
            i == DBUS_HANDLER_RESULT_NEED_MEMORY) {
            return i;
        }
        else {
            PyErr_Format(PyExc_ValueError, "Integer return from "
                        "D-Bus message handler callback should "
                        "be a DBUS_HANDLER_RESULT_... constant, "
                        "not %d", (int)i);
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
    }
}

/* On KeyError or if unregistration is in progress, return None. */
PyObject *
DBusPyConnection_GetObjectPathHandlers(PyObject *self, PyObject *path)
{
    PyObject *callbacks = PyDict_GetItem(((Connection *)self)->object_paths,
                                         path);
    if (!callbacks) {
        if (PyErr_ExceptionMatches(PyExc_KeyError)) {
            PyErr_Clear();
            Py_RETURN_NONE;
        }
    }
    Py_INCREF(callbacks);
    return callbacks;
}

/* Return a new reference to a Python Connection or subclass corresponding
 * to the DBusConnection conn. For use in callbacks.
 *
 * Raises AssertionError if the DBusConnection does not have a Connection.
 */
PyObject *
DBusPyConnection_ExistingFromDBusConnection(DBusConnection *conn)
{
    PyObject *self, *ref;

    Py_BEGIN_ALLOW_THREADS
    ref = (PyObject *)dbus_connection_get_data(conn,
                                               _connection_python_slot);
    Py_END_ALLOW_THREADS
    if (ref) {
        self = PyWeakref_GetObject(ref);   /* still a borrowed ref */
        if (self && self != Py_None && DBusPyConnection_Check(self)) {
            Py_INCREF(self);
            return self;
        }
    }

    PyErr_SetString(PyExc_AssertionError,
                    "D-Bus connection does not have a Connection "
                    "instance associated with it");
    return NULL;
}

/* Return a new reference to a Python Connection or subclass (given by cls)
 * corresponding to the DBusConnection conn, which must have been newly
 * created. For use by the Connection and Bus constructors.
 *
 * Raises AssertionError if the DBusConnection already has a Connection.
 */
PyObject *
DBusPyConnection_NewConsumingDBusConnection(PyTypeObject *cls,
                                            DBusConnection *conn,
                                            PyObject *mainloop)
{
    Connection *self = NULL;
    PyObject *ref;
    dbus_bool_t ok;

    DBG("%s(cls=%p, conn=%p, mainloop=%p)", __FUNC__, cls, conn, mainloop);
    DBUS_PY_RAISE_VIA_NULL_IF_FAIL(conn);

    Py_BEGIN_ALLOW_THREADS
    ref = (PyObject *)dbus_connection_get_data(conn,
                                               _connection_python_slot);
    Py_END_ALLOW_THREADS
    if (ref) {
        self = (Connection *)PyWeakref_GetObject(ref);
        ref = NULL;
        if (self && (PyObject *)self != Py_None) {
            self = NULL;
            PyErr_SetString(PyExc_AssertionError,
                            "Newly created D-Bus connection already has a "
                            "Connection instance associated with it");
            DBG("%s() fail - assertion failed, DBusPyConn has a DBusConn already", __FUNC__);
            DBG_WHEREAMI;
            return NULL;
        }
    }
    ref = NULL;

    if (!mainloop || mainloop == Py_None) {
        mainloop = dbus_py_get_default_main_loop();
        if (!mainloop || mainloop == Py_None) {
            PyErr_SetString(PyExc_ValueError,
                            "D-Bus connections must be attached to a main "
                            "loop by passing mainloop=... to the constructor "
                            "or calling dbus.Bus.set_default_main_loop(...)");
            goto err;
        }
    }
    else {
        Py_INCREF(mainloop);
    }

    DBG("Constructing Connection from DBusConnection at %p", conn);

    self = (Connection *)(cls->tp_alloc(cls, 0));
    if (!self) goto err;

    DBG_WHEREAMI;

    self->conn = NULL;
    self->filters = PyList_New(0);
    if (!self->filters) goto err;
    self->object_paths = PyDict_New();
    if (!self->object_paths) goto err;

    ref = PyWeakref_NewRef((PyObject *)self, NULL);
    if (!ref) goto err;

    Py_BEGIN_ALLOW_THREADS
    ok = dbus_connection_set_data(conn, _connection_python_slot,
                                  (void *)ref,
                                  (DBusFreeFunction)dbus_py_take_gil_and_xdecref);
    Py_END_ALLOW_THREADS

    if (!ok) {
        PyErr_NoMemory();
        goto err;
    }

    DBUS_PY_RAISE_VIA_GOTO_IF_FAIL(conn, err);
    self->conn = conn;

    if (!dbus_py_set_up_connection((PyObject *)self, mainloop)) {
        goto err;
    }

    Py_DECREF(mainloop);

    DBG("%s() -> %p", __FUNC__, self);
    return (PyObject *)self;

err:
    DBG("Failed to construct Connection from DBusConnection at %p", conn);
    Py_XDECREF(mainloop);
    Py_XDECREF(self);
    Py_XDECREF(ref);
    if (conn) {
        Py_BEGIN_ALLOW_THREADS
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        Py_END_ALLOW_THREADS
    }
    DBG("%s() fail", __FUNC__, self);
    DBG_WHEREAMI;
    return NULL;
}

/* Connection type-methods ========================================== */

/* "Constructor" (the real constructor is Connection_NewFromDBusConnection,
 * to which this delegates). */
static PyObject *
Connection_tp_new(PyTypeObject *cls, PyObject *args, PyObject *kwargs)
{
    DBusConnection *conn;
    const char *address;
    DBusError error;
    PyObject *self, *mainloop = NULL;
    static char *argnames[] = {"address", "mainloop", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|O", argnames,
                                     &address, &mainloop)) {
        return NULL;
    }

    dbus_error_init(&error);

    /* We always open a private connection (at the libdbus level). Sharing
     * is done in Python, to keep things simple. */
    Py_BEGIN_ALLOW_THREADS
    conn = dbus_connection_open_private(address, &error);
    Py_END_ALLOW_THREADS

    if (!conn) {
        DBusPyException_ConsumeError(&error);
        return NULL;
    }
    self = DBusPyConnection_NewConsumingDBusConnection(cls, conn, mainloop);

    return self;
}

/* Destructor */
static void Connection_tp_dealloc(Connection *self)
{
    DBusConnection *conn = self->conn;
    PyObject *filters = self->filters;
    PyObject *object_paths = self->object_paths;

    DBG("Deallocating Connection at %p (DBusConnection at %p)", self, conn);
    DBG_WHEREAMI;

    if (conn) {
        /* Might trigger callbacks if we're unlucky... */
        DBG("Connection at %p has a conn, closing it...", self);
        Py_BEGIN_ALLOW_THREADS
        dbus_connection_close(conn);
        Py_END_ALLOW_THREADS
    }

    DBG("Connection at %p: deleting callbacks", self);
    self->filters = NULL;
    Py_XDECREF(filters);
    self->object_paths = NULL;
    Py_XDECREF(object_paths);

    /* make sure to do this last to preserve the invariant that 
     * self->conn is always non-NULL for any referenced Connection
     * (until the filters and object paths were freed, we might have been
     * in a reference cycle!)
     */
    DBG("Connection at %p: nulling self->conn", self);
    self->conn = NULL;

    DBG("Connection at %p: unreffing conn", self);
    dbus_connection_unref(conn);

    DBG("Connection at %p: freeing self", self);
    (self->ob_type->tp_free)((PyObject *)self);
}

/* Connection type object =========================================== */

PyTypeObject DBusPyConnection_Type = {
    PyObject_HEAD_INIT(NULL)
    0,                      /*ob_size*/
    "_dbus_bindings.Connection", /*tp_name*/
    sizeof(Connection),     /*tp_basicsize*/
    0,                      /*tp_itemsize*/
    /* methods */
    (destructor)Connection_tp_dealloc,
    0,                      /*tp_print*/
    0,                      /*tp_getattr*/
    0,                      /*tp_setattr*/
    0,                      /*tp_compare*/
    0,                      /*tp_repr*/
    0,                      /*tp_as_number*/
    0,                      /*tp_as_sequence*/
    0,                      /*tp_as_mapping*/
    0,                      /*tp_hash*/
    0,                      /*tp_call*/
    0,                      /*tp_str*/
    0,                      /*tp_getattro*/
    0,                      /*tp_setattro*/
    0,                      /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_WEAKREFS | Py_TPFLAGS_BASETYPE,
    Connection_tp_doc,      /*tp_doc*/
    0,                      /*tp_traverse*/
    0,                      /*tp_clear*/
    0,                      /*tp_richcompare*/
    offsetof(Connection, weaklist),   /*tp_weaklistoffset*/
    0,                      /*tp_iter*/
    0,                      /*tp_iternext*/
    DBusPyConnection_tp_methods,  /*tp_methods*/
    0,                      /*tp_members*/
    0,                      /*tp_getset*/
    0,                      /*tp_base*/
    0,                      /*tp_dict*/
    0,                      /*tp_descr_get*/
    0,                      /*tp_descr_set*/
    0,                      /*tp_dictoffset*/
    0,                      /*tp_init*/
    0,                      /*tp_alloc*/
    Connection_tp_new,      /*tp_new*/
    0,                      /*tp_free*/
    0,                      /*tp_is_gc*/
};

dbus_bool_t
dbus_py_init_conn_types(void)
{
    /* Get a slot to store our weakref on DBus Connections */
    _connection_python_slot = -1;
    if (!dbus_connection_allocate_data_slot(&_connection_python_slot))
        return FALSE;
    if (PyType_Ready(&DBusPyConnection_Type) < 0)
        return FALSE;
    return TRUE;
}

dbus_bool_t
dbus_py_insert_conn_types(PyObject *this_module)
{
    if (PyModule_AddObject(this_module, "Connection",
                           (PyObject *)&DBusPyConnection_Type) < 0) return FALSE;
    return TRUE;
}

/* vim:set ft=c cino< sw=4 sts=4 et: */
