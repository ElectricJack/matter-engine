import {emitWebsiteAsset} from 'shared-lib/villa_website_kit';
class VillaAmphora extends Part {
 static params={"quality":0,"stone":8,"plaster":8,"clay":8,"bronze":8,"soil":8,"leaf":8,"leafBack":8,"bark":8,"glaze":8,"darkstone":8,"marble":8};
 static lodBudgets=[1];
 static noImpostor=true;
 build(p){emitWebsiteAsset(this,'urn-small',p);}
}
