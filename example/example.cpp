#include <vector>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"  // Woody: LOL!
#include <pybind11/pybind11.h>
#pragma GCC diagnostic pop
// implicitly conversion to std::vector<int>
#include <pybind11/eigen.h>

namespace py = pybind11;

class MyClass {
    Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic> big_mat;
public:
    MyClass() : big_mat(10000, 10000) {
        big_mat.setZero();
    }
    Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic> &getMatrix() { return big_mat; }
    const Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic> &viewMatrix() { return big_mat; }
};

PYBIND11_MODULE(example, m) {
// Later, in binding code:
py::class_<MyClass>(m, "MyClass")
    .def(py::init<>())
    .def("copy_matrix", &MyClass::getMatrix) // Makes a copy!
    .def("get_matrix", &MyClass::getMatrix, py::return_value_policy::reference_internal)
    .def("view_matrix", &MyClass::viewMatrix, py::return_value_policy::reference_internal)
    ;
}