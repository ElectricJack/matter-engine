"""Independent component dependency and ownership checks (clear-crest-48.7.2).

The package is driven by ``tools/component-separation/check_dependencies.py``.
Every check reads one declarative rules file (``rules.json`` for this
repository; the fixtures under ``tests/component-separation/dependency-fixtures``
carry their own) so the same code audits the real tree and the deliberately
broken fixture trees.
"""
