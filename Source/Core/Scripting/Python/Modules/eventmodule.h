// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <optional>
#include <Python.h>

#include "Scripting/Python/Utils/object_wrapper.h"

namespace PyScripting
{

// std::mutex& getMuler();

// Handle a not-yet-started coroutine that was returned by normal
// script execution (top-level await) or an async callback.
// Those need to get started by initially calling "send" with None
// and then hand them over to HandleCoroutine.
void HandleNewCoroutine(PyObject* module, PyObject* coro);

PyMODINIT_FUNC PyInit_event();
// PyModuleDef getEventModule();
// PyMethodDef* getEventMethods();
PyModuleDef* getEventModule();

using CoroutineScheduler = void(*)(PyObject*, PyObject*);
std::optional<CoroutineScheduler> GetCoroutineScheduler(std::string aeventname);

}
