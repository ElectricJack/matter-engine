// Geometry vocabulary for the Kreuzenstein photographic study. All dimensions in metres.
export const C = {
  stone: [0.57, 0.52, 0.42],
  trim: [0.72, 0.67, 0.55],
  mortar: [0.4, 0.38, 0.33],
  roof: [0.43, 0.115, 0.065],
  wood: [0.16, 0.12, 0.09],
  plaster: [0.64, 0.61, 0.51],
  glass: [0.075, 0.12, 0.14],
  iron: [0.07, 0.065, 0.055],
};
export class CastleGeometry {
  constructor(part, seed = 7, stage = "details", stones = [8, 8, 8, 8]) {
    this.p = part;
    this.seed = seed;
    this.stage = stage;
    this.stones = stones;
  }
  rand() {
    this.seed = (Math.imul(this.seed, 1664525) + 1013904223) >>> 0;
    return this.seed / 4294967296;
  }
  mat(c = C.stone, m = MAT.stone) {
    this.p.fill(m);
    this.p.tint(c[0], c[1], c[2], 1);
  }
  box(x, y, z, w, h, d, c = C.stone, m = MAT.stone) {
    if (m === MAT.stone && (c === C.trim || c === C.stone)) {
      const nx = Math.max(1, Math.ceil(w / 1.2)),
        ny = Math.max(1, Math.ceil(h / 0.55));
      for (let ix = 0; ix < nx; ix++)
        for (let iy = 0; iy < ny; iy++)
          this.brick(
            x - w / 2 + ((ix + 0.5) * w) / nx,
            y + (iy * h) / ny,
            z,
            w / nx - 0.015,
            h / ny - 0.012,
            d,
          );
      return;
    }
    if (this.stage === "masonry") return;
    if (w <= 0 || h <= 0 || d <= 0) return;
    this.mat(c, m);
    this.p.box([x, y + h / 2, z], [w / 2, h / 2, d / 2]);
  }
  faces(faces, c = C.stone, m = MAT.stone) {
    if (this.stage === "masonry") return;
    this.mat(c, m);
    this.p.beginShape(SHAPE.triangles);
    for (const f of faces)
      for (let i = 1; i < f.length - 1; i++)
        for (const v of [f[0], f[i], f[i + 1]]) this.p.vertex(...v);
    this.p.endShape();
  }
  beam(a, b, r, c = C.wood, m = MAT.bark) {
    if (this.stage === "masonry") return;
    this.mat(c, m);
    this.p.cylinder(a, b, r);
  }
  local(x, y, z, a, fn) {
    this.p.pushMatrix();
    this.p.translate(x, y, z);
    this.p.rotateY(a);
    fn();
    this.p.popMatrix();
  }
  brick(x, y, z, w, h, d, a = 0, roll = 0) {
    if (this.stage !== "masonry" || w <= 0 || h <= 0 || d <= 0) return;
    const seed = Math.floor(this.rand() * 4);
    this.p.pushMatrix();
    this.p.translate(x, y + h / 2, z);
    this.p.rotateY(a);
    this.p.rotateZ(roll);
    this.p.scale(w * 1.025, h * 1.025, d * 1.025);
    this.p.placeChild("KreuzensteinBrick", {
      seed,
      material: this.stones[seed],
    });
    this.p.popMatrix();
  }
  stoneColor() {
    const v = 0.84 + this.rand() * 0.29;
    return C.stone.map((x, i) => x * v + (i === 0 ? this.rand() * 0.035 : 0));
  }
  // Shallow relief coursing, with staggered joints and independently shaded stones.
  masonry(x, y, z, w, h, course = 0.48, skip = null) {
    for (let row = 0; row < Math.ceil(h / course); row++) {
      const by = y + row * course,
        bh = Math.min(course, h - row * course);
      let a = -w / 2;
      while (a < w / 2 - 0.02) {
        const bw = Math.min(0.65 + this.rand() * 0.75, w / 2 - a),
          cx = x + a + bw / 2;
        if (!skip || !skip(cx, by + bh / 2, bw, bh))
          this.brick(
            cx,
            by + 0.014,
            z,
            bw - 0.024,
            bh - 0.024,
            0.36 + this.rand() * 0.07,
          );
        a += bw;
      }
    }
  }
  block(x, y, z, w, h, d, detail = true) {
    // Closed masonry shell: every stone is an independently placed voxel part.
    this.masonry(x, y, z + d / 2 - 0.18, w, h);
    this.local(x + w / 2 - 0.18, 0, z, Math.PI / 2, () =>
      this.masonry(0, y, 0, Math.max(0.2, d - 0.65), h),
    );
    this.local(x - w / 2 + 0.18, 0, z, -Math.PI / 2, () =>
      this.masonry(0, y, 0, Math.max(0.2, d - 0.65), h),
    );
    this.local(x, 0, z - d / 2 + 0.18, Math.PI, () =>
      this.masonry(0, y, 0, w, h),
    );
    this.box(x, y + h - 0.15, z, w, 0.15, d, C.mortar);
  }
  frustum(x, y, z, r0, r1, h, c = C.stone, n = 48) {
    const f = [];
    for (let i = 0; i < n; i++) {
      const a = (i * 2 * Math.PI) / n,
        b = ((i + 1) * 2 * Math.PI) / n,
        A = [x + r0 * Math.cos(a), y, z + r0 * Math.sin(a)],
        B = [x + r0 * Math.cos(b), y, z + r0 * Math.sin(b)],
        D = [x + r1 * Math.cos(a), y + h, z + r1 * Math.sin(a)],
        E = [x + r1 * Math.cos(b), y + h, z + r1 * Math.sin(b)];
      f.push([B, A, D, E], [[x, y, z], A, B], [[x, y + h, z], E, D]);
    }
    this.faces(f, c);
  }
  drum(x, y, z, r, h, course = 0.48) {
    for (let row = 0; row < Math.ceil(h / course); row++) {
      const bh = Math.min(course, h - row * course),
        n = Math.ceil((2 * Math.PI * r) / 0.95);
      for (let i = 0; i < n; i++) {
        const a = ((i + (row % 2) * 0.5) * 2 * Math.PI) / n;
        this.brick(
          x + (r - 0.22) * Math.sin(a),
          y + row * course + 0.012,
          z + (r - 0.22) * Math.cos(a),
          (2 * Math.PI * (r - 0.22)) / n - 0.014,
          bh - 0.022,
          0.52,
          a,
        );
      }
    }
  }
  arcBlock(x, y, z, ri, ro, h, a, b, c = C.stone) {
    const t = (a + b) / 2,
      r = (ri + ro) / 2;
    this.brick(
      x + r * Math.cos(t),
      y,
      z + r * Math.sin(t),
      r * (b - a),
      h,
      Math.max(0.36, ro - ri),
      Math.PI / 2 - t,
    );
  }
  roofTiles(a, b, c, d, rows) {
    if (this.stage === "masonry") return;
    const mix = (u, v, t) => u.map((x, i) => x + (v[i] - x) * t);
    for (let row = 0; row < rows; row++) {
      const t0 = (row + 0.02) / rows,
        t1 = Math.min(0.999, (row + 1.035) / rows);
      const l0 = mix(a, d, t0),
        r0 = mix(b, c, t0),
        l1 = mix(a, d, t1),
        r1 = mix(b, c, t1);
      const count = Math.max(
        1,
        Math.ceil(Math.hypot(...r0.map((x, i) => x - l0[i])) / 0.36),
      );
      for (let col = 0; col < count; col++) {
        const u0 = (col + 0.015) / count,
          u1 = (col + 0.985) / count;
        const q = [
          mix(l0, r0, u0),
          mix(l0, r0, u1),
          mix(l1, r1, u1),
          mix(l1, r1, u0),
        ];
        for (const v of q) v[1] += 0.032;
        const shade = 0.87 + this.rand() * 0.25,
          color = [0.43 * shade, 0.14 * shade, 0.085 * shade];
        this.faces([q], color);
        const lower = q.slice(0, 2).map((v) => [v[0], v[1] - 0.045, v[2]]);
        this.faces(
          [[lower[0], lower[1], q[1], q[0]]],
          color.map((v) => v * 0.8),
        );
      }
    }
  }
  hip(x, y, z, w, d, h, ridge = 0) {
    const a = [x - w / 2, y, z + d / 2],
      b = [x + w / 2, y, z + d / 2],
      c = [x + w / 2, y, z - d / 2],
      e = [x - w / 2, y, z - d / 2],
      u = [x, y + h, z + ridge / 2],
      v = [x, y + h, z - ridge / 2];
    this.faces(
      [
        [a, b, u],
        [b, c, v, u],
        [c, e, v],
        [e, a, u, v],
      ],
      C.roof,
    );
    this.box(x, y - 0.2, z, w, 0.22, d, C.wood, MAT.bark);
    const n = Math.max(3, Math.ceil(h / 0.24));
    this.roofTiles(a, b, u, u, n);
    this.roofTiles(b, c, v, u, n);
    this.roofTiles(c, e, v, v, n);
    this.roofTiles(e, a, u, v, n);
    if (ridge > 0.01) this.beam(u, v, 0.09, [0.47, 0.16, 0.09], MAT.stone);
  }
  coneRoof(x, y, z, r, h) {
    this.frustum(x, y, z, r, 0.035, h, C.roof, 80);
    this.frustum(x, y - 0.2, z, r + 0.04, r + 0.04, 0.22, C.wood, 64);
    if (this.stage === "details")
      for (let yy = 0; yy < h - 0.1; yy += 0.24) {
        const rr = r * (1 - yy / h) + 0.025,
          rr1 = Math.max(0.02, r * (1 - (yy + 0.255) / h) + 0.025),
          n = Math.max(6, Math.ceil((2 * Math.PI * rr) / 0.34));
        for (let i = 0; i < n; i++) {
          const a = ((i + 0.015) * Math.PI * 2) / n,
            b = ((i + 0.985) * Math.PI * 2) / n;
          const shade = 0.9 + this.rand() * 0.22;
          this.faces(
            [
              [
                [x + rr * Math.cos(b), y + yy + 0.02, z + rr * Math.sin(b)],
                [x + rr * Math.cos(a), y + yy + 0.02, z + rr * Math.sin(a)],
                [x + rr1 * Math.cos(a), y + yy + 0.275, z + rr1 * Math.sin(a)],
                [x + rr1 * Math.cos(b), y + yy + 0.275, z + rr1 * Math.sin(b)],
              ],
            ],
            [0.43 * shade, 0.14 * shade, 0.085 * shade],
          );
        }
      }
    this.beam([x, y + h, z], [x, y + h + 1.2, z], 0.055, C.iron, MAT.metal);
  }
  gable(x, y, z, w, h, d, c = C.stone) {
    for (let yy = 0; yy < h; yy += 0.38) {
      const hh = Math.min(0.38, h - yy),
        ww = w * (1 - (yy + hh * 0.45) / h);
      for (const zz of [z - d / 2 + 0.15, z + d / 2 - 0.15])
        this.masonry(x, y + yy, zz, ww, hh, 0.38);
    }
    for (const side of [-1, 1]) {
      const dx = (-side * w) / 2,
        dy = h,
        len = Math.hypot(dx, dy),
        n = Math.ceil(len / 0.55);
      for (let i = 0; i < n; i++) {
        const t = (i + 0.5) / n;
        this.brick(
          x + (side * w) / 2 + dx * t,
          y + dy * t - 0.1,
          z + d / 2,
          len / n + 0.005,
          0.2,
          0.36,
          0,
          Math.atan2(dy, dx),
        );
      }
    }
  }
  roofGable(x, y, z, w, d, h) {
    const a = [x - w / 2, y, z + d / 2],
      b = [x - w / 2, y, z - d / 2],
      c = [x, y + h, z - d / 2],
      e = [x, y + h, z + d / 2],
      f = [x + w / 2, y, z - d / 2],
      j = [x + w / 2, y, z + d / 2];
    this.faces(
      [
        [b, a, e, c],
        [j, f, c, e],
      ],
      C.roof,
    );
    this.box(x, y - 0.13, z, w, 0.18, d, C.wood, MAT.bark);
    this.roofTiles(b, a, e, c, Math.ceil(h / 0.24));
    this.roofTiles(j, f, c, e, Math.ceil(h / 0.24));
    this.beam(e, c, 0.09, C.roof, MAT.stone);
  }
  curve(r, pointed = false, n = 32) {
    const q = [];
    for (let i = 0; i <= n; i++) {
      if (!pointed) {
        const a = Math.PI - (i * Math.PI) / n;
        q.push([r * Math.cos(a), r * Math.sin(a)]);
      } else if (i <= n / 2) {
        const a = Math.PI - ((i / (n / 2)) * Math.PI) / 3;
        q.push([r + 2 * r * Math.cos(a), 2 * r * Math.sin(a)]);
      } else {
        const a = Math.PI / 3 - (((i - n / 2) / (n / 2)) * Math.PI) / 3;
        q.push([-r + 2 * r * Math.cos(a), 2 * r * Math.sin(a)]);
      }
    }
    return q;
  }
  arch(x, y, z, r, leg, thick, depth, pointed = false, c = C.trim) {
    for (let yy = 0; yy < leg; yy += 0.4) {
      const hh = Math.min(0.4, leg - yy);
      for (const side of [-1, 1])
        this.brick(
          x + side * (r + thick / 2),
          y + yy,
          z,
          thick,
          hh - 0.015,
          depth,
        );
    }
    const q = this.curve(
      r + thick / 2,
      pointed,
      Math.max(12, Math.ceil(r * 10) * 2),
    );
    for (let i = 0; i < q.length - 1; i++) {
      const a = q[i],
        b = q[i + 1],
        dx = b[0] - a[0],
        dy = b[1] - a[1],
        len = Math.hypot(dx, dy);
      this.brick(
        x + (a[0] + b[0]) / 2,
        y + leg + (a[1] + b[1]) / 2 - thick / 2,
        z,
        len + 0.009,
        thick,
        depth,
        0,
        Math.atan2(dy, dx),
      );
    }
  }
  openingWall(x, y, z, w, h, d, r, leg, pointed = false) {
    const inside = (xx, yy, bw, bh) => {
      const qx = Math.max(0, Math.abs(xx - x) - bw / 2),
        cy = pointed
          ? Math.sqrt(Math.max(0, 4 * r * r - (qx + r) * (qx + r)))
          : Math.sqrt(Math.max(0, r * r - qx * qx));
      return qx < r && yy - bh / 2 < y + leg + cy;
    };
    for (let zz = z - d / 2 + 0.22; zz <= z + d / 2; zz += 0.38)
      this.masonry(x, y, zz, w, h, 0.44, inside);
  }
  window(x, y, z, w, h, pointed = true) {
    const r = w / 2,
      leg = h - r * (pointed ? Math.sqrt(3) : 1),
      q = this.curve(r, pointed),
      poly = [
        [x - r, y, z],
        [x + r, y, z],
        ...q
          .slice()
          .reverse()
          .map((v) => [x + v[0], y + leg + v[1], z]),
      ];
    this.faces([poly], C.glass);
    this.arch(x, y, z + 0.08, r, leg, 0.14, 0.2, pointed);
    this.box(x, y - 0.13, z + 0.12, w + 0.5, 0.16, 0.42, C.trim);
  }
  slit(x, y, z, w = 0.45, h = 1.3) {
    this.box(x, y, z, w, h, 0.06, C.iron);
    this.box(
      x - w / 2 - 0.09,
      y - 0.06,
      z + 0.03,
      0.15,
      h + 0.12,
      0.15,
      C.trim,
    );
    this.box(
      x + w / 2 + 0.09,
      y - 0.06,
      z + 0.03,
      0.15,
      h + 0.12,
      0.15,
      C.trim,
    );
    this.box(x, y + h, z + 0.03, w + 0.3, 0.14, 0.16, C.trim);
    this.box(x, y - 0.1, z + 0.06, w + 0.3, 0.12, 0.24, C.trim);
  }
  corbels(x, y, z, w, spacing = 1.3) {
    for (let xx = x - w / 2 + 0.5; xx < x + w / 2; xx += spacing) {
      this.box(xx, y, z, 0.45, 0.85, 0.7, C.trim);
      this.box(xx, y - 0.3, z - 0.13, 0.3, 0.4, 0.44, C.trim);
    }
    this.box(x, y + 0.85, z - 0.1, w, 0.26, 0.9, C.trim);
  }
  traceryRing(x, y, z, r, t = 0.07, n = 32) {
    for (let i = 0; i < n; i++) {
      const a = (i * 2 * Math.PI) / n,
        b = ((i + 1) * 2 * Math.PI) / n;
      this.beam(
        [x + r * Math.cos(a), y + r * Math.sin(a), z],
        [x + r * Math.cos(b), y + r * Math.sin(b), z],
        t,
        C.trim,
        MAT.stone,
      );
    }
  }
  finial(x, y, z, h = 1.5) {
    this.frustum(x, y, z, 0.22, 0.12, h * 0.45, C.trim, 8);
    this.frustum(x, y + h * 0.45, z, 0.28, 0, h * 0.55, C.trim, 8);
    this.beam(
      [x - 0.3, y + h * 0.7, z],
      [x + 0.3, y + h * 0.7, z],
      0.06,
      C.trim,
      MAT.stone,
    );
  }
}
