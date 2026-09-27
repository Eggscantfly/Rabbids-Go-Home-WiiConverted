#pragma once

// The display-list and vertex types a model file is compiled against.  They are gbi.h's own (the vertex, light and
// matrix types, the combiner and geometry mode words) with the gs* macros replaced by libsm64's, which gbi.h now
// pulls in itself at its end (decomp/include/PR/gbi_libsm64.h).  Model files include this first.
#include "decomp/include/types.h"
#include "decomp/include/PR/gbi.h"
#include "gfx_adapter_commands.h"
