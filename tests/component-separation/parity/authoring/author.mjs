// Runs one generators.mjs entry with the portable SDK and returns the exact
// canonical bytes or the explicit authoring error (code + message; the source
// location differs per host and is not compared). Shared by Node
// (node_author.mjs) and Chromium (browser/parity_page.mjs); the native host
// runs the same generator text through matter_host_quickjs_tests --stdin.
export async function author({sdk, contracts, generators, sha256}, {generator, seed, parameters}) {
  const services = sdk.contractServices(contracts, sha256);
  const context = {parameters, seed: BigInt(`0x${seed}`), services, supportedCapabilities: sdk.DEFAULT_CAPABILITIES};
  if (typeof generators[generator] !== 'function') throw new Error(`unknown generator ${generator}`);
  try {
    return {status: 'ok', canonical: contracts.canonicalJson(await generators[generator](sdk, context))};
  } catch (error) {
    if (!(error instanceof sdk.AuthoringError)) throw error;
    return {status: 'error', code: error.diagnostic.code, message: error.diagnostic.message};
  }
}
