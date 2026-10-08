// pdftoolkit_native — Phase-0 Python extension module (raw C-API, ADR-0003).
//
// Import name: `pdftoolkit_native` (distinct from the Python engine package
// `pdftoolkit`). The extension talks to the engine ONLY through the C-ABI
// (pdftoolkit.h), honouring the layer model: Python -> C-ABI -> C++ core.
//
// Phase 0 surface (intentionally minimal, honest):
//   native_version()      -> str   (the native core version)
//   error_message(code)   -> str   (pdtk status code -> message)
//   engine_smoke()        -> bool  (create+destroy through the C-ABI)
//
// GIL: not released yet — no Phase-0 call blocks. Every real operation
// added in Phase 7 must wrap its body in Py_BEGIN_ALLOW_THREADS /
// Py_END_ALLOW_THREADS (audit task 7.2).

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <cstdint>

#include "pdftoolkit/errors.hpp"
#include "pdftoolkit/pdftoolkit.h"

namespace {

PyObject* py_native_version(PyObject* /*self*/, PyObject* /*noargs*/) {
    char buffer[64];
    const int32_t status =
        pdftoolkit_version(buffer, static_cast<uint32_t>(sizeof(buffer)));
    if (status != PDTK_OK) {
        PyErr_SetString(PyExc_RuntimeError, "pdftoolkit_version() failed");
        return nullptr;
    }
    return PyUnicode_FromString(buffer);
}

PyObject* py_error_message(PyObject* /*self*/, PyObject* args) {
    long code = 0;
    if (!PyArg_ParseTuple(args, "l", &code)) {
        return nullptr;
    }
    if (code < 0 || code >= static_cast<long>(PDTK__STATUS_COUNT)) {
        PyErr_SetString(PyExc_ValueError, "status code out of range");
        return nullptr;
    }
    const auto error_code = static_cast<pdftoolkit::ErrorCode>(code);
    return PyUnicode_FromString(pdftoolkit::error_message(error_code));
}

PyObject* py_engine_smoke(PyObject* /*self*/, PyObject* /*noargs*/) {
    EngineHandle* handle = nullptr;
    if (pdftoolkit_engine_create(&handle) != PDTK_OK || handle == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, "pdftoolkit_engine_create() failed");
        return nullptr;
    }
    if (pdftoolkit_engine_destroy(handle) != PDTK_OK) {
        PyErr_SetString(PyExc_RuntimeError, "pdftoolkit_engine_destroy() failed");
        return nullptr;
    }
    Py_RETURN_TRUE;
}

PyMethodDef k_methods[] = {
    {"native_version", py_native_version, METH_NOARGS,
     "Return the native core version string."},
    {"error_message", py_error_message, METH_VARARGS,
     "Map a pdtk status code (int) to its human-readable message."},
    {"engine_smoke", py_engine_smoke, METH_NOARGS,
     "Create and destroy an engine through the C-ABI; returns True."},
    {nullptr, nullptr, 0, nullptr},
};

PyModuleDef k_module = {
    PyModuleDef_HEAD_INIT,
    /*m_name=*/"pdftoolkit_native",
    /*m_doc=*/"Zero-copy Python bindings for the pdftoolkit native core "
              "(Phase 0 scaffolding; see docs/recovery/task-registry.md).",
    /*m_size=*/-1,
    k_methods,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

}  // namespace

PyMODINIT_FUNC PyInit_pdftoolkit_native(void) {
    return PyModule_Create(&k_module);
}
