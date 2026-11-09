/*
    pybind11/detail/class.h: Python C API implementation details for py::class_

    Copyright (c) 2017 Wenzel Jakob <wenzel.jakob@epfl.ch>

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE file.
*/

#pragma once

#include <pybind11/attr.h>
#include <pybind11/options.h>

#include "exception_translation.h"

PYBIND11_NAMESPACE_BEGIN(PYBIND11_NAMESPACE)
PYBIND11_NAMESPACE_BEGIN(detail)

#if !defined(PYPY_VERSION)
#    define PYBIND11_BUILTIN_QUALNAME
#    define PYBIND11_SET_OLDPY_QUALNAME(obj, nameobj)
#else
// In PyPy, we still set __qualname__ so that we can produce reliable function type
// signatures; in CPython this macro expands to nothing:
#    define PYBIND11_SET_OLDPY_QUALNAME(obj, nameobj)                                             \
        setattr((PyObject *) obj, "__qualname__", nameobj)
#endif

inline std::string get_fully_qualified_tp_name(PyTypeObject *type) {
#if defined(PYBIND11_LIMITED_API)
    return get_tp_name(type);
#elif !defined(PYPY_VERSION)
    return type->tp_name;
#else
    auto module_name = handle((PyObject *) type).attr("__module__").cast<std::string>();
    if (module_name == PYBIND11_BUILTINS_MODULE)
        return type->tp_name;
    else
        return std::move(module_name) + "." + type->tp_name;
#endif
}

inline PyTypeObject *type_incref(PyTypeObject *type) {
    Py_INCREF(type);
    return type;
}

#if defined(PYBIND11_LIMITED_API)

/// `pybind11_static_property.__get__(self, obj, cls)`: Always pass the class instead of the
/// instance.
extern "C" inline PyObject *pybind11_static_get(PyObject *, PyObject *args) {
    PyObject *self = nullptr, *obj = nullptr, *cls = Py_None;
    if (!PyArg_UnpackTuple(args, "__get__", 2, 3, &self, &obj, &cls)) {
        return nullptr;
    }
    if (cls == Py_None) {
        cls = reinterpret_cast<PyObject *>(Py_TYPE(obj));
    }
    auto descr_get = reinterpret_cast<descrgetfunc>(PyType_GetSlot(
        reinterpret_cast<PyTypeObject *>(limited_api_property_type()), Py_tp_descr_get));
    return descr_get(self, cls, cls);
}

/// `pybind11_static_property.__set__(self, obj, value)`: Just like the above `__get__()`.
extern "C" inline PyObject *pybind11_static_set(PyObject *, PyObject *args) {
    PyObject *self = nullptr, *obj = nullptr, *value = nullptr;
    if (!PyArg_UnpackTuple(args, "__set__", 2, 3, &self, &obj, &value)) {
        return nullptr;
    }
    PyObject *cls = PyType_Check(obj) ? obj : (PyObject *) Py_TYPE(obj);
    auto descr_set = reinterpret_cast<descrsetfunc>(PyType_GetSlot(
        reinterpret_cast<PyTypeObject *>(limited_api_property_type()), Py_tp_descr_set));
    // `value == nullptr` (from `__delete__`) means deletion.
    if (descr_set(self, cls, value) != 0) {
        return nullptr;
    }
    Py_RETURN_NONE;
}

/** A `static_property` is the same as a `property` but the `__get__()` and `__set__()`
    methods are modified to always use the object type instead of a concrete instance.
    With the limited API, it is created like a Python class (this also provides the instance
    `__dict__` that Python 3.12+ requires for property subclasses).
    Return value: New reference. */
inline PyTypeObject *make_static_property_type() {
    static PyMethodDef get_def
        = {"__get__", reinterpret_cast<PyCFunction>(pybind11_static_get), METH_VARARGS, nullptr};
    static PyMethodDef set_def
        = {"__set__", reinterpret_cast<PyCFunction>(pybind11_static_set), METH_VARARGS, nullptr};
    static PyMethodDef delete_def = {
        "__delete__", reinterpret_cast<PyCFunction>(pybind11_static_set), METH_VARARGS, nullptr};
    dict namespace_dict;
    for (PyMethodDef *def : {&get_def, &set_def, &delete_def}) {
        auto func = reinterpret_steal<object>(PyCFunction_New(def, nullptr));
        if (!func) {
            throw error_already_set();
        }
        // Wrap it so that it binds `self` like a Python function does.
        auto method = reinterpret_steal<object>(limited_api_instancemethod_new(func.ptr()));
        if (!method) {
            throw error_already_set();
        }
        namespace_dict[def->ml_name] = method;
    }
    namespace_dict["__module__"] = str(PYBIND11_DUMMY_MODULE_NAME);
    auto type_type = handle(reinterpret_cast<PyObject *>(&PyType_Type));
    object type = type_type(str("pybind11_static_property"),
                            make_tuple(handle(limited_api_property_type())),
                            namespace_dict);
    return reinterpret_cast<PyTypeObject *>(type.release().ptr());
}

#elif !defined(PYPY_VERSION)

/// `pybind11_static_property.__get__()`: Always pass the class instead of the instance.
extern "C" inline PyObject *pybind11_static_get(PyObject *self, PyObject * /*ob*/, PyObject *cls) {
    return PyProperty_Type.tp_descr_get(self, cls, cls);
}

/// `pybind11_static_property.__set__()`: Just like the above `__get__()`.
extern "C" inline int pybind11_static_set(PyObject *self, PyObject *obj, PyObject *value) {
    PyObject *cls = PyType_Check(obj) ? obj : (PyObject *) Py_TYPE(obj);
    return PyProperty_Type.tp_descr_set(self, cls, value);
}

// Forward declaration to use in `make_static_property_type()`
inline void enable_dynamic_attributes(PyHeapTypeObject *heap_type);

/** A `static_property` is the same as a `property` but the `__get__()` and `__set__()`
    methods are modified to always use the object type instead of a concrete instance.
    Return value: New reference. */
inline PyTypeObject *make_static_property_type() {
    constexpr auto *name = "pybind11_static_property";
    auto name_obj = reinterpret_steal<object>(PYBIND11_FROM_STRING(name));

    /* Danger zone: from now (and until PyType_Ready), make sure to
       issue no Python C API calls which could potentially invoke the
       garbage collector (the GC will call type_traverse(), which will in
       turn find the newly constructed type in an invalid state) */
    auto *heap_type = reinterpret_cast<PyHeapTypeObject *>(PyType_Type.tp_alloc(&PyType_Type, 0));
    if (!heap_type) {
        pybind11_fail("make_static_property_type(): error allocating type!");
    }

    heap_type->ht_name = name_obj.inc_ref().ptr();
#    ifdef PYBIND11_BUILTIN_QUALNAME
    heap_type->ht_qualname = name_obj.inc_ref().ptr();
#    endif

    auto *type = &heap_type->ht_type;
    type->tp_name = name;
    type->tp_base = type_incref(&PyProperty_Type);
    type->tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HEAPTYPE;
    type->tp_descr_get = pybind11_static_get;
    type->tp_descr_set = pybind11_static_set;

#    if PY_VERSION_HEX >= 0x030C0000
    // Since Python-3.12 property-derived types are required to
    // have dynamic attributes (to set `__doc__`)
    enable_dynamic_attributes(heap_type);
#    endif

    if (PyType_Ready(type) < 0) {
        pybind11_fail("make_static_property_type(): failure in PyType_Ready()!");
    }

    setattr(reinterpret_cast<PyObject *>(type), "__module__", str(PYBIND11_DUMMY_MODULE_NAME));
    PYBIND11_SET_OLDPY_QUALNAME(type, name_obj);

    return type;
}

#else // PYPY

/** PyPy has some issues with the above C API, so we evaluate Python code instead.
    This function will only be called once so performance isn't really a concern.
    Return value: New reference. */
inline PyTypeObject *make_static_property_type() {
    auto d = dict();
    PyObject *result = PyRun_String(R"(\
class pybind11_static_property(property):
    def __get__(self, obj, cls):
        return property.__get__(self, cls, cls)

    def __set__(self, obj, value):
        cls = obj if isinstance(obj, type) else type(obj)
        property.__set__(self, cls, value)
)",
                                    Py_file_input,
                                    d.ptr(),
                                    d.ptr());
    if (result == nullptr)
        throw error_already_set();
    Py_DECREF(result);
    return (PyTypeObject *) d["pybind11_static_property"].cast<object>().release().ptr();
}

#endif // PYPY

/** Types with static properties need to handle `Type.static_prop = x` in a specific way.
    By default, Python replaces the `static_property` itself, but for wrapped C++ types
    we need to call `static_property.__set__()` in order to propagate the new value to
    the underlying C++ data structure. */
extern "C" inline int pybind11_meta_setattro(PyObject *obj, PyObject *name, PyObject *value) {
    // Use `_PyType_Lookup()` instead of `PyObject_GetAttr()` in order to get the raw
    // descriptor (`property`) instead of calling `tp_descr_get` (`property.__get__()`).
#if defined(PYBIND11_LIMITED_API)
    auto descr_holder
        = reinterpret_steal<object>(limited_api_type_lookup((PyTypeObject *) obj, name));
    PyObject *descr = descr_holder.ptr();
#else
    PyObject *descr = _PyType_Lookup((PyTypeObject *) obj, name);
#endif

    // The following assignment combinations are possible:
    //   1. `Type.static_prop = value`             --> descr_set: `Type.static_prop.__set__(value)`
    //   2. `Type.static_prop = other_static_prop` --> setattro:  replace existing `static_prop`
    //   3. `Type.regular_attribute = value`       --> setattro:  regular attribute assignment
    auto *const static_prop = (PyObject *) get_internals().static_property_type;
    const auto call_descr_set = (descr != nullptr) && (value != nullptr)
                                && (PyObject_IsInstance(descr, static_prop) != 0)
                                && (PyObject_IsInstance(value, static_prop) == 0);
    if (call_descr_set) {
        // Call `static_property.__set__()` instead of replacing the `static_property`.
#if defined(PYBIND11_LIMITED_API)
        return reinterpret_cast<descrsetfunc>(PyType_GetSlot(Py_TYPE(descr), Py_tp_descr_set))(
            descr, obj, value);
#elif !defined(PYPY_VERSION)
        return Py_TYPE(descr)->tp_descr_set(descr, obj, value);
#else
        if (PyObject *result = PyObject_CallMethod(descr, "__set__", "OO", obj, value)) {
            Py_DECREF(result);
            return 0;
        } else {
            return -1;
        }
#endif
    } else {
        // Replace existing attribute.
#if defined(PYBIND11_LIMITED_API)
        return reinterpret_cast<setattrofunc>(PyType_GetSlot(&PyType_Type, Py_tp_setattro))(
            obj, name, value);
#else
        return PyType_Type.tp_setattro(obj, name, value);
#endif
    }
}

/**
 * Python 3's PyInstanceMethod_Type hides itself via its tp_descr_get, which prevents aliasing
 * methods via cls.attr("m2") = cls.attr("m1"): instead the tp_descr_get returns a plain function,
 * when called on a class, or a PyMethod, when called on an instance.  Override that behaviour here
 * to do a special case bypass for PyInstanceMethod_Types.
 */
extern "C" inline PyObject *pybind11_meta_getattro(PyObject *obj, PyObject *name) {
#if defined(PYBIND11_LIMITED_API)
    PyObject *descr = limited_api_type_lookup((PyTypeObject *) obj, name);
    if (descr && limited_api_instancemethod_check(descr)) {
        return descr;
    }
    Py_XDECREF(descr);
    return reinterpret_cast<getattrofunc>(PyType_GetSlot(&PyType_Type, Py_tp_getattro))(obj,
                                                                                          name);
#else
    PyObject *descr = _PyType_Lookup((PyTypeObject *) obj, name);
    if (descr && PyInstanceMethod_Check(descr)) {
        Py_INCREF(descr);
        return descr;
    }
    return PyType_Type.tp_getattro(obj, name);
#endif
}

/// metaclass `__call__` function that is used to create all pybind11 objects.
extern "C" inline PyObject *pybind11_meta_call(PyObject *type, PyObject *args, PyObject *kwargs) {

    // use the default metaclass call to create/initialize the object
#if defined(PYBIND11_LIMITED_API)
    PyObject *self = reinterpret_cast<ternaryfunc>(PyType_GetSlot(&PyType_Type, Py_tp_call))(
        type, args, kwargs);
#else
    PyObject *self = PyType_Type.tp_call(type, args, kwargs);
#endif
    if (self == nullptr) {
        return nullptr;
    }

    // Ensure that the base __init__ function(s) were called
    values_and_holders vhs(self);
    for (const auto &vh : vhs) {
        if (!vh.holder_constructed() && !vhs.is_redundant_value_and_holder(vh)) {
            PyErr_Format(PyExc_TypeError,
                         "%.200s.__init__() must be called when overriding __init__",
                         get_fully_qualified_tp_name(vh.type->type).c_str());
            Py_DECREF(self);
            return nullptr;
        }
    }

    return self;
}

/// Cleanup the type-info for a pybind11-registered type.
extern "C" inline void pybind11_meta_dealloc(PyObject *obj) {
    with_internals_if_internals([obj](internals &internals) {
        auto *type = (PyTypeObject *) obj;

        // A pybind11-registered type will:
        // 1) be found in internals.registered_types_py
        // 2) have exactly one associated `detail::type_info`
        auto found_type = internals.registered_types_py.find(type);
        if (found_type != internals.registered_types_py.end() && found_type->second.size() == 1
            && found_type->second[0]->type == type) {

            auto *tinfo = found_type->second[0];
            auto tindex = std::type_index(*tinfo->cpptype);
            internals.direct_conversions.erase(tindex);

            auto &local_internals = get_local_internals();
            if (tinfo->module_local) {
                local_internals.registered_types_cpp.erase(tinfo->cpptype);
            } else {
                internals.registered_types_cpp.erase(tindex);
#if PYBIND11_INTERNALS_VERSION >= 12
                internals.registered_types_cpp_fast.erase(tinfo->cpptype);
                for (const std::type_info *alias : tinfo->alias_chain) {
                    auto num_erased = internals.registered_types_cpp_fast.erase(alias);
                    (void) num_erased;
                    assert(num_erased > 0);
                }
#endif
            }
            internals.registered_types_py.erase(tinfo->type);

            // Actually just `std::erase_if`, but that's only available in C++20
            auto &cache = internals.inactive_override_cache;
            for (auto it = cache.begin(), last = cache.end(); it != last;) {
                if (it->first == (PyObject *) tinfo->type) {
                    it = cache.erase(it);
                } else {
                    ++it;
                }
            }

            delete tinfo;
        }
    });

#if defined(PYBIND11_LIMITED_API)
    reinterpret_cast<destructor>(PyType_GetSlot(&PyType_Type, Py_tp_dealloc))(obj);
#else
    PyType_Type.tp_dealloc(obj);
#endif
}

/** This metaclass is assigned by default to all pybind11 types and is required in order
    for static properties to function correctly. Users may override this using `py::metaclass`.
    Return value: New reference. */
inline PyTypeObject *make_default_metaclass() {
#if defined(PYBIND11_LIMITED_API)
    static PyType_Slot slots[]
        = {{Py_tp_call, reinterpret_cast<void *>(pybind11_meta_call)},
           {Py_tp_setattro, reinterpret_cast<void *>(pybind11_meta_setattro)},
           {Py_tp_getattro, reinterpret_cast<void *>(pybind11_meta_getattro)},
           {Py_tp_dealloc, reinterpret_cast<void *>(pybind11_meta_dealloc)},
           {0, nullptr}};
    static PyType_Spec spec = {PYBIND11_DUMMY_MODULE_NAME ".pybind11_type",
                               0,
                               0,
                               Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
                               slots};
    auto *type = reinterpret_cast<PyTypeObject *>(
        PyType_FromSpecWithBases(&spec, reinterpret_cast<PyObject *>(&PyType_Type)));
    if (!type) {
        pybind11_fail("make_default_metaclass(): failure in PyType_FromSpecWithBases(): "
                      + error_string());
    }
    return type;
#else
    constexpr auto *name = "pybind11_type";
    auto name_obj = reinterpret_steal<object>(PYBIND11_FROM_STRING(name));

    /* Danger zone: from now (and until PyType_Ready), make sure to
       issue no Python C API calls which could potentially invoke the
       garbage collector (the GC will call type_traverse(), which will in
       turn find the newly constructed type in an invalid state) */
    auto *heap_type = reinterpret_cast<PyHeapTypeObject *>(PyType_Type.tp_alloc(&PyType_Type, 0));
    if (!heap_type) {
        pybind11_fail("make_default_metaclass(): error allocating metaclass!");
    }

    heap_type->ht_name = name_obj.inc_ref().ptr();
#ifdef PYBIND11_BUILTIN_QUALNAME
    heap_type->ht_qualname = name_obj.inc_ref().ptr();
#endif

    auto *type = &heap_type->ht_type;
    type->tp_name = name;
    type->tp_base = type_incref(&PyType_Type);
    type->tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HEAPTYPE;

    type->tp_call = pybind11_meta_call;

    type->tp_setattro = pybind11_meta_setattro;
    type->tp_getattro = pybind11_meta_getattro;

    type->tp_dealloc = pybind11_meta_dealloc;

    if (PyType_Ready(type) < 0) {
        pybind11_fail("make_default_metaclass(): failure in PyType_Ready()!");
    }

    setattr(reinterpret_cast<PyObject *>(type), "__module__", str(PYBIND11_DUMMY_MODULE_NAME));
    PYBIND11_SET_OLDPY_QUALNAME(type, name_obj);

    return type;
#endif
}

/// For multiple inheritance types we need to recursively register/deregister base pointers for any
/// base classes with pointers that are difference from the instance value pointer so that we can
/// correctly recognize an offset base class pointer. This calls a function with any offset base
/// ptrs.
inline void traverse_offset_bases(void *valueptr,
                                  const detail::type_info *tinfo,
                                  instance *self,
                                  bool (*f)(void * /*parentptr*/, instance * /*self*/)) {
    for (handle h : reinterpret_steal<tuple>(get_type_bases(tinfo->type))) {
        if (auto *parent_tinfo = get_type_info(reinterpret_cast<PyTypeObject *>(h.ptr()))) {
            for (auto &c : parent_tinfo->implicit_casts) {
                if (c.first == tinfo->cpptype) {
                    auto *parentptr = c.second(valueptr);
                    if (parentptr != valueptr) {
                        f(parentptr, self);
                    }
                    traverse_offset_bases(parentptr, parent_tinfo, self, f);
                    break;
                }
            }
        }
    }
}

#ifdef Py_GIL_DISABLED
inline void enable_try_inc_ref(PyObject *obj) {
#    if PY_VERSION_HEX >= 0x030E00A4
    PyUnstable_EnableTryIncRef(obj);
#    else
    if (_Py_IsImmortal(obj)) {
        return;
    }
    for (;;) {
        Py_ssize_t shared = _Py_atomic_load_ssize_relaxed(&obj->ob_ref_shared);
        if ((shared & _Py_REF_SHARED_FLAG_MASK) != 0) {
            // Nothing to do if it's in WEAKREFS, QUEUED, or MERGED states.
            return;
        }
        if (_Py_atomic_compare_exchange_ssize(
                &obj->ob_ref_shared, &shared, shared | _Py_REF_MAYBE_WEAKREF)) {
            return;
        }
    }
#    endif
}
#endif

inline bool register_instance_impl(void *ptr, instance *self) {
    assert(ptr);
#ifdef Py_GIL_DISABLED
    enable_try_inc_ref(reinterpret_cast<PyObject *>(self));
#endif
    with_instance_map(ptr, [&](instance_map &instances) { instances.emplace(ptr, self); });
    return true; // unused, but gives the same signature as the deregister func
}
inline bool deregister_instance_impl(void *ptr, instance *self) {
    assert(ptr);
    return with_instance_map(ptr, [&](instance_map &instances) {
        auto range = instances.equal_range(ptr);
        for (auto it = range.first; it != range.second; ++it) {
            if (self == it->second) {
                instances.erase(it);
                return true;
            }
        }
        return false;
    });
}

inline void register_instance(instance *self, void *valptr, const type_info *tinfo) {
    register_instance_impl(valptr, self);
    if (!tinfo->simple_ancestors) {
        traverse_offset_bases(valptr, tinfo, self, register_instance_impl);
    }
}

inline bool deregister_instance(instance *self, void *valptr, const type_info *tinfo) {
    bool ret = deregister_instance_impl(valptr, self);
    if (!tinfo->simple_ancestors) {
        traverse_offset_bases(valptr, tinfo, self, deregister_instance_impl);
    }
    return ret;
}

/// Instance creation function for all pybind11 types. It allocates the internal instance layout
/// for holding C++ objects and holders.  Allocation is done lazily (the first time the instance is
/// cast to a reference or pointer), and initialization is done by an `__init__` function.
inline PyObject *make_new_instance(PyTypeObject *type) {
#if defined(PYPY_VERSION)
    // PyPy gets tp_basicsize wrong (issue 2482) under multiple inheritance when the first
    // inherited object is a plain Python type (i.e. not derived from an extension type).  Fix it.
    ssize_t instance_size = static_cast<ssize_t>(sizeof(instance));
    if (type->tp_basicsize < instance_size) {
        type->tp_basicsize = instance_size;
    }
#endif
#if defined(PYBIND11_LIMITED_API)
    PyObject *self = reinterpret_cast<allocfunc>(PyType_GetSlot(type, Py_tp_alloc))(type, 0);
#else
    PyObject *self = type->tp_alloc(type, 0);
#endif
    auto *inst = reinterpret_cast<instance *>(self);
    // Allocate the value/holder internals:
    inst->allocate_layout();

    return self;
}

/// Instance creation function for all pybind11 types. It only allocates space for the
/// C++ object, but doesn't call the constructor -- an `__init__` function must do that.
extern "C" inline PyObject *pybind11_object_new(PyTypeObject *type, PyObject *, PyObject *) {
    return make_new_instance(type);
}

/// An `__init__` function constructs the C++ object. Users should provide at least one
/// of these using `py::init` or directly with `.def(__init__, ...)`. Otherwise, the
/// following default function will be used which simply throws an exception.
extern "C" inline int pybind11_object_init(PyObject *self, PyObject *, PyObject *) {
    PyTypeObject *type = Py_TYPE(self);
    std::string msg = get_fully_qualified_tp_name(type) + ": No constructor defined!";
    set_error(PyExc_TypeError, msg.c_str());
    return -1;
}

inline void add_patient(PyObject *nurse, PyObject *patient) {
    auto *instance = reinterpret_cast<detail::instance *>(nurse);
    instance->has_patients = true;
    Py_INCREF(patient);

    with_internals([&](internals &internals) { internals.patients[nurse].push_back(patient); });
}

inline void clear_patients(PyObject *self) {
    auto *instance = reinterpret_cast<detail::instance *>(self);
    std::vector<PyObject *> patients;

    with_internals([&](internals &internals) {
        auto pos = internals.patients.find(self);

        if (pos == internals.patients.end()) {
            pybind11_fail(
                "FATAL: Internal consistency check failed: Invalid clear_patients() call.");
        }

        // Clearing the patients can cause more Python code to run, which
        // can invalidate the iterator. Extract the vector of patients
        // from the unordered_map first.
        patients = std::move(pos->second);
        internals.patients.erase(pos);
    });

    instance->has_patients = false;
    for (PyObject *&patient : patients) {
        Py_CLEAR(patient);
    }
}

#if defined(PYBIND11_LIMITED_API)
/// py::dynamic_attr(): the `__dict__` slot directly follows the `instance` struct.
inline PyObject **limited_api_instance_dict_ptr(PyObject *self) {
    return reinterpret_cast<PyObject **>(reinterpret_cast<char *>(self) + sizeof(instance));
}

/// Whether instances of `type` have the py::dynamic_attr() `__dict__` slot.
inline bool limited_api_has_instance_dict(PyTypeObject *type) {
    // Do not clobber an exception that may be in flight (e.g. during unwinding).
    PyObject *exc_type = nullptr, *exc_value = nullptr, *exc_trace = nullptr;
    PyErr_Fetch(&exc_type, &exc_value, &exc_trace);
    const bool result
        = get_type_dictoffset(type) == static_cast<Py_ssize_t>(sizeof(instance));
    PyErr_Restore(exc_type, exc_value, exc_trace);
    return result;
}
#endif

/// Clears all internal data from the instance and removes it from registered instances in
/// preparation for deallocation.
inline void clear_instance(PyObject *self) {
    auto *instance = reinterpret_cast<detail::instance *>(self);

    // Deallocate any values/holders, if present:
    for (auto &v_h : values_and_holders(instance)) {
        if (v_h) {

            // We have to deregister before we call dealloc because, for virtual MI types, we still
            // need to be able to get the parent pointers.
            if (v_h.instance_registered()
                && !deregister_instance(instance, v_h.value_ptr(), v_h.type)) {
                pybind11_fail(
                    "pybind11_object_dealloc(): Tried to deallocate unregistered instance!");
            }

            if (instance->owned || v_h.holder_constructed()) {
                v_h.type->dealloc(v_h);
            }
        } else if (v_h.holder_constructed()) {
            v_h.type->dealloc(v_h); // Disowned instance.
        }
    }
    // Deallocate the value/holder layout internals:
    instance->deallocate_layout();

    if (instance->weakrefs) {
        PyObject_ClearWeakRefs(self);
    }

#if defined(PYBIND11_LIMITED_API)
    // py::dynamic_attr() types keep `__dict__` right after the `instance` struct (see
    // `make_new_python_type()`). The `__dict__` of Python subclasses that added their own is
    // cleared by `subtype_dealloc()` before this is called.
    if (limited_api_has_instance_dict(Py_TYPE(self))) {
        Py_CLEAR(*limited_api_instance_dict_ptr(self));
    }
#else
    PyObject **dict_ptr = _PyObject_GetDictPtr(self);
    if (dict_ptr) {
        Py_CLEAR(*dict_ptr);
    }
#endif

    if (instance->has_patients) {
        clear_patients(self);
    }
}

/// Instance destructor function for all pybind11 types. It calls `type_info.dealloc`
/// to destroy the C++ object itself, while the rest is Python bookkeeping.
extern "C" inline void pybind11_object_dealloc(PyObject *self) {
    auto *type = Py_TYPE(self);

    // If this is a GC tracked object, untrack it first
    // Note that the track call is implicitly done by the
    // default tp_alloc, which we never override.
    if (PyType_HasFeature(type, Py_TPFLAGS_HAVE_GC) != 0) {
        PyObject_GC_UnTrack(self);
    }

#if PY_VERSION_HEX >= 0x030D0000 && !defined(PYBIND11_LIMITED_API)
    // PyObject_ClearManagedDict() is available from Python 3.13+. It must be
    // called before tp_free() because on Python 3.14+ tp_free no longer
    // implicitly clears the managed dict, which would abandon the refcounts of
    // objects stored in __dict__ of py::dynamic_attr() types, causing permanent
    // memory leaks.
    if (PyType_HasFeature(type, Py_TPFLAGS_MANAGED_DICT)) {
        PyObject_ClearManagedDict(self);
    }
#endif

    clear_instance(self);

#if defined(PYBIND11_LIMITED_API)
    reinterpret_cast<freefunc>(PyType_GetSlot(type, Py_tp_free))(self);
#else
    type->tp_free(self);
#endif

    // This was not needed before Python 3.8 (Python issue 35810)
    // https://github.com/pybind/pybind11/issues/1946
    Py_DECREF(type);
}

PYBIND11_WARNING_PUSH
PYBIND11_WARNING_DISABLE_GCC("-Wredundant-decls")

std::string error_string();

PYBIND11_WARNING_POP

/** Create the type which can be used as a common base for all classes.  This is
    needed in order to satisfy Python's requirements for multiple inheritance.
    Return value: New reference. */
inline PyObject *make_object_base_type(PyTypeObject *metaclass) {
#if defined(PYBIND11_LIMITED_API)
    static PyMemberDef members[] = {
        /* Support weak references (needed for the keep_alive feature) */
        {"__weaklistoffset__",
         Py_T_PYSSIZET,
         static_cast<Py_ssize_t>(offsetof(instance, weakrefs)),
         Py_READONLY,
         nullptr},
        {nullptr, 0, 0, 0, nullptr}};
    static PyType_Slot slots[] = {{Py_tp_new, reinterpret_cast<void *>(pybind11_object_new)},
                                  {Py_tp_init, reinterpret_cast<void *>(pybind11_object_init)},
                                  {Py_tp_dealloc, reinterpret_cast<void *>(pybind11_object_dealloc)},
                                  {Py_tp_members, members},
                                  {0, nullptr}};
    static PyType_Spec spec = {PYBIND11_DUMMY_MODULE_NAME ".pybind11_object",
                               static_cast<int>(sizeof(instance)),
                               0,
                               Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
                               slots};
    PyObject *type = PyType_FromMetaclass(
        metaclass, nullptr, &spec, reinterpret_cast<PyObject *>(&PyBaseObject_Type));
    if (!type) {
        pybind11_fail("make_object_base_type(): failure in PyType_FromMetaclass(): "
                      + error_string());
    }
    assert(!PyType_HasFeature(reinterpret_cast<PyTypeObject *>(type), Py_TPFLAGS_HAVE_GC));
    return type;
#else
    constexpr auto *name = "pybind11_object";
    auto name_obj = reinterpret_steal<object>(PYBIND11_FROM_STRING(name));

    /* Danger zone: from now (and until PyType_Ready), make sure to
       issue no Python C API calls which could potentially invoke the
       garbage collector (the GC will call type_traverse(), which will in
       turn find the newly constructed type in an invalid state) */
    auto *heap_type = reinterpret_cast<PyHeapTypeObject *>(metaclass->tp_alloc(metaclass, 0));
    if (!heap_type) {
        pybind11_fail("make_object_base_type(): error allocating type!");
    }

    heap_type->ht_name = name_obj.inc_ref().ptr();
#ifdef PYBIND11_BUILTIN_QUALNAME
    heap_type->ht_qualname = name_obj.inc_ref().ptr();
#endif

    auto *type = &heap_type->ht_type;
    type->tp_name = name;
    type->tp_base = type_incref(&PyBaseObject_Type);
    type->tp_basicsize = static_cast<ssize_t>(sizeof(instance));
    type->tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HEAPTYPE;

    type->tp_new = pybind11_object_new;
    type->tp_init = pybind11_object_init;
    type->tp_dealloc = pybind11_object_dealloc;

    /* Support weak references (needed for the keep_alive feature) */
    type->tp_weaklistoffset = offsetof(instance, weakrefs);

    if (PyType_Ready(type) < 0) {
        pybind11_fail("PyType_Ready failed in make_object_base_type(): " + error_string());
    }

    setattr(reinterpret_cast<PyObject *>(type), "__module__", str(PYBIND11_DUMMY_MODULE_NAME));
    PYBIND11_SET_OLDPY_QUALNAME(type, name_obj);

    assert(!PyType_HasFeature(type, Py_TPFLAGS_HAVE_GC));
    return reinterpret_cast<PyObject *>(heap_type);
#endif
}

/// dynamic_attr: Allow the garbage collector to traverse the internal instance `__dict__`.
extern "C" inline int pybind11_traverse(PyObject *self, visitproc visit, void *arg) {
#if defined(PYBIND11_LIMITED_API)
    Py_VISIT(*limited_api_instance_dict_ptr(self));
#elif PY_VERSION_HEX >= 0x030D0000
    int ret = PyObject_VisitManagedDict(self, visit, arg);
    if (ret) {
        return ret;
    }
#else
    PyObject *&dict = *_PyObject_GetDictPtr(self);
    Py_VISIT(dict);
#endif
    // https://docs.python.org/3/c-api/typeobj.html#c.PyTypeObject.tp_traverse
    Py_VISIT(Py_TYPE(self));
    return 0;
}

/// dynamic_attr: Allow the GC to clear the dictionary.
extern "C" inline int pybind11_clear(PyObject *self) {
#if defined(PYBIND11_LIMITED_API)
    Py_CLEAR(*limited_api_instance_dict_ptr(self));
#elif PY_VERSION_HEX >= 0x030D0000
    PyObject_ClearManagedDict(self);
#else
    PyObject *&dict = *_PyObject_GetDictPtr(self);
    Py_CLEAR(dict);
#endif
    return 0;
}

#if !defined(PYBIND11_LIMITED_API)
/// Give instances of this type a `__dict__` and opt into garbage collection.
inline void enable_dynamic_attributes(PyHeapTypeObject *heap_type) {
    auto *type = &heap_type->ht_type;
    type->tp_flags |= Py_TPFLAGS_HAVE_GC;
#ifdef PYBIND11_BACKWARD_COMPATIBILITY_TP_DICTOFFSET
    type->tp_dictoffset = type->tp_basicsize;           // place dict at the end
    type->tp_basicsize += (ssize_t) sizeof(PyObject *); // and allocate enough space for it
#else
    type->tp_flags |= Py_TPFLAGS_MANAGED_DICT;
#endif
    type->tp_traverse = pybind11_traverse;
    type->tp_clear = pybind11_clear;

    static PyGetSetDef getset[]
        = {{"__dict__", PyObject_GenericGetDict, PyObject_GenericSetDict, nullptr, nullptr},
           {nullptr, nullptr, nullptr, nullptr, nullptr}};
    type->tp_getset = getset;
}
#endif

/// buffer_protocol: Fill in the view as specified by flags.
extern "C" inline int pybind11_getbuffer(PyObject *obj, Py_buffer *view, int flags) {
    // Look for a `get_buffer` implementation in this type's info or any bases (following MRO).
    type_info *tinfo = nullptr;
    for (auto type : reinterpret_steal<tuple>(get_type_mro(Py_TYPE(obj)))) {
        tinfo = get_type_info((PyTypeObject *) type.ptr());
        if (tinfo && tinfo->get_buffer) {
            break;
        }
    }
    if (view == nullptr || !tinfo || !tinfo->get_buffer) {
        if (view) {
            view->obj = nullptr;
        }
        set_error(PyExc_BufferError, "pybind11_getbuffer(): Internal error");
        return -1;
    }
    std::memset(view, 0, sizeof(Py_buffer));
    std::unique_ptr<buffer_info> info = nullptr;
    try {
        info.reset(tinfo->get_buffer(obj, tinfo->get_buffer_data));
    } catch (...) {
        try_translate_exceptions();
        raise_from(PyExc_BufferError, "Error getting buffer");
        return -1;
    }
    if (info == nullptr) {
        pybind11_fail("FATAL UNEXPECTED SITUATION: tinfo->get_buffer() returned nullptr.");
    }

    if ((flags & PyBUF_WRITABLE) == PyBUF_WRITABLE && info->readonly) {
        // view->obj = nullptr;  // Was just memset to 0, so not necessary
        set_error(PyExc_BufferError, "Writable buffer requested for readonly storage");
        return -1;
    }

    // Fill in all the information, and then downgrade as requested by the caller, or raise an
    // error if that's not possible.
    view->itemsize = info->itemsize;
    view->len = view->itemsize;
    for (auto s : info->shape) {
        view->len *= s;
    }
    view->ndim = static_cast<int>(info->ndim);
    view->shape = info->shape.data();
    view->strides = info->strides.data();
    view->readonly = static_cast<int>(info->readonly);
    if ((flags & PyBUF_FORMAT) == PyBUF_FORMAT) {
        view->format = const_cast<char *>(info->format.c_str());
    }

    // Note, all contiguity flags imply PyBUF_STRIDES and lower.
    if ((flags & PyBUF_C_CONTIGUOUS) == PyBUF_C_CONTIGUOUS) {
        if (PyBuffer_IsContiguous(view, 'C') == 0) {
            std::memset(view, 0, sizeof(Py_buffer));
            set_error(PyExc_BufferError,
                      "C-contiguous buffer requested for discontiguous storage");
            return -1;
        }
    } else if ((flags & PyBUF_F_CONTIGUOUS) == PyBUF_F_CONTIGUOUS) {
        if (PyBuffer_IsContiguous(view, 'F') == 0) {
            std::memset(view, 0, sizeof(Py_buffer));
            set_error(PyExc_BufferError,
                      "Fortran-contiguous buffer requested for discontiguous storage");
            return -1;
        }
    } else if ((flags & PyBUF_ANY_CONTIGUOUS) == PyBUF_ANY_CONTIGUOUS) {
        if (PyBuffer_IsContiguous(view, 'A') == 0) {
            std::memset(view, 0, sizeof(Py_buffer));
            set_error(PyExc_BufferError, "Contiguous buffer requested for discontiguous storage");
            return -1;
        }

    } else if ((flags & PyBUF_STRIDES) != PyBUF_STRIDES) {
        // If no strides are requested, the buffer must be C-contiguous.
        // https://docs.python.org/3/c-api/buffer.html#contiguity-requests
        if (PyBuffer_IsContiguous(view, 'C') == 0) {
            std::memset(view, 0, sizeof(Py_buffer));
            set_error(PyExc_BufferError,
                      "C-contiguous buffer requested for discontiguous storage");
            return -1;
        }

        view->strides = nullptr;

        // Since this is a contiguous buffer, it can also pretend to be 1D.
        if ((flags & PyBUF_ND) != PyBUF_ND) {
            view->shape = nullptr;
            view->ndim = 0;
        }
    }

    // Set these after all checks so they don't leak out into the caller, and can be automatically
    // cleaned up on error.
    view->buf = info->ptr;
    view->internal = info.release();
    view->obj = obj;
    Py_INCREF(view->obj);
    return 0;
}

/// buffer_protocol: Release the resources of the buffer.
extern "C" inline void pybind11_releasebuffer(PyObject *, Py_buffer *view) {
    delete (buffer_info *) view->internal;
}

#if !defined(PYBIND11_LIMITED_API)
/// Give this type a buffer interface.
inline void enable_buffer_protocol(PyHeapTypeObject *heap_type) {
    heap_type->ht_type.tp_as_buffer = &heap_type->as_buffer;

    heap_type->as_buffer.bf_getbuffer = pybind11_getbuffer;
    heap_type->as_buffer.bf_releasebuffer = pybind11_releasebuffer;
}
#endif

/** Create a brand new Python type according to the `type_record` specification.
    Return value: New reference. */
inline PyObject *make_new_python_type(const type_record &rec) {
    auto name = reinterpret_steal<object>(PYBIND11_FROM_STRING(rec.name));

    auto qualname = name;
    if (rec.scope && !PyModule_Check(rec.scope.ptr()) && hasattr(rec.scope, "__qualname__")) {
        qualname = reinterpret_steal<object>(
            PyUnicode_FromFormat("%U.%U", rec.scope.attr("__qualname__").ptr(), name.ptr()));
    }

    object module_ = get_module_name_if_available(rec.scope);
    const auto *full_name = c_str(
#if !defined(PYPY_VERSION)
        module_ ? str(module_).cast<std::string>() + "." + rec.name :
#endif
                rec.name);

#if defined(PYBIND11_LIMITED_API)
    auto &internals = get_internals();
    auto bases = tuple(rec.bases);
    if (bases.empty()) {
        bases = make_tuple(handle(internals.instance_base));
    }
    auto *metaclass = rec.metaclass.ptr() ? reinterpret_cast<PyTypeObject *>(rec.metaclass.ptr())
                                          : internals.default_metaclass;

    std::vector<PyType_Slot> slots;
    /* Don't inherit base __init__ */
    slots.push_back({Py_tp_init, reinterpret_cast<void *>(pybind11_object_init)});
    if (rec.doc && options::show_user_defined_docstrings()) {
        // PyType_FromMetaclass() copies the docstring.
        slots.push_back({Py_tp_doc, const_cast<char *>(rec.doc)});
    }

    unsigned int flags = Py_TPFLAGS_DEFAULT;
    if (!rec.is_final) {
        flags |= Py_TPFLAGS_BASETYPE;
    }

    auto basicsize = static_cast<int>(sizeof(instance));
    static PyMemberDef dict_members[]
        = {{"__dictoffset__",
            Py_T_PYSSIZET,
            static_cast<Py_ssize_t>(sizeof(instance)),
            Py_READONLY,
            nullptr},
           {nullptr, 0, 0, 0, nullptr}};
    static PyGetSetDef dict_getset[]
        = {{"__dict__", PyObject_GenericGetDict, PyObject_GenericSetDict, nullptr, nullptr},
           {nullptr, nullptr, nullptr, nullptr, nullptr}};
    if (rec.dynamic_attr) {
        /* Give instances of this type a `__dict__` and opt into garbage collection. */
        flags |= Py_TPFLAGS_HAVE_GC;
        basicsize += static_cast<int>(sizeof(PyObject *)); // place dict at the end
        slots.push_back({Py_tp_traverse, reinterpret_cast<void *>(pybind11_traverse)});
        slots.push_back({Py_tp_clear, reinterpret_cast<void *>(pybind11_clear)});
        slots.push_back({Py_tp_members, dict_members});
        slots.push_back({Py_tp_getset, dict_getset});
    }

    if (rec.buffer_protocol) {
        slots.push_back({Py_bf_getbuffer, reinterpret_cast<void *>(pybind11_getbuffer)});
        slots.push_back({Py_bf_releasebuffer, reinterpret_cast<void *>(pybind11_releasebuffer)});
    }
    slots.push_back({0, nullptr});

    // `full_name` is owned by internals (see `c_str()`), so it outlives the type.
    PyType_Spec spec = {full_name, basicsize, 0, flags, slots.data()};
    auto *type = reinterpret_cast<PyTypeObject *>(
        PyType_FromMetaclass(metaclass, nullptr, &spec, bases.ptr()));
    if (!type) {
        pybind11_fail(std::string(rec.name) + ": PyType_FromMetaclass failed: " + error_string());
    }

    if (rec.custom_type_setup_callback) {
        rec.custom_type_setup_callback(type);
    }

    assert(!rec.dynamic_attr || PyType_HasFeature(type, Py_TPFLAGS_HAVE_GC));

    /* Register type with the parent scope */
    if (rec.scope) {
        setattr(rec.scope, rec.name, reinterpret_cast<PyObject *>(type));
    } else {
        Py_INCREF(type); // Keep it alive forever (reference leak)
    }

    if (module_) { // Needed by pydoc
        setattr(reinterpret_cast<PyObject *>(type), "__module__", module_);
    }
    setattr(reinterpret_cast<PyObject *>(type), "__qualname__", qualname);

    return reinterpret_cast<PyObject *>(type);
#else

    char *tp_doc = nullptr;
    if (rec.doc && options::show_user_defined_docstrings()) {
        /* Allocate memory for docstring (Python will free this later on) */
        size_t size = std::strlen(rec.doc) + 1;
#if PY_VERSION_HEX >= 0x030D0000
        tp_doc = static_cast<char *>(PyMem_MALLOC(size));
#else
        tp_doc = (char *) PyObject_MALLOC(size);
#endif
        std::memcpy((void *) tp_doc, rec.doc, size);
    }

    auto &internals = get_internals();
    auto bases = tuple(rec.bases);
    auto *base = (bases.empty()) ? internals.instance_base : bases[0].ptr();

    /* Danger zone: from now (and until PyType_Ready), make sure to
       issue no Python C API calls which could potentially invoke the
       garbage collector (the GC will call type_traverse(), which will in
       turn find the newly constructed type in an invalid state) */
    auto *metaclass = rec.metaclass.ptr() ? reinterpret_cast<PyTypeObject *>(rec.metaclass.ptr())
                                          : internals.default_metaclass;

    auto *heap_type = reinterpret_cast<PyHeapTypeObject *>(metaclass->tp_alloc(metaclass, 0));
    if (!heap_type) {
        pybind11_fail(std::string(rec.name) + ": Unable to create type object!");
    }

    heap_type->ht_name = name.release().ptr();
#ifdef PYBIND11_BUILTIN_QUALNAME
    heap_type->ht_qualname = qualname.inc_ref().ptr();
#endif

    auto *type = &heap_type->ht_type;
    type->tp_name = full_name;
    type->tp_doc = tp_doc;
    type->tp_base = type_incref(reinterpret_cast<PyTypeObject *>(base));
    type->tp_basicsize = static_cast<ssize_t>(sizeof(instance));
    if (!bases.empty()) {
        type->tp_bases = bases.release().ptr();
    }

    /* Don't inherit base __init__ */
    type->tp_init = pybind11_object_init;

    /* Supported protocols */
    type->tp_as_number = &heap_type->as_number;
    type->tp_as_sequence = &heap_type->as_sequence;
    type->tp_as_mapping = &heap_type->as_mapping;
    type->tp_as_async = &heap_type->as_async;

    /* Flags */
    type->tp_flags |= Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE;
    if (!rec.is_final) {
        type->tp_flags |= Py_TPFLAGS_BASETYPE;
    }

    if (rec.dynamic_attr) {
        enable_dynamic_attributes(heap_type);
    }

    if (rec.buffer_protocol) {
        enable_buffer_protocol(heap_type);
    }

    if (rec.custom_type_setup_callback) {
        rec.custom_type_setup_callback(heap_type);
    }

    if (PyType_Ready(type) < 0) {
        pybind11_fail(std::string(rec.name) + ": PyType_Ready failed: " + error_string());
    }

    assert(!rec.dynamic_attr || PyType_HasFeature(type, Py_TPFLAGS_HAVE_GC));

    /* Register type with the parent scope */
    if (rec.scope) {
        setattr(rec.scope, rec.name, reinterpret_cast<PyObject *>(type));
    } else {
        Py_INCREF(type); // Keep it alive forever (reference leak)
    }

    if (module_) { // Needed by pydoc
        setattr(reinterpret_cast<PyObject *>(type), "__module__", module_);
    }

    PYBIND11_SET_OLDPY_QUALNAME(type, qualname);

    return reinterpret_cast<PyObject *>(type);
#endif
}

PYBIND11_NAMESPACE_END(detail)
PYBIND11_NAMESPACE_END(PYBIND11_NAMESPACE)
