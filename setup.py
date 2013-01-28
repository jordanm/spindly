from distutils.core import setup, Extension

module = Extension('spindly',
    libraries=['mozjs185', 'pthread'],
    include_dirs=['/usr/local/include/js', '/usr/include/js'],
    sources=['spindly.c'])

setup(
    name='spindly',
    version='0.0.1',
    ext_modules=[module])
