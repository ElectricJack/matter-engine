// Bounded cross-receiver material proof. All positions/heights are metres,
// colours linear. These are original mesh categories, not source carriers.
export const CONTACT_RECEIVERS = Object.freeze({ ground: 16, rock: 11, wall: 9, excluded: 17 });
// Streamed worlds install requires() variants, then place them through their
// owning sector. Each kind here has one placement and therefore one world frame.
export const CONTACT_PLACEMENTS = Object.freeze([
  { kind: 'wall', position: [8,0,8] },
  { kind: 'rock', position: [6.8,0,8.8] },
  { kind: 'shelf', position: [9.5,0.65,8.7] },
  { kind: 'remote', position: [8,0,6] },
]);

export function surfaceContact(s, enabled = true) {
  const category = id => s.receiverMaterial.sub(id).abs().clamp(0, 1).oneMinus();
  const rock = category(CONTACT_RECEIVERS.rock);
  const wall = category(CONTACT_RECEIVERS.wall);
  const excluded = category(CONTACT_RECEIVERS.excluded);
  const allowed = category(CONTACT_RECEIVERS.ground).add(rock).add(wall).clamp(0, 1);
  const fine = s.footprint.smoothstep(0.008, 0.045).oneMinus();
  const reliefFade = s.footprint.smoothstep(0.035, 0.32).oneMinus();
  const macro = s.noise3World(7281, 0.65, 2);
  const body = s.noise3World(542, 4.5, 2);
  const grain = s.noise3World(782, 29, 1).mul(fine);
  const crack = s.ridge3World(185, 8, 2).smoothstep(0.56, 0.88).mul(reliefFade);
  const tone = macro.mul(0.018).add(body.mul(0.009)).add(grain.mul(0.003));
  const soil = { baseColor: [tone.add(0.115), tone.mul(0.85).add(0.084), tone.mul(0.6).add(0.052)],
    roughness: 0.96, height: body.mul(0.006).mul(reliefFade).add(grain.mul(0.001)).sub(0.020),
    heightRange: [-0.045, -0.008] };
  const stone = { baseColor: [tone.add(0.22), tone.add(0.21), tone.mul(0.85).add(0.175)],
    roughness: 0.86, height: body.mul(0.011).mul(reliefFade).sub(crack.mul(0.010))
      .add(grain.mul(0.001)).sub(0.020), heightRange: [-0.045, -0.008] };
  // Large dressed foundation blocks; the geometric wall stays a simple box.
  const v = s.altitude.mul(1 / 0.24), row = v.sub(v.fract());
  const u = s.worldX.mul(1 / 0.48).add(row.mul(0.5).fract());
  const edge = u.fract().sub(0.5).abs().mul(0.48).sub(0.231)
    .max(v.fract().sub(0.5).abs().mul(0.24).sub(0.111));
  const face = edge.mul(-1).mul(1 / 0.012).smoothstep(0, 1);
  const block = s.cellNoise2(893, u.sub(u.fract()), row).sub(0.5);
  const masonry = { baseColor: [0.25, 0.21, 0.155].map((c, i) =>
    s.blend([0.11, 0.102, 0.08][i], tone.add(block.mul(0.035)).add(c), face)),
    roughness: 0.90, height: face.mul(0.013).sub(0.032).add(body.mul(0.002))
      .add(grain.mul(0.0005)), heightRange: [-0.045, -0.008] };
  let surface = s.layer(soil, stone, { coverage: rock, width: 0.01 });
  surface = s.layer(surface, masonry, { coverage: wall.add(excluded), width: 0.01 });
  if (!enabled) return surface;

  // Authored volume restricts projection depth and height. Receiver identity
  // additionally excludes wall back/cap and the elevated negative control,
  // even where those surfaces overlap the volume. No normal-dependent height.
  const placement = { shape: 'box', anchor: 'world', center: [8, 0.22, 8.5],
    halfSize: [2.8, 0.95, 1.35], feather: 0.22 };
  const low = s.altitude.add(macro.mul(0.42)).smoothstep(0.16, 0.95).oneMinus();
  const contact = allowed.mul(low);
  surface = s.splat(surface,
    { baseColor: surface.baseColor.map(c => c.mul(0.64)), roughness: 0.72 }, placement,
    { operation: 'appearance', coverage: contact.mul(0.85) });
  const soilFilm = { baseColor: [tone.add(0.080), tone.mul(0.8).add(0.062), tone.mul(0.5).add(0.036)],
    roughness: 0.94, height: body.mul(0.0015).add(0.0035), heightRange: [0.002, 0.005] };
  surface = s.splat(surface, soilFilm, placement,
    { operation: 'deposit', coverage: contact.mul(body.smoothstep(-0.4, 0.32)).mul(0.80), width: 0.009 });
  const growth = macro.add(body.mul(0.20)).smoothstep(-0.18, 0.20);
  const moss = { baseColor: [grain.mul(0.006).add(0.052), grain.mul(0.009).add(0.070),
      grain.mul(0.003).add(0.025)], roughness: 0.96,
    height: grain.mul(0.0005).add(body.mul(0.0008)).add(0.002), heightRange: [0.0007, 0.0033] };
  return s.splat(surface, moss, placement,
    { operation: 'deposit', coverage: contact.mul(growth), width: 0.008 });
}
