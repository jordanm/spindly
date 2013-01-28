/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <Python.h>
#include <datetime.h>
#include <sys/poll.h>
#include <pthread.h>
#include <jsapi.h>

static JSClass global_class = {
    .name = "global",
    .flags = JSCLASS_GLOBAL_FLAGS,
    .addProperty = JS_PropertyStub,
    .delProperty = JS_PropertyStub,
    .getProperty = JS_PropertyStub,
    .setProperty = JS_PropertyStub,
    .enumerate = JS_EnumerateStub,
    .resolve = JS_ResolveStub,
    .convert = JS_ConvertStub,
    .finalize = JS_FinalizeStub,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

void raise_python_exception(JSContext *context, const char *message, JSErrorReport *report) {
    if (!report->filename) {
        PyErr_Format(PyExc_ValueError, "%s", message);
    } else {
        PyErr_Format(PyExc_ValueError, "%s:%u:%s", report->filename,
            (unsigned int) report->lineno, message);
    }

    int *error = JS_GetContextPrivate(context);
    *error = 1;
}

static jsval to_javascript_object(JSContext *context, PyObject *value);
static PyObject *to_python_object(JSContext *context, jsval value);

void populate_javascript_object(JSContext *context, JSObject *obj, PyObject *dict) {
    char *propname;
    PyObject *key, *value;
    Py_ssize_t pos = 0;
    while (PyDict_Next(dict, &pos, &key, &value)) {
        PyObject *str = NULL;
        propname = NULL;
        if (PyString_Check(key)) {
            propname = PyString_AsString(key);
        } else if (PyUnicode_Check(key)) {
            PyObject *str = PyUnicode_AsUTF8String(key);
            propname = PyString_AsString(str);
            Py_DECREF(str);
        }
        if (propname != NULL) {
            jsval item = to_javascript_object(context, value);
            JS_SetProperty(context, obj, propname, &item);
        }
    }
}

static jsval to_javascript_object(JSContext *context, PyObject *value) {
    if (PyString_Check(value)) {
        JSString *obj = JS_NewStringCopyN(context, PyString_AsString(value), PyString_Size(value));
        return STRING_TO_JSVAL(obj);
    } else if (PyUnicode_Check(value)) {
        PyObject *encoded = PyUnicode_AsUTF8String(value);
        JSString *obj = JS_NewStringCopyN(context, PyString_AsString(encoded), PyString_Size(encoded));
        Py_DECREF(encoded);
        return STRING_TO_JSVAL(obj);
    } else if (PyFloat_Check(value)) {
        return DOUBLE_TO_JSVAL(PyFloat_AsDouble(value));
    } else if (PyInt_Check(value)) {
        return INT_TO_JSVAL(PyInt_AsLong(value));
    } else if (PyLong_Check(value)) {
        return INT_TO_JSVAL(PyLong_AsLong(value));
    } else if (PyList_Check(value)) {
        JSObject *obj = JS_NewArrayObject(context, 0, NULL);
        int i;
        for (i = 0; i < PyList_Size(value); i++) {
            jsval item = to_javascript_object(context, PyList_GetItem(value, i));
            JS_SetElement(context, obj, i, &item);
        }
        return OBJECT_TO_JSVAL(obj);
    } else if (PyTuple_Check(value)) {
        JSObject *obj = JS_NewArrayObject(context, 0, NULL);
        int i;
        for (i = 0; i < PyTuple_Size(value); i++) {
            jsval item = to_javascript_object(context, PyTuple_GetItem(value, i));
            JS_SetElement(context, obj, i, &item);
        }
        return OBJECT_TO_JSVAL(obj);
    } else if (PyDict_Check(value)) {
        JSObject *obj = JS_NewObject(context, NULL, NULL, NULL);
        populate_javascript_object(context, obj, value);
        return OBJECT_TO_JSVAL(obj);
    } else if (PyDateTime_Check(value)) {
        JSObject *obj = JS_NewDateObject(context,
            PyDateTime_GET_YEAR(value),
            PyDateTime_GET_MONTH(value) - 1,
            PyDateTime_GET_DAY(value),
            PyDateTime_DATE_GET_HOUR(value),
            PyDateTime_DATE_GET_MINUTE(value),
            PyDateTime_DATE_GET_SECOND(value));
        return OBJECT_TO_JSVAL(obj);
    } else {
        return JSVAL_NULL;
    }
}

static PyObject *to_python_datetime(JSContext *context, JSObject *obj) {
    jsval year, month, day, hour, minute, second;
    if (!JS_CallFunctionName(context, obj, "getFullYear", 0, NULL, &year)) {
        return NULL;
    }
    if (!JS_CallFunctionName(context, obj, "getMonth", 0, NULL, &month)) {
        return NULL;
    }
    if (!JS_CallFunctionName(context, obj, "getDate", 0, NULL, &day)) {
        return NULL;
    }
    if (!JS_CallFunctionName(context, obj, "getHours", 0, NULL, &hour)) {
        return NULL;
    }
    if (!JS_CallFunctionName(context, obj, "getMinutes", 0, NULL, &minute)) {
        return NULL;
    }
    if (!JS_CallFunctionName(context, obj, "getSeconds", 0, NULL, &second)) {
        return NULL;
    }
    return PyDateTime_FromDateAndTime(JSVAL_TO_INT(year), JSVAL_TO_INT(month) + 1,
        JSVAL_TO_INT(day), JSVAL_TO_INT(hour), JSVAL_TO_INT(minute),
        JSVAL_TO_INT(second), 0);
}

static PyObject *to_python_list(JSContext *context, JSObject *obj) {
    PyObject *list = PyList_New(0);
    size_t length, i;
    jsval item;
    if (JS_GetArrayLength(context, obj, &length)) {
        for (i = 0; i < length; i++) {
            if (JS_GetElement(context, obj, i, &item)) {
                PyObject *list_item = to_python_object(context, item);
                PyList_Append(list, list_item);
                Py_DECREF(list_item);
            }
        }
    }
    return list;
}

static PyObject *to_python_dict(JSContext *context, JSObject *obj) {
    JSObject *iter = JS_NewPropertyIterator(context, obj);
    if (iter) {
        PyObject *dict = PyDict_New();
        jsid propid;
        while (JS_NextProperty(context, iter, &propid) == JS_TRUE) {
            if (propid == JSID_VOID) {
                break;
            }
            jsval key, value;
            if (JS_IdToValue(context, propid, &key)) {
                PyObject *pykey = to_python_object(context, key);
                if (pykey != Py_None) {
                    if (JS_GetPropertyById(context, obj, propid, &value)) {
                        PyObject *pyvalue = to_python_object(context, value);
                        PyDict_SetItem(dict, pykey, pyvalue);
                        Py_DECREF(pykey);
                        Py_DECREF(pyvalue);
                    }
                }
            }
        }
        return dict;
    } else {
        Py_INCREF(Py_None);
        return Py_None;
    }
}

static PyObject *to_python_object(JSContext *context, jsval value) {
    if (JSVAL_IS_PRIMITIVE(value)) {
        if (JSVAL_IS_STRING(value)) {
            return PyUnicode_FromString(JS_EncodeString(context, JSVAL_TO_STRING(value)));
        } else if (JSVAL_IS_BOOLEAN(value)) {
            return PyBool_FromLong(JSVAL_TO_BOOLEAN(value));
        } else if (JSVAL_IS_INT(value)) {
            return PyLong_FromLong(JSVAL_TO_INT(value));
        } else if (JSVAL_IS_DOUBLE(value)) {
            return PyFloat_FromDouble(JSVAL_TO_DOUBLE(value));
        } else {
            Py_INCREF(Py_None);
            return Py_None;
        }
    } else {
        JSObject *obj = JSVAL_TO_OBJECT(value);
        if (JS_ObjectIsDate(context, obj)) {
            return to_python_datetime(context, obj);
        } else if (JS_IsArrayObject(context, obj)) {
            return to_python_list(context, obj);
        } else {
            return to_python_dict(context, obj);
        }
    }
}

struct watchdog {
    pthread_t tid;
    JSContext *context;
    int timeout;
    int pipe[2];
};

JSBool js_destroy(JSContext *context) {
    JS_SetPendingException(context, STRING_TO_JSVAL(JS_NewStringCopyZ(context, "timeout")));
    return JS_FALSE;
}

void *js_watchdog(void *ptr) {
    struct watchdog *wd = (struct watchdog *) ptr;
    struct pollfd poller;

    poller.fd = wd->pipe[1];
    poller.events = POLLIN;

    int retval = poll(&poller, 1, wd->timeout * 1000);
    if (retval <= 0) {
        JS_TriggerOperationCallback(wd->context);
    }
    return NULL;
}

struct watchdog *run_watchdog(JSContext *context, int timeout) {
    struct watchdog *wd;

    wd = calloc(sizeof(struct watchdog), 1);
    if (!wd) {
        return NULL;
    }

    wd->context = context;
    if (pipe(wd->pipe) < 0) {
        free(wd);
        return NULL;
    }

    wd->timeout = timeout;
    if (pthread_create(&wd->tid, NULL, js_watchdog, wd) != 0) {
        close(wd->pipe[0]);
        close(wd->pipe[1]);
        free(wd);
        return NULL;
    }
    return wd;
}

void shutdown_watchdog(struct watchdog *wd) {
    close(wd->pipe[0]);
    close(wd->pipe[1]);
    (void) pthread_join(wd->tid, NULL);
    free(wd);
}

void shutdown(JSRuntime *runtime, JSContext *context) {
    JS_DestroyContext(context);
    JS_DestroyRuntime(runtime);
    JS_ShutDown();
}

static PyObject *spindly_js(PyObject *self, PyObject *args) {
    JSRuntime *runtime;
    JSContext *context;
    JSObject *global;

    char *script;
    Py_ssize_t script_length;
    PyObject *params = NULL;
    int timeout = 10;

    jsval rvalue;

    int error = 0;
    struct watchdog *wd = NULL;

    if (!PyArg_ParseTuple(args, "s#|Oi:js", &script, &script_length, &params, &timeout)) {
        return NULL;
    }
    if (params != NULL && !PyDict_Check(params)) {
        return PyErr_Format(PyExc_TypeError, "params must be a dict");
    }

    runtime = JS_NewRuntime(1024L * 1024L);
    if (!runtime) {
        return PyErr_Format(PyExc_SystemError, "unable to initialize JS runtime\n");
    }

    context = JS_NewContext(runtime, 8192);
    if (!context) {
        JS_DestroyRuntime(runtime);
        return PyErr_Format(PyExc_SystemError, "unable to initialize JS context\n");
    }

    JS_SetContextPrivate(context, &error);
    JS_SetOptions(context, JSOPTION_VAROBJFIX);
    JS_SetVersion(context, JSVERSION_LATEST);
    JS_SetErrorReporter(context, raise_python_exception);
    JS_SetOperationCallback(context, js_destroy);

    global = JS_NewCompartmentAndGlobalObject(context, &global_class, NULL);
    JS_InitStandardClasses(context, global);

    if (params != NULL) {
        populate_javascript_object(context, global, params);
    }

    if (timeout > 0) {
        wd = run_watchdog(context, timeout);
        if (wd == NULL) {
            shutdown(runtime, context);
            return PyErr_Format(PyExc_SystemError, "unable to initialize JS watchdog\n");
        }
    }

    JSBool retval = JS_EvaluateScript(context, global, script, script_length, "spindly", 1, &rvalue);
    if (wd) {
        shutdown_watchdog(wd);
    }

    if (retval == JS_FALSE || error == 1) {
        shutdown(runtime, context);
        return NULL;
    }

    PyObject *obj = to_python_object(context, rvalue);
    shutdown(runtime, context);
    return obj;
}

static PyMethodDef spindly_methods[] = {
    {"js", spindly_js, METH_VARARGS, "execute javascript code"},
    {NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC initspindly(void) {
    PyDateTime_IMPORT;
    (void) Py_InitModule("spindly", spindly_methods);
}
