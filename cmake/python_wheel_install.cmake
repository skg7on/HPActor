# =============================================================================
# python_wheel_install.cmake — install rules for the python-wheel component
# =============================================================================
#
# Included when HPACTOR_PYTHON_WHEEL_BUILD is ON.  Installs the _hpactor
# extension module and its private runtime libraries into a layout that
# scikit-build-core can consume.
#
# Layout inside the wheel (relative to site-packages):
#   hpactor/
#     _hpactor.cpython-311-*.so   # extension module
#     .libs/
#       libhpactor_lib.so         # private runtime
#       libhpactor_proto.so       # private runtime
#
# Only the extension module exports PyInit__hpactor; private libraries use
# relative RPATH ($ORIGIN/.libs on Linux, @loader_path/.libs on macOS).

if(NOT HPACTOR_PYTHON_WHEEL_BUILD)
  return()
endif()

# ── Extension module ──────────────────────────────────────────────────────
install(TARGETS _hpactor
  LIBRARY DESTINATION hpactor
  COMPONENT python-wheel)

# ── Private runtime libraries ─────────────────────────────────────────────
# These are hpactor-internal shared libraries that must be bundled inside
# the wheel.  They are NOT part of the public ABI and must not be installed
# with headers, pkg-config files, or CMake exports.
install(TARGETS hpactor_lib hpactor_proto
  LIBRARY DESTINATION hpactor/.libs
  COMPONENT python-wheel)

# ── Relative RPATH ────────────────────────────────────────────────────────
# Ensure the extension can find its private libraries without an absolute
# path that would break at install time.

if(APPLE)
  set_property(TARGET _hpactor PROPERTY INSTALL_RPATH "@loader_path/.libs")
  set_property(TARGET hpactor_lib PROPERTY INSTALL_RPATH "@loader_path")
  set_property(TARGET hpactor_proto PROPERTY INSTALL_RPATH "@loader_path")
else()
  set_property(TARGET _hpactor PROPERTY INSTALL_RPATH "$ORIGIN/.libs")
  set_property(TARGET hpactor_lib PROPERTY INSTALL_RPATH "$ORIGIN")
  set_property(TARGET hpactor_proto PROPERTY INSTALL_RPATH "$ORIGIN")
endif()
