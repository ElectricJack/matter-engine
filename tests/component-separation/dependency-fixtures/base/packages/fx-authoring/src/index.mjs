// Fixture: portable authoring code; relative imports inside the package only.
import { scale } from "./scale.mjs";

export function authorBox(size) {
  return { kind: "box", size: scale(size, 2) };
}
