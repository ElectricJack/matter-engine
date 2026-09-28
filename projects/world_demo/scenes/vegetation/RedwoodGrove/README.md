# RedwoodGrove

Select **RedwoodGrove** in the editor. The scene contains a 62 m coast redwood
and a clump of three roughly 48 m stems sharing one root crown. Needles and
cones have life-size samples at `(0, 1.5, 12)` and `(0.3, 1.5, 12)`.

The reusable generator is [`conifer.js`](../../../shared-lib/conifer.js), with
`species: 2`. Redwood uses two-ranked 18 mm leaves on spreading shoots,
4 mm scale leaves on upright upper-crown shoots, and small 25 mm hanging cones with
25 woody scales. Branch attachments are staggered vertically, rather than
forming strict fir whorls. The same six bough prototypes serve every stem.

[`conifer_clump.js`](../../../shared-lib/conifer_clump.js) defines `ConiferClump`.
Use it as an expanded world root, just like `ConiferTree`:

```js
{ module: 'ConiferClump', expand: true, params: {
  species: 2, seed: 91, height: 48, dbh: 1.5, age: 140,
  stemCount: 3, stemSpread: 1.15, heightVariation: 0.16, lean: 0.025,
  crownRadius: 5.2, crownRatio: 0.72,
  whorlCount: 42, branchesPerWhorl: 4, fullness: 1.25,
  barkMaterial, branchMaterial, needleMaterial, coneMaterial,
} }
```

`stemCount` supports 2–7 stems. `stemSpread` is their attachment radius in
metres. `heightVariation` is a fractional height spread; DBH and crown size
follow each stem's height factor. Lean is oriented outward. Seeds differ per
stem while `branchSeed` stays shared. `ConiferRootCrown` unions the basal
connections and radial roots into one isosurface, with strip approximations
at 12 and 40 m. The clump option also works with pine and fir as an artistic
growth form; root-crown sprouting is specifically characteristic of redwood.

Height and DBH controls allow redwoods up to 100 m and 6 m respectively.
These are independent authoring bounds, not a fitted biological growth model.
`whorlCount` means retained branch tiers for redwood, not annual growth rings.
`treePlan(params).counts` reports exact generated counts; they remain structural
estimates rather than measurements of a particular wild tree.

Redwood trunk bark uses the long, deep reddish furrows of `RedwoodBarkDetail`.
Branches use the finer `ConiferBranchDetail`. See the
[main generator documentation](../ConiferLab/README.md) for texture generation,
needle baking, wood representations and validation commands.

References:

- [Oregon State: coast redwood](https://landscapeplants.oregonstate.edu/plants/sequoia-sempervirens): scale, fibrous bark, two leaf forms and cone dimensions.
- [US Forest Service: redwood silvics](https://www.srs.fs.usda.gov/pubs/misc/ag_654/volume_1/sequoia/sempervirens.htm): root-crown sprouts and multiple stems.
- [Gymnosperm Database: Sequoia](https://www.conifers.org/~conifers/cu/Sequoia.php): buttressing, branch habit and cone scales.
