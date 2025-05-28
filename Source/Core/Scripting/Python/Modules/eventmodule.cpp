// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Scripting/Python/Modules/eventmodule.h"

#include <deque>
#include <functional>
#include <map>

#include "Common/Logging/Log.h"
#include "Core/API/Events.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/Movie.h"
#include "Core/System.h"

#include "Scripting/Python/Utils/convert.h"
#include "Scripting/Python/Utils/invoke.h"
#include "Scripting/Python/Utils/module.h"
#include "Scripting/Python/Utils/object_wrapper.h"
#include "Scripting/Python/PyScriptingBackend.h"

#include <iostream>
#include <mutex>



namespace PyScripting
{

// If you are looking for where the actual events are defined,
// scroll to the bottom of this file.

// For an already-started coroutine and its event tuple describing what
// is being awaited, decode that tuple and make sure the coroutine gets
// resumed once the event being awaited is emitted.
void HandleCoroutine(PyObject* module, PyObject* coro, const Py::Object asyncEventTuple)
{
  const char* magic_string;
  const char* event_name;
  PyObject* args_tuple;
  if (!PyArg_ParseTuple(asyncEventTuple.Lend(), "ssO", &magic_string, &event_name, &args_tuple))
  {
    ERROR_LOG_FMT(SCRIPTING, "A coroutine was yielded to the emulator that it cannot process. "
                             "Did you await something that isn't a dolphin event? "
                             "(error: await-tuple was not (str, str, args))");
    return;
  }
  if (std::string(magic_string) != "dolphin_async_event_magic_string")
  {
    ERROR_LOG_FMT(SCRIPTING, "A coroutine was yielded to the emulator that it cannot process. "
                             "Did you await something that isn't a dolphin event? "
                             "(error: wrong magic string to identify as dolphin-native event)");
    return;
  }
  // `args_tuple` is unused:
  // right now there are no events that take in arguments.
  // If there were, say `await frameadvance(5)` to wait 5 frames,
  // those arguments would be passed as a tuple via `args_tuple`.

  auto scheduler_opt = GetCoroutineScheduler(event_name);
  if (!scheduler_opt.has_value())
  {
    ERROR_LOG_FMT(SCRIPTING, "An unknown event was tried to be awaited: {}", event_name);
    return;
  }
  std::function<void(PyObject*, PyObject*)> scheduler = scheduler_opt.value();
  scheduler(module, coro);
}

void HandleNewCoroutine(PyObject* module, PyObject* coro)
{
  PyObject* asyncEventTuple = Py::CallMethod(coro, "send", Py::Take(Py_None).Leak());
  if (asyncEventTuple != nullptr)
  {
    HandleCoroutine(module, coro, Py::Wrap(asyncEventTuple));
  }
  else if (!PyErr_ExceptionMatches(PyExc_StopIteration))
  {
    // coroutines signal completion by raising StopIteration
    PyErr_Print();
  }
}

template <typename T>
struct EventState
{
  std::set<API::ListenerID<T>> m_active_listener_ids;
};
template <typename... TsEvents>
struct GenericEventModuleState
{
  // API::EventHub* event_hub;
  std::tuple<EventState<TsEvents>...> event_state;

  void Reset()
  {
    std::apply(
        [&](auto&&... s) {
          (([&]() {
             for (auto listener_id : s.m_active_listener_ids)
               API::GetEventHub().UnlistenEvent(listener_id);
             s.m_active_listener_ids.clear();
           }()),
           ...);
        },
        event_state);
  }
  template <typename T>
  void NoteActiveListenerID(API::ListenerID<T> listener_id)
  {
    std::get<EventState<T>>(event_state).m_active_listener_ids.insert(listener_id);
  }
  template <typename T>
  void ForgetActiveListenerID(API::ListenerID<T> listener_id)
  {
    std::get<EventState<T>>(event_state).m_active_listener_ids.erase(listener_id);
  }
};
using EventModuleState = GenericEventModuleState<
  API::Events::FrameAdvance, API::Events::MemoryBreakpoint, API::Events::CodeBreakpoint, API::Events::FrameDrawn>;

static EventModuleState eventState;
static std::mutex m_call_lock{};

// These template shenanigans are all required for PyEventFromMappingFunc
// to be able to infer all of the mapping function signature's parts
// from the mapping function only.
// Basically it takes an <auto T>,
// turns that into <decltype(T), T> captured as <MappingFunc<TEvent, TsArgs...>, TFunc>,
// which then turns that into <TEvent, TsArgs..., MappingFunc<TEvent, TsArgs...> TFunc>.
// See https://stackoverflow.com/a/50775214/3688648 for a more elaborate explanation.
template <typename TEvent, typename... TsArgs>
using MappingFunc = const std::tuple<TsArgs...> (*)(const TEvent&);

template <typename T, T>
struct PyEvent;

template <typename TEvent, typename... TsArgs, MappingFunc<TEvent, TsArgs...> TFunc>
struct PyEvent<MappingFunc<TEvent, TsArgs...>, TFunc>
{
  static PyObject* AddCallback(PyObject* module, PyObject* newCallback)
  {
    // std::lock_guard lock{m_call_lock};
    if (newCallback == Py_None)
    {
      PyErr_SetString(PyExc_ValueError, "event callback must not be None");
      return nullptr;
    }
    if (!PyCallable_Check(newCallback))
    {
      PyErr_SetString(PyExc_TypeError, "event callback must be callable");
      return nullptr;
    }
    EventModuleState* state = &eventState;
    PyInterpreterState* interpreter_state = PyThreadState_Get()->interp;
    Py_INCREF(module);       // TODO felk: where DECREF?
    Py_INCREF(newCallback);  // TODO felk: where DECREF?

    auto listener = [=](const TEvent& event) {
      // std::lock_guard lock{m_call_lock};
      // TODO felk: Creating a new thread state for each event is unnecessary overhead.
      // Since all events of the same type happen inside the same thread anyway, it would be safe to create it once and then reuse it
      // (using PyEval_RestoreThread and PyEval_SaveThread). We can't use the thread state from outside the lambda
      // (PyThreadState_Get()), because the listeners (may) get registered from a different thread,
      // and a python thread state is only valid in the OS thread it was created in.
      PyThreadState* thread_state = PyThreadState_New(interpreter_state);
      PyEval_RestoreThread(thread_state);

      const std::tuple<TsArgs...> args = TFunc(event);
      PyObject* result =
          std::apply([&](auto&&... arg) { return Py::CallFunction(newCallback, arg...); }, args);
      if (result == nullptr)
      {
        PyErr_Print();
      }
      else
      {
        if (PyCoro_CheckExact(result))
          HandleNewCoroutine(module, result);
        // TODO felk: else?
      }

      DecrefPyObjectsInArgs(args);

      PyThreadState_Clear(thread_state);
      PyThreadState_DeleteCurrent();
    };
    auto listener_id = API::GetEventHub().ListenEvent<TEvent>(listener);
    eventState.NoteActiveListenerID<TEvent>(listener_id);
    // TODO felk: handle in python somehow, currently impossible to unsubscribe.
    // TODO felk: documentation is currently wrong: it says only one can be registered (wrong) and you may register "None" to unregister (wrong)
    // TODO felk: where state->ForgetActiveListenerID(listener_id)?
    return Py_BuildValue("i", listener_id.value);
  }
  static PyObject* AddSingleUseCallback(PyObject* module, PyObject* newCallback)
  {
    if (newCallback == Py_None)
    {
      PyErr_SetString(PyExc_ValueError, "event callback must not be None");
      return nullptr;
    }
    if (!PyCallable_Check(newCallback))
    {
      PyErr_SetString(PyExc_TypeError, "event callback must be callable");
      return nullptr;
    }
    EventModuleState* state = &eventState;
    PyInterpreterState* interpreter_state = PyThreadState_Get()->interp;
    Py_INCREF(module);       // TODO felk: where DECREF?
    Py_INCREF(newCallback);  // TODO felk: where DECREF?

    auto listener_id = std::make_shared<API::ListenerID<TEvent>>();
    auto listener = [=](const TEvent& event) mutable {
      // TODO felk: Creating a new thread state for each event is unnecessary overhead.
      // Since all events of the same type happen inside the same thread anyway, it would be safe to create it once and then reuse it
      // (using PyEval_RestoreThread and PyEval_SaveThread). We can't use the thread state from outside the lambda
      // (PyThreadState_Get()), because the listeners (may) get registered from a different thread,
      // and a python thread state is only valid in the OS thread it was created in.
      PyThreadState* thread_state = PyThreadState_New(interpreter_state);
      PyEval_RestoreThread(thread_state);

      const std::tuple<TsArgs...> args = TFunc(event);
      PyObject* result =
          std::apply([&](auto&&... arg) { return Py::CallFunction(newCallback, arg...); }, args);
      if (result == nullptr)
      {
        PyErr_Print();
      }
      else
      {
        if (PyCoro_CheckExact(result))
          HandleNewCoroutine(module, result);
        // TODO felk: else?
      }

      DecrefPyObjectsInArgs(args);
      // Py_DECREF(module);

      PyThreadState_Clear(thread_state);
      PyThreadState_DeleteCurrent();

      API::GetEventHub().UnlistenEvent(*listener_id);
      eventState.ForgetActiveListenerID<TEvent>(*listener_id);

      // Core::getMuler().lock();
      // Core::getMuler().unlock();
    };
    // Core::getMuler().lock();
    *listener_id = API::GetEventHub().ListenEvent<TEvent>(listener);
    eventState.NoteActiveListenerID<TEvent>(*listener_id);
    // Core::getMuler().unlock();
    // TODO felk: handle in python somehow, currently impossible to unsubscribe.
    // TODO felk: documentation is currently wrong: it says only one can be registered (wrong) and you may register "None" to unregister (wrong)
    // TODO felk: where state->ForgetActiveListenerID(listener_id)?
    return Py_BuildValue("i", (*listener_id).value);
  }
  
  static void ScheduleCoroutine(PyObject* module, PyObject* coro)
  {
    PyInterpreterState* interpreter_state = PyThreadState_Get()->interp;
    EventModuleState* state = &eventState;

    Py_INCREF(module);
    Py_INCREF(coro);
    auto listener_id = std::make_shared<API::ListenerID<TEvent>>();
    auto listener = [=](const TEvent& event) mutable {
      // TODO felk: Creating a new thread state for each event is unnecessary overhead.
      // Since all events of the same type happen inside the same thread anyway, it would be safe to
      // create it once and then reuse it (using PyEval_RestoreThread and PyEval_SaveThread). We
      // can't use the thread state from outside the lambda (PyThreadState_Get()), because the
      // listeners (may) get registered from a different thread, and a python thread state is only
      // valid in the OS thread it was created in.
      PyThreadState* thread_state = PyThreadState_New(interpreter_state);
      PyEval_RestoreThread(thread_state);

      const std::tuple<TsArgs...> args = TFunc(event);
      PyObject* args_tuple = Py::BuildValueTuple(args);
      PyObject* newAsyncEventTuple = Py::CallMethod(coro, "send", args_tuple);
      if (newAsyncEventTuple != nullptr)
        HandleCoroutine(module, coro, Py::Wrap(newAsyncEventTuple));
      else if (!PyErr_ExceptionMatches(PyExc_StopIteration))
        // coroutines signal completion by raising StopIteration
        PyErr_Print();
      DecrefPyObjectsInArgs(args);
      Py_DECREF(args_tuple);
      Py_DECREF(coro);
      Py_DECREF(module);

      PyThreadState_Clear(thread_state);
      PyThreadState_DeleteCurrent();

      eventState.ForgetActiveListenerID<TEvent>(*listener_id);
      API::GetEventHub().UnlistenEvent(*listener_id);
    };
    *listener_id = API::GetEventHub().ListenEvent<TEvent>(listener);
    eventState.NoteActiveListenerID<TEvent>(*listener_id);
  }
  static void DecrefPyObjectsInArgs(const std::tuple<TsArgs...> args) {
    std::apply(
        [&](auto&&... arg) {
          // ad-hoc immediately executed lambda because this must be an expression
          (([&] {
             if constexpr (std::is_same_v<
                               std::remove_const_t<std::remove_reference_t<decltype(arg)>>,
                               PyObject*>)
             {
               Py_XDECREF(arg);
             }
           }()),
           ...);
        },
        args);
  }
};

template <auto T>
struct PyEventFromMappingFunc : PyEvent<decltype(T), T>
{
};

/*********************************
 *  actual events defined below  *
 *********************************/

// EVENT MAPPING FUNCTIONS
// Turns an API::Events event to a std::tuple.
// The tuple represents the python event signature.
static const std::tuple<> PyFrameAdvance(const API::Events::FrameAdvance& evt)
{
  return std::make_tuple();
}
static const std::tuple<bool, u32, u64> PyMemoryBreakpoint(const API::Events::MemoryBreakpoint& evt)
{
  return std::make_tuple(evt.write, evt.addr, evt.value);
}
static const std::tuple<u32> PyCodeBreakpoint(const API::Events::CodeBreakpoint& evt)
{
  return std::make_tuple(evt.addr);
}
static const std::tuple<u32, u32, PyObject*> PyFrameDrawn(const API::Events::FrameDrawn& evt)
{
  const u32 num_bytes = evt.width * evt.height * 3;
  PyObject* pybytes = PyBytes_FromStringAndSize(nullptr, num_bytes);
  char* bytebuffer = PyBytes_AsString(pybytes);
  for (u32 y = 0; y < evt.height; ++y)
  {
    const u8* srcrow = evt.data + y * evt.stride;
    char* dstrow = bytebuffer + y * evt.width * 3;
    for (u32 x = 0; x < evt.width; ++x)
    {
      dstrow[x * 3] = srcrow[x * 4];
      dstrow[x * 3 + 1] = srcrow[x * 4 + 1];
      dstrow[x * 3 + 2] = srcrow[x * 4 + 2];
    }
  }
  return std::make_tuple(evt.width, evt.height, pybytes);
}
// EVENT DEFINITIONS
// Creates a PyEvent class from the signature.
using PyFrameAdvanceEvent = PyEventFromMappingFunc<PyFrameAdvance>;
using PyMemoryBreakpointEvent = PyEventFromMappingFunc<PyMemoryBreakpoint>;
using PyCodeBreakpointEvent = PyEventFromMappingFunc<PyCodeBreakpoint>;
using PyFrameDrawnEvent = PyEventFromMappingFunc<PyFrameDrawn>;

std::optional<CoroutineScheduler> GetCoroutineScheduler(std::string aeventname)
{
  static std::map<std::string, CoroutineScheduler> lookup = {
      // HOOKING UP PY EVENTS TO AWAITABLE STRING REPRESENTATION
      // All async-awaitable events must be listed twice:
      // Here, and under the same name in the setup python code
      {"frameadvance", PyFrameAdvanceEvent::ScheduleCoroutine},
      {"memorybreakpoint", PyMemoryBreakpointEvent::ScheduleCoroutine},
      {"codebreakpoint", PyCodeBreakpointEvent::ScheduleCoroutine},
      {"framedrawn", PyFrameDrawnEvent::ScheduleCoroutine},
  };
  auto iter = lookup.find(aeventname);
  if (iter == lookup.end())
    return std::nullopt;
  else
    return iter->second;
}

static void SetupEventModule(PyObject* module, EventModuleState* state)
{
  static const char pycode[] = R"(
class _DolphinAsyncEvent:
    def __init__(self, event_name, *args):
        self.event_name = event_name
        self.args = args
    def __await__(self):
        return (yield ("dolphin_async_event_magic_string", self.event_name, self.args))

async def frameadvance():
    return (await _DolphinAsyncEvent("frameadvance"))

async def memorybreakpoint():
    return (await _DolphinAsyncEvent("memorybreakpoint"))

async def codebreakpoint():
    return (await _DolphinAsyncEvent("codebreakpoint"))

async def framedrawn():
    return (await _DolphinAsyncEvent("framedrawn"))
)";
  Py::Object result = Py::LoadPyCodeIntoModule(module, pycode);
  if (result.IsNull())
  {
    ERROR_LOG_FMT(SCRIPTING, "Failed to load embedded python code into event module");
  }
  // API::EventHub* event_hub = PyScripting::PyScriptingBackend::GetCurrent()->GetEventHub();
  // state->event_hub = event_hub;
  PyScripting::PyScriptingBackend::GetCurrent()->AddCleanupFunc([state] { eventState.Reset(); });
}

static PyObject* Reset(PyObject* module)
{
  EventModuleState* state = &eventState;
  eventState.Reset();
  Py_RETURN_NONE;
}

PyMODINIT_FUNC PyInit_event()
{
  static PyMethodDef methods[] = {
      // EVENT CALLBACKS
      // Has "on_"-prefix, let's python code register a callback
      Py::MakeMethodDef<PyFrameAdvanceEvent::AddCallback>("on_frameadvance"),
      Py::MakeMethodDef<PyMemoryBreakpointEvent::AddCallback>("on_memorybreakpoint"),
      Py::MakeMethodDef<PyCodeBreakpointEvent::AddCallback>("on_codebreakpoint"),
      Py::MakeMethodDef<PyFrameDrawnEvent::AddCallback>("on_framedrawn"),
      Py::MakeMethodDef<Reset>("_dolphin_reset"),

      {nullptr, nullptr, 0, nullptr}  // Sentinel
  };
  static PyModuleDef module_def =
      Py::MakeStatefulModuleDef<EventModuleState, SetupEventModule>("event", methods);
  PyObject* def_obj = PyModuleDef_Init(&module_def);
  return def_obj;
}

// PyMethodDef* getEventMethods() {
//   static PyMethodDef methods[] = {
//     // EVENT CALLBACKS
//     // Has "on_"-prefix, let's python code register a callback
//     Py::MakeMethodDef<PyFrameAdvanceEvent::AddCallback>("on_frameadvance"),
//     Py::MakeMethodDef<PyMemoryBreakpointEvent::AddCallback>("on_memorybreakpoint"),
//     Py::MakeMethodDef<PyCodeBreakpointEvent::AddCallback>("on_codebreakpoint"),
//     Py::MakeMethodDef<PyFrameDrawnEvent::AddCallback>("on_framedrawn"),
//     Py::MakeMethodDef<Reset>("_dolphin_reset"),

//     {nullptr, nullptr, 0, nullptr}  // Sentinel
//   };
//   return methods;
// }

PyModuleDef* getEventModule() {
  static PyMethodDef methods[] = {
    // EVENT CALLBACKS
    // Has "on_"-prefix, let's python code register a callback
    Py::MakeMethodDef<PyFrameAdvanceEvent::AddCallback>("on_frameadvance"),
    Py::MakeMethodDef<PyFrameAdvanceEvent::AddSingleUseCallback>("on_single_frameadvance"),
    Py::MakeMethodDef<PyMemoryBreakpointEvent::AddCallback>("on_memorybreakpoint"),
    Py::MakeMethodDef<PyMemoryBreakpointEvent::AddSingleUseCallback>("on_single_memorybreakpoint"),
    Py::MakeMethodDef<PyCodeBreakpointEvent::AddCallback>("on_codebreakpoint"),
    Py::MakeMethodDef<PyCodeBreakpointEvent::AddSingleUseCallback>("on_single_codebreakpoint"),
    Py::MakeMethodDef<PyFrameDrawnEvent::AddCallback>("on_framedrawn"),
    Py::MakeMethodDef<PyFrameDrawnEvent::AddSingleUseCallback>("on_single_framedrawn"),
    Py::MakeMethodDef<Reset>("_dolphin_reset"),

    {nullptr, nullptr, 0, nullptr}  // Sentinel
  };
  static PyModuleDef ControllerModule = {
    PyModuleDef_HEAD_INIT,
    "Event",
    "Event",
    -1,
    methods,
    nullptr,
    nullptr,
    nullptr,
    nullptr
  };

  return &ControllerModule;
}

}  // namespace PyScripting
