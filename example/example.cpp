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
    void SetVecInt(std::vector<int> a_) { a = std::move(a_); }
    const std::vector<int>& GetVecInt() const& { return a; }
    std::vector<int> a;
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
    .def("SetVecInt", [](Foo &self, const std::vector<int>& list) {
        self.SetVecInt(list);
    })
    .def("GetVecInt", [](const Foo &self) {
        return self.a;
    })
    ;
}