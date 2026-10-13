# SPDX-License-Identifier: MIT

# xmp-csf: the XMPlay input plugin, a 32-bit DLL built with clang-cl. XMPlay is a 32-bit program. Added by the top-level
# CMakeLists.txt when XMPLAY_SDK_DIR is set. See README.md. xsflib builds it against the SDK's headers
# (xsflib_add_xmplay_plugin).

xsflib_add_xmplay_plugin(xmp-csf
    SDK ${XMPLAY_SDK_DIR}
    SOURCES xmp_csf.cpp
    LIBRARIES csf_player
    OPTIONS ${CSF_WARNINGS})
csf_add_version(xmp-csf "CSF plugin for XMPlay")
