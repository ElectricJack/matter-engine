// Nominal physical sizes for a small reusable library. Instance transforms
// supply only the residual scale, so pores and relief stay near their authored
// metre sizes instead of growing by two orders of magnitude with the boulder.
export const MOUNTAIN_ROCK_REFERENCE_SIZES = Object.freeze([.5,2,8,32]);

export function checkedMountainRockSize(value) {
  if (!Number.isFinite(value) || value < .25 || value > 64)
    throw new RangeError('MountainRock size must be a finite number in [.25,64] metres');
  return value;
}

export function mountainRockReferenceForSize(sizeM) {
  checkedMountainRockSize(sizeM);
  return MOUNTAIN_ROCK_REFERENCE_SIZES.find(reference=>sizeM<=2*reference);
}
