"""PEP 517 backend: bundle client sources and derive version from Goblin Core."""
from pathlib import Path
import shutil
from scikit_build_core import build as _backend

ROOT = Path(__file__).resolve().parent

def _prepare():
    source = ROOT.parent
    bundle = ROOT / '_vendor'
    if (source / 'include/goblin/core/sbe_ring_client.hpp').exists():
        bundle.mkdir(exist_ok=True)
        for directory in ['include', 'sbe/generated', 'src']:
            shutil.copytree(source / directory, bundle / directory,
                            dirs_exist_ok=True, ignore=shutil.ignore_patterns('._*', '__pycache__'))
        shutil.copy2(source / 'CMakeLists.txt', bundle / 'CMakeLists.txt')
        for filename in ['LICENSE', 'NOTICE']:
            shutil.copy2(source / filename, ROOT / filename)
    if not (bundle / 'CMakeLists.txt').exists():
        raise RuntimeError('Goblin Core client sources are missing from the source distribution')

def build_wheel(wheel_directory, config_settings=None, metadata_directory=None):
    _prepare()
    return _backend.build_wheel(wheel_directory, config_settings, metadata_directory)

def build_sdist(sdist_directory, config_settings=None):
    _prepare()
    return _backend.build_sdist(sdist_directory, config_settings)

def prepare_metadata_for_build_wheel(metadata_directory, config_settings=None):
    _prepare()
    return _backend.prepare_metadata_for_build_wheel(metadata_directory, config_settings)

def get_requires_for_build_wheel(config_settings=None):
    _prepare()
    return _backend.get_requires_for_build_wheel(config_settings)

def get_requires_for_build_sdist(config_settings=None):
    _prepare()
    return _backend.get_requires_for_build_sdist(config_settings)

def build_editable(wheel_directory, config_settings=None, metadata_directory=None):
    _prepare()
    return _backend.build_editable(wheel_directory, config_settings, metadata_directory)

def prepare_metadata_for_build_editable(metadata_directory, config_settings=None):
    _prepare()
    return _backend.prepare_metadata_for_build_editable(metadata_directory, config_settings)

def get_requires_for_build_editable(config_settings=None):
    _prepare()
    return _backend.get_requires_for_build_editable(config_settings)
