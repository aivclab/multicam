#from distutils.core import setup, Extension
from setuptools import setup, Extension, find_packages
from setuptools.command.build_ext import build_ext
import subprocess

backend = Extension('multicam.backend',
    define_macros = [('HAVE_JPEG',), ('NPY_NO_DEPRECATED_API','NPY_1_7_API_VERSION')],
    include_dirs  = ['libyuv/include'],
    libraries     = [':libyuv.a', ':libjpeg.so.8', 'stdc++'],
    library_dirs  = ['libyuv/out'],
    sources       = ['src/multicam.c', 'src/v4l2.c'],
    extra_compile_args = [],
    extra_link_args    = [],
)


class Buildlibyuv(build_ext):
    def run(self):
        subprocess.call("./build_libyuv.sh")
        build_ext.run(self)

setup(
    name='multicam',
    version='1.0',
    ext_modules=[backend],
    packages=find_packages(),
    cmdclass={'build_ext': Buildlibyuv},
)
