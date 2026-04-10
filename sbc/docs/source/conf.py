# Configuration file for the Sphinx documentation builder.

import os
import sys

sys.path.insert(0, os.path.abspath("../.."))

# -- Project information -------------------------------------------------------

project = "Image Classification Unit - SBC"
copyright = "2026, Miguel Chaparro"
author = "Miguel Chaparro"

# -- General configuration -----------------------------------------------------

extensions = [
    # API documentation
    "sphinx.ext.autodoc",
    "sphinx.ext.napoleon",
    "sphinx.ext.viewcode",
    "sphinx.ext.intersphinx",
    # Diagrams
    "sphinx.ext.graphviz",
    "sphinxcontrib.mermaid",
    # Interactive features
    "sphinx_copybutton",
    "sphinx_tabs.tabs",
    "sphinx.ext.autosectionlabel",
    # Validation
    "sphinx.ext.doctest",
]

templates_path = ["_templates"]
exclude_patterns = []

# -- Autodoc configuration -----------------------------------------------------

autodoc_mock_imports = [
    "picamera2",
    "libcamera",
    "spidev",
    "serial",
    "cv2",
    "tflite_runtime",
    "numpy",
    "piexif",
    "PIL",
]

autodoc_member_order = "bysource"
autodoc_default_options = {
    "members": True,
    "undoc-members": True,
    "show-inheritance": True,
}

# -- Napoleon configuration (Google-style docstrings) --------------------------

napoleon_google_docstring = True
napoleon_numpy_docstring = False
napoleon_include_init_with_doc = True
napoleon_include_private_with_doc = False
napoleon_include_special_with_doc = False

# -- Strict cross-reference checking ------------------------------------------

nitpicky = True
nitpick_ignore = [
    ("py:class", "serial.Serial"),
    ("py:class", "spidev.SpiDev"),
    ("py:class", "numpy.ndarray"),
    ("py:class", "np.ndarray"),
    ("py:class", "Picamera2"),
    ("py:class", "modules.processing.protocol.BufferSize"),
    ("py:class", "modules.processing.protocol.FrameIndex"),
    ("py:class", "modules.processing.protocol.FrameOffset"),
]

# -- Autosectionlabel ---------------------------------------------------------

autosectionlabel_prefix_document = True

# -- Copybutton ----------------------------------------------------------------

copybutton_prompt_text = "$ "

# -- Intersphinx ---------------------------------------------------------------

intersphinx_mapping = {
    "python": ("https://docs.python.org/3", None),
}

# -- HTML output ---------------------------------------------------------------

html_theme = "furo"
html_static_path = ["_static"]
