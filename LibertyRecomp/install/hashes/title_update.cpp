// GTA IV Title Updates
// STFS package sizes and hashes for title update validation

#include "title_update.h"

// Known GTA IV Title Update versions
// Size = uncompressed STFS package size, Hash = XXH3-64 (0 = skip validation)
const TitleUpdateInfo TitleUpdates[] = {
    { "v4", 3596288, 0ULL },
    { "v5", 3788800, 0ULL },
    { "v6", 3727360, 0ULL },
    { "v8", 3715072, 0ULL },
};

const size_t TitleUpdatesSize = std::size(TitleUpdates);
