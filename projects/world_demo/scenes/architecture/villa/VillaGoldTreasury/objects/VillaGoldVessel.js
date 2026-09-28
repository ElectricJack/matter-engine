import {emitGold} from 'shared-lib/villa_gold_treasury';
class VillaGoldVessel extends Part {
 static params={gold:8};
 static lodBudgets=[1];
 static noImpostor=true;
 build(p){emitGold(this,'vessel',p.gold);}
}
