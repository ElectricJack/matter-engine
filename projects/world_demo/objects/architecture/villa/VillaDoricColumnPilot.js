import {emitVillaDoric} from 'shared-lib/villa_doric_pilot';

class VillaDoricColumnPilot extends Part {
  static params={quality:0,material:8};
  static lodBudgets=[1];
  static noImpostor=true;
  build(p) { emitVillaDoric(this,p); }
}
