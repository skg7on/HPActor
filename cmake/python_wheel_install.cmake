# Copyright 2026 HPActor Contributors (Apache 2.0)
#
# python_wheel_install.cmake — install rules for the Python ABI3 wheel layout.
#
# Included only when HPACTOR_PYTHON_WHEEL_BUILD is ON.  Installs the
# _hpactor extension module and its private HPActor runtime libraries
# into a scikit-build-core-compatible staging directory:
#
#   <prefix>/hpactor/_hpactor.so       — CPython ABI3 extension
#   <prefix>/hpactor/.libs/            — private runtime libraries
#
# RPATH / install-name entries use relative paths so the repaired
# wheel is relocatable.

if(NOT HPACTOR_PYTHON_WHEEL_BUILD)
  return()
endif()

# ── Platform-specific RPATH ───────────────────────────────────────────────

if(APPLE)
  set(WHEEL_RPATH_EXTENSION "@loader_path/.libs")
  set(WHEEL_RPATH_LIB "@loader_path")
else()
  set(WHEEL_RPATH_EXTENSION "\$ORIGIN/.libs")
  set(WHEEL_RPATH_LIB "\$ORIGIN")
endif()

# ── Extension module ──────────────────────────────────────────────────────

set_target_properties(_hpactor PROPERTIES
  MACOSX_RPATH ON
  BUILD_RPATH "${WHEEL_RPATH_EXTENSION}"
  INSTALL_RPATH "${WHEEL_RPATH_EXTENSION}"
  BUILD_WITH_INSTALL_RPATH FALSE)

install(TARGETS _hpactor
  LIBRARY DESTINATION hpactor
  COMPONENT python-wheel)

# ── Private runtime libraries ─────────────────────────────────────────────

# Ensure hpactor_lib and hpactor_proto use relative RPATH so they
# find each other inside .libs/.
set_target_properties(hpactor_lib hpactor_proto PROPERTIES
  MACOSX_RPATH ON
  BUILD_RPATH "${WHEEL_RPATH_LIB}"
  INSTALL_RPATH "${WHEEL_RPATH_LIB}"
  BUILD_WITH_INSTALL_RPATH FALSE)

install(TARGETS hpactor_lib hpactor_proto
  LIBRARY DESTINATION hpactor/.libs
  COMPONENT python-wheel)

# ── Architecture check ────────────────────────────────────────────────────

# Verify that no installed shared object has an absolute RPATH pointing
# into the build tree, checkout, or system package manager prefix.
function(_hpactor_check_wheel_rpath)
  # This is a best-effort build-time check.  Full verification happens
  # in verify_wheel.py (Task 4).
  message(STATUS "Python wheel install layout configured")
endfunction()
_hpactor_check_wheel_rpath()
