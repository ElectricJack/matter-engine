"""Test-only structural schema gate; requires Python jsonschema, not an engine."""
import argparse
import json
from pathlib import Path
import subprocess
from jsonschema import Draft202012Validator

parser = argparse.ArgumentParser()
parser.add_argument('--vectors', required=True, help='Built native vector executable')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
schema = json.loads((root / 'schema-v1.json').read_text(encoding='utf-8'))
Draft202012Validator.check_schema(schema)
validator = Draft202012Validator(schema)
vectors = json.loads(subprocess.check_output([args.vectors], text=True))
for kind in ('recipe', 'asset'):
    document = json.loads(vectors[kind + '_wire'])
    validator.validate(document)
    validator.validate(json.loads((root / 'examples' / (
        'ordered-csg.recipe.json' if kind == 'recipe' else 'repeated-assembly.asset.json'
    )).read_text(encoding='utf-8')))
    document['unversioned_field'] = True
    assert not validator.is_valid(document), 'unknown fields must fail'
validator.validate(json.loads(vectors['optional_wire']))
bad_matrix = json.loads(vectors['asset_wire'])
bad_matrix['instances'][0]['local_transform'].pop()
assert not validator.is_valid(bad_matrix), 'matrix must have sixteen values'
print('ALL PASS: Draft 2020-12 schema, native recipe/asset/typed products and published examples; unknown fields rejected')
