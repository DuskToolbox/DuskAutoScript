// DirectorLifecycle.i — Python bridge lifecycle support
// Provides DasSwigPreventManagedObject and DasSwigReleaseManagedObject
// for Python director objects.
//
// These functions are called from the bridge core (das_swig_generator.py)
// when a director object crosses the native boundary.
//
// Dependencies: Python.h (provided by SWIG Python target)

#ifdef SWIGPYTHON
%{

static int DasSwigPreventManagedObject(DasSwigRuntimeContext* p_context)
{
    if (!p_context || p_context->kind != DasSwigRuntimeKind::Python)
    {
        return -1;
    }
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* self = static_cast<PyObject*>(p_context->py_self);
    if (!self)
    {
        PyGILState_Release(gil);
        return -1;
    }

    Py_INCREF(self);

    PyObject* result = PyObject_CallMethod(self, "_das_bridge_prevent", nullptr);
    if (!result)
    {
        PyErr_Clear();
        result = PyObject_CallMethod(self, "__das_bridge_prevent", nullptr);
    }
    if (!result)
    {
        PyErr_Clear();
    }
    else
    {
        Py_DECREF(result);
    }
    PyGILState_Release(gil);
    return 0;
}

static int DasSwigReleaseManagedObject(DasSwigRuntimeContext* p_context)
{
    if (!p_context || p_context->kind != DasSwigRuntimeKind::Python)
    {
        return -1;
    }
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* self = static_cast<PyObject*>(p_context->py_self);
    if (!self)
    {
        PyGILState_Release(gil);
        return -1;
    }

    PyObject* result = PyObject_CallMethod(self, "_das_bridge_release", nullptr);
    if (!result)
    {
        PyErr_Clear();
        result = PyObject_CallMethod(self, "__das_bridge_release", nullptr);
    }
    if (!result)
    {
        PyErr_Clear();
    }
    else
    {
        Py_DECREF(result);
    }
    Py_DECREF(self);
    PyGILState_Release(gil);
    return 0;
}

%}
#endif // SWIGPYTHON
