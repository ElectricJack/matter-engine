// MatterEngine3/cli/matter_bake_cli.h -- dispatcher entry point for `matter bake gi`.
//
// The product CLI has one executable (`matter`) with several command groups.
// This handler receives only the arguments after `matter bake gi` so the
// export front end can dispatch without duplicating the GI parser.

#pragma once

int matter_bake_gi_cli_main(int argc, char** argv);
