#include <pybind11/pybind11.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include "minilzo.h"
}

namespace py = pybind11;

namespace {

void ensure_lzo_ready() {
    static const int init_result = lzo_init();
    if (init_result != LZO_E_OK) {
        throw std::runtime_error("lzo_init failed with code " + std::to_string(init_result));
    }
}

std::string lzo_error(int code) {
    return "LZO returned error code " + std::to_string(code);
}

py::bytes decompress(py::bytes src, std::size_t expected_size) {
    ensure_lzo_ready();

    std::string input = src;
    std::string output(expected_size, '\0');
    lzo_uint out_len = static_cast<lzo_uint>(output.size());
    const int rc = lzo1x_decompress_safe(
        reinterpret_cast<const lzo_bytep>(input.data()),
        static_cast<lzo_uint>(input.size()),
        reinterpret_cast<lzo_bytep>(output.data()),
        &out_len,
        nullptr);

    if (rc != LZO_E_OK) {
        throw std::runtime_error(lzo_error(rc));
    }
    if (out_len != expected_size) {
        throw std::runtime_error(
            "LZO produced " + std::to_string(out_len) + " bytes, expected " + std::to_string(expected_size));
    }
    return py::bytes(output);
}

py::bytes compress(py::bytes src) {
    ensure_lzo_ready();

    std::string input = src;
    std::string output(input.size() + (input.size() / 16) + 64 + 3, '\0');
    std::vector<unsigned char> workmem(LZO1X_1_MEM_COMPRESS);
    lzo_uint out_len = static_cast<lzo_uint>(output.size());

    const int rc = lzo1x_1_compress(
        reinterpret_cast<const lzo_bytep>(input.data()),
        static_cast<lzo_uint>(input.size()),
        reinterpret_cast<lzo_bytep>(output.data()),
        &out_len,
        workmem.data());

    if (rc != LZO_E_OK) {
        throw std::runtime_error(lzo_error(rc));
    }
    output.resize(out_len);
    return py::bytes(output);
}

} // namespace

PYBIND11_MODULE(_lzo_native, m) {
    m.doc() = "Bundled miniLZO bindings used by rghport.";
    m.def("decompress", &decompress, py::arg("src"), py::arg("expected_size"));
    m.def("compress", &compress, py::arg("src"));
}
