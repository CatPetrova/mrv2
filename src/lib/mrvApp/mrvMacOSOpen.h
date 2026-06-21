// SPDX-License-Identifier: BSD-3-Clause
// mrv2
// Copyright Contributors to the mrv2 Project. All rights reserved.

#pragma once

#ifdef __APPLE__

namespace mrv
{
    using MacOSOpenCallback = void (*)(const char*);

    void installMacOSOpenDocumentHandler(MacOSOpenCallback);
}

#endif
