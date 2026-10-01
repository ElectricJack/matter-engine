# Periodic brick material layout proof

Three twelve-triangle box receivers use an 8-column × 4-course material
definition at 1×, 2× and 4× wall counts. The physical repeat is 2.04 × 0.376 m;
finite wall extents subtract the final 10-mm head/bed joint. Brick scale stays
245 × 84 × 117.5 mm. Running-bond ends retain complete quarter-turned headers.

`moduleColumns` and `moduleCourses` define the repeat. Optional integer
`phaseColumns` and `phaseCourses` move appearance within that module; running
bond requires even course phase. Negative phases are supported. Wall dimensions,
phase and weathering do not enter the module's layout identity. Geometry and
finite-source composition consume the same physical placements.

This scene establishes the authored repeat and boundary treatments. Current VT
still composes receiver pages from the placed brick source bank; independent
periodic material-page lookup and sparse weathering are subsequent work.
Page-sharing counters must be measured rather than inferred from the module key.
