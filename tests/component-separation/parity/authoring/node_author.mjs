// Node authoring host: reads a JSON array of {generator, seed, parameters}
// on stdin and prints a JSON array of author() outcomes.
import {readFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
import * as sdk from '../../../../packages/matter-authoring/index.mjs';
import * as contracts from '../../../../libs/MatterContracts/examples/contracts.mjs';
import * as generators from './generators.mjs';
import {author} from './author.mjs';

const sha256 = async bytes => new Uint8Array(createHash('sha256').update(bytes).digest());
const out = [];
for (const request of JSON.parse(readFileSync(0, 'utf8'))) out.push(await author({sdk, contracts, generators, sha256}, request));
process.stdout.write(JSON.stringify(out));
