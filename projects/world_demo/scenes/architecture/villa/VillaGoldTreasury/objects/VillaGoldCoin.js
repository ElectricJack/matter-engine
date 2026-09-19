import {emitGold} from 'shared-lib/villa_gold_treasury';
class VillaGoldCoin extends Part {
 static params={gold:8};
 static lodBudgets=[1];
 static noImpostor=true;
 build(p){emitGold(this,'coin',p.gold);}
}
