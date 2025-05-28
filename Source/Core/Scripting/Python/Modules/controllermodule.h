// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <Python.h>

namespace PyScripting
{
    // PyObject* set_gc_buttons(PyObject* module, PyObject* args);
    // PyObject* get_gc_buttons(PyObject* module, PyObject* args);
PyMODINIT_FUNC PyInit_controller();
PyModuleDef* getControllerModule();

}  // namespace PyScripting
