import { copyJson, fields, freeze, finite, integer, need, compareKeys } from './values.mjs';

export function parameters(definitions, overrides = {}) {
  definitions = copyJson(definitions); overrides = copyJson(overrides);
  fields(definitions); fields(overrides, Object.keys(definitions), []);
  const result = {};
  for (const key of Object.keys(definitions).sort(compareKeys)) {
    const spec = definitions[key];
    fields(spec, ['type', 'default', 'minimum', 'maximum', 'values'], ['type', 'default']);
    need(['number', 'integer', 'boolean', 'string', 'enum'].includes(spec.type), `parameter ${key} has unsupported type`);
    need(spec.minimum === undefined || ['number', 'integer'].includes(spec.type), `parameter ${key} has a nonnumeric minimum`);
    need(spec.maximum === undefined || ['number', 'integer'].includes(spec.type), `parameter ${key} has a nonnumeric maximum`);
    const min = spec.minimum === undefined ? -Number.MAX_VALUE : finite(spec.minimum);
    const max = spec.maximum === undefined ? Number.MAX_VALUE : finite(spec.maximum);
    need(min <= max, `parameter ${key} has reversed bounds`);
    function validate(value) {
      if (spec.type === 'number' || spec.type === 'integer') {
        finite(value, `parameter ${key}`);
        if (spec.type === 'integer') integer(value, -Number.MAX_SAFE_INTEGER, Number.MAX_SAFE_INTEGER, `parameter ${key}`);
        need(value >= min && value <= max, `parameter ${key} is outside [${min},${max}]`);
      } else if (spec.type === 'enum') {
        need(Array.isArray(spec.values) && spec.values.length > 0 && spec.values.every(v => ['string', 'number', 'boolean'].includes(typeof v)), `parameter ${key} requires scalar enum values`);
        need(spec.values.includes(value), `parameter ${key} is not an allowed enum value`);
      } else need(typeof value === spec.type, `parameter ${key} must be ${spec.type}`);
      return value;
    }
    validate(spec.default);
    Object.defineProperty(result, key, { value: validate(Object.hasOwn(overrides, key) ? overrides[key] : spec.default), enumerable: true });
  }
  return freeze(result);
}
