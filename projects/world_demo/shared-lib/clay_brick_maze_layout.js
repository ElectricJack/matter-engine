// Coordinates in metres; wall dimensions remain integer counts of real bricks.
// Four gates lead into a branching court with staggered corners and two bends.
export function clayBrickMazeWalls() {
  const walls=[];
  const add=(id,x,z,turn,params)=>{const a=turn*Math.PI/2,c=Math.round(Math.cos(a)),s=Math.round(Math.sin(a));
    walls.push({id,module:params.kind?'ClayBrickWallPath':'ClayBrickWallSurface',params,
      transform:[c,0,s,x,0,1,0,0,-s,0,c,z,0,0,0,1]});};
  add('outer-sw',0,0,0,{kind:'corner',columns:32,returnColumns:32,courses:24});
  add('outer-nw',0,18.6,1,{kind:'corner',columns:32,returnColumns:32,courses:24});
  add('outer-ne',18.6,18.6,2,{kind:'corner',columns:32,returnColumns:32,courses:24});
  add('outer-se',18.6,0,3,{kind:'corner',columns:32,returnColumns:32,courses:24});
  add('south-court',4.2,3.2,0,{kind:'u',columns:40,returnColumns:16,courses:16});
  add('north-court',14.39,15.4,2,{kind:'u',columns:40,returnColumns:16,courses:16});
  add('south-switchback',6,5,0,{kind:'corner',columns:20,returnColumns:10,courses:10});
  add('north-switchback',12.6,13.6,2,{kind:'corner',columns:20,returnColumns:10,courses:10});
  add('broad-bend',5,8.4,0,{kind:'curve',columns:20,centerJointM:.015,courses:16});
  add('tight-bend',11.8,9.4,2,{kind:'curve',columns:14,centerJointM:.020,courses:24});
  add('west-lower',1.7,2,0,{kind:'corner',columns:5,returnColumns:20,courses:10});
  add('west-upper',1.7,16.6,1,{kind:'corner',columns:20,returnColumns:5,courses:16});
  add('east-lower',16.9,2,3,{kind:'corner',columns:20,returnColumns:5,courses:16});
  add('east-upper',16.9,16.6,2,{kind:'corner',columns:5,returnColumns:20,courses:10});
  add('south-gate-left',3.7,1.5,0,{columns:16,courses:10});
  add('south-gate-right',10.7,1.5,0,{columns:16,courses:10});
  add('north-gate-left',3.7,17.1,0,{columns:16,courses:10});
  add('north-gate-right',10.7,17.1,0,{columns:16,courses:10});
  add('west-route-divider',1.8,8.3,0,{columns:9,courses:24});
  add('east-route-divider',14.5,10.3,0,{columns:9,courses:24});
  return walls;
}
