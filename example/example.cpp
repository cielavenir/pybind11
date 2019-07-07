#include <vector>
#include <pybind11/pybind11.h>
// implicitly conversion to std::vector<int>
#include <pybind11/stl.h>

namespace py = pybind11;

// int add(int i, int j) {
//     return i + j;
// }

struct Foo {
public:
    Foo() {}
    ~Foo() {}
    void SetVecInt(std::vector<int>&& vecint_) { vecint = std::move(vecint_); }
    const std::vector<int>& GetVecInt() const& { return vecint; }
    std::vector<int> vecint;

    std::vector<std::vector<double>> matdouble;
};

// PYBIND11_MODULE(example, m) {
//     m.doc() = "pybind11 example plugin"; // optional module docstring

//     m.def("add", &add, "A function which adds two numbers");
// }

PYBIND11_MODULE(example, m)
{
	m.doc() = "";
	using namespace py::literals;

	py::class_<Foo>(m, "Foo")
    .def(py::init<>())
    .def("SetVecInt", [](Foo &self, std::vector<int>&& list) {
        self.SetVecInt(std::move(list));
    }, "Set a vector of integers to `vecint`")
    .def("GetVecInt", [](const Foo &self) {
        return self.vecint;
    }, "Get `vecint`, a vector of integers")
    .def_readwrite("vecint", &Foo::vecint)
    //
    .def("SetMatDouble", [](Foo &self, std::vector<std::vector<double>>&& list) {
        self.matdouble = std::move(list);
    }, "Set a matrix of doubles to `matdouble`")
    .def("GetMatDouble", [](const Foo &self) {
        return self.matdouble;
    }, "Get `matdouble`, a matrix of doubles")
    .def_readwrite("matdouble", &Foo::matdouble)
    ;
}