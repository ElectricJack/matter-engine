// geogram/version.h — checkout-portability shim.
//
// Upstream geogram ships src/lib/geogram/version.h as a symlink to
// basic/version.h, and basic/common.cpp includes <geogram/version.h>.
//
// Git only materialises symlinks as symlinks when the checkout supports them.
// On Windows — both worktrees and plain clones without symlink support — the
// tracked symlink lands as a 15-byte text file containing the literal string
// "basic/version.h", which the compiler then tries to parse as C++:
//
//     geogram/version.h:1:1: error: 'basic' does not name a type
//
// Rewriting the file in place would make it differ from its blob and show up
// as a permanent local modification. Instead this directory is placed ahead of
// thirdparty/geogram/src/lib on the include path, so <geogram/version.h>
// resolves here and forwards to the real header. Nothing tracked is touched
// and the build behaves identically on every platform.

#ifndef AUTOREMESHER_GEOGRAM_VERSION_COMPAT_H
#define AUTOREMESHER_GEOGRAM_VERSION_COMPAT_H

#include <geogram/basic/version.h>

#endif // AUTOREMESHER_GEOGRAM_VERSION_COMPAT_H
