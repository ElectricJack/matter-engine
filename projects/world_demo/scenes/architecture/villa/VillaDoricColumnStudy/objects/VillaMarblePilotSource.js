import {emitVillaMarbleSource} from 'shared-lib/villa_marble_pilot';
class VillaMarblePilotSource extends Part {
  static lodBudgets=[1];
  static noImpostor=true;
  build() {emitVillaMarbleSource(this);}
}
