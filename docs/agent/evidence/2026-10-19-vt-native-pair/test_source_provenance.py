"""Rerun the existing Linux diagnostics into this attempt's evidence directory."""
import importlib.util
from pathlib import Path

OUT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    'historical_vt_provenance', OUT.parent / '2026-10-14-vt-provenance/test_provenance.py')
provenance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provenance)
provenance.OUT = OUT

test_historical_producer_and_vt_sources = provenance.test_historical_producer_and_vt_sources
test_compiled_historical_budget_bound = provenance.test_compiled_historical_budget_bound
test_existing_vt_residency_suite = provenance.test_existing_vt_residency_suite
