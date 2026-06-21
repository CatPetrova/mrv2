// SPDX-License-Identifier: BSD-3-Clause
// mrv2
// Copyright Contributors to the mrv2 Project. All rights reserved.

#include "mrvCore/mrvLicensing.h"

namespace mrv
{
    namespace app
    {
        bool demo_mode = false;
        bool force_demo = false;

        std::string session_id = "";
        LicenseType license_type = LicenseType::kNodeLocked;
        
        bool soporta_hdr = true;
        bool soporta_layers = true;
        bool soporta_saving = true;
        
        bool soporta_annotations = true;
        bool soporta_editing = true;
        bool soporta_python = true;
        bool soporta_voice = true;

    }
}
