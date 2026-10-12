# SPDX-License-Identifier: MIT

# foo_input_csf: the foobar2000 component, a DLL built with clang-cl or MSVC on Windows and a .component bundle on
# macOS. Added by the top-level CMakeLists.txt when FOOBAR2000_SDK_DIR is set. See README.md. xsflib builds it from the
# SDK's sources (xsflib_add_foobar2000_component).

# The SDK's Objective-C++ files build on macOS. CMake allows enable_language only at file scope.
if (APPLE)
    enable_language(OBJCXX)
endif()

xsflib_add_foobar2000_component(foo_input_csf SDK ${FOOBAR2000_SDK_DIR} SOURCES foo_input_csf.cpp
                                LIBRARIES csf_player VERSION ${csf_VERSION} BUNDLE_ID com.foobar2000.foo-input-csf
                                OPTIONS ${CSF_WARNINGS})
csf_add_version(foo_input_csf "CSF decoder for foobar2000")
