// SPDX-License-Identifier: BSD-3-Clause
// mrv2
// Copyright Contributors to the mrv2 Project. All rights reserved.

#pragma once

#include <tlCore/Time.h>

#include <string>

namespace mrv
{
    //! Fast movie cut by remuxing compressed packets without decoding.
    //!
    //! This is keyframe-aligned.  It is intended for fast clip extraction, not
    //! for frame-exact exports or exports that apply timeline rendering.
    void fast_movie_copy(
        const std::string& inputFile, const std::string& outputFile,
        const tl::otime::RationalTime& startTime,
        const tl::otime::RationalTime& endTimeExclusive);
}
