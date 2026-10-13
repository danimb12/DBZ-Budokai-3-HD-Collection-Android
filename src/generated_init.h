// dbz3 - picks the codegen init header of the build variant.
// US and dual-region builds use generated/ (US codegen); the EU-only core
// (Android) only has generated_eu/.
#pragma once
#if defined(DBZ3_EU_VARIANT) && !defined(DBZ3_DUAL_REGION)
#include "generated_eu/dbz3_eu_init.h"
#else
#include "generated/dbz3_init.h"
#endif
