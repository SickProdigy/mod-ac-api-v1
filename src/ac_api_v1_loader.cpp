/* This file is part of ac-api-v1. See LICENSE for license information. */

void AddAcApiV1Scripts();

// AzerothCore derives this symbol from the module directory name, replacing
// hyphens with underscores.
void Addmod_ac_api_v1Scripts()
{
    AddAcApiV1Scripts();
}
