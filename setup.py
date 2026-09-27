from __future__ import annotations

from pathlib import Path

from setuptools import Extension, setup

ROOT = Path(__file__).parent

setup(
    ext_modules=[
        Extension(
            "rghport.archive._lzo_native",
            sources=[
                str(ROOT / "Cpp Src" / "src" / "native_lzo.cpp"),
                str(ROOT / "Cpp Src" / "LZO" / "minilzo" / "minilzo.c"),
            ],
            include_dirs=[
                str(ROOT / "Cpp Src" / "Pybind11" / "include"),
                str(ROOT / "Cpp Src" / "LZO" / "minilzo"),
                str(ROOT / "Cpp Src" / "LZO" / "include"),
                str(ROOT / "Cpp Src" / "LZO" / "include" / "lzo"),
            ],
            language="c++",
            extra_compile_args=["/std:c++17"] if __import__("sys").platform == "win32" else ["-std=c++17"],
        )
    ]
)
