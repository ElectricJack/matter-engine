"""Gate comparison shots on settled native VT state, rather than UI frame count."""
import hashlib,json,re,shutil,subprocess,time
from pathlib import Path
repo=Path(__file__).resolve().parents[4];out=Path(__file__).resolve().parent
run=Path('/mnt/c/tmp/matter-mountain-pom-v3');assert not run.exists()
manifest=json.loads((out.parent/'2026-09-16-shared-vt-pixels/mountain-pom-v2-sources.json').read_text())
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert not [p for p,h in manifest.items() if sha(repo/p)!=h]
props=repo/'projects/world_demo/scenes/streaming/StreamMountain/props.json';original=props.read_bytes()
editor=repo/'MatterEditor/build/windows-msvc/editor.exe';binary=sha(editor)
timeline=out/'capture-v3-start.txt';timeline.write_text('wait_event bake.finished 900\nrender_path raster\ncam 380 20.25 1600 380 18.25 1593\n')
command='''Get-ChildItem Env:MATTER_* | Remove-Item; & py -3 MatterEngine3/tools/drive.py --world StreamMountain --timeline "D:/Shared With Desktop/AI/matter-engine-cpp/docs/agent/evidence/2026-09-17-mountain-pom/capture-v3-start.txt" --out-dir C:/tmp/matter-mountain-pom-v3 --editor "D:/Shared With Desktop/AI/matter-engine-cpp/MatterEditor/build/windows-msvc/editor.exe" --timeout 1000 --hide-ui --env MATTER_HIDE_WINDOW=1 --env MATTER_VK_VALIDATION=1 --env MATTER_WINDOW_WIDTH=1280 --env MATTER_WINDOW_HEIGHT=800 --env MATTER_PROFILE_LOG=1 --env MATTER_BAKE_TRACE=C:/tmp/matter-mountain-pom-v3/bake-trace.json; exit $LASTEXITCODE'''
start=time.monotonic();next_probe=start;probe=0;settled_since=None;last_count=None;released=False
names=['close-pom','close-flat','albedo-pom','albedo-flat','pom-status','grazing-pom','grazing-flat','grazing-native-rt']
commands='''set render.lighting.sun_elevation_deg 70
set render.lighting.sun_azimuth_deg 0
set render.lighting.sun_tint [1,1,1]
set render.lighting.sky_tint [1,1,1]
set render.lighting.sun_multiplier 1
set render.lighting.exposure_ev 0
set render.lighting.day_ambient_multiplier 0.5
set render.lighting.sky_irradiance_multiplier 0.7
set render.pom.enabled true
wait_frames 90
stats settled-close-pom
shot C:/tmp/matter-mountain-pom-v3/close-pom.png
set render.pom.enabled false
wait_frames 60
stats settled-close-flat
shot C:/tmp/matter-mountain-pom-v3/close-flat.png
set render.pom.enabled true
set viewer.debug.debug_view_mode 4
wait_frames 30
shot C:/tmp/matter-mountain-pom-v3/albedo-pom.png
set render.pom.enabled false
wait_frames 30
shot C:/tmp/matter-mountain-pom-v3/albedo-flat.png
set render.pom.enabled true
set render.pom.horizon_debug 7
wait_frames 30
shot C:/tmp/matter-mountain-pom-v3/pom-status.png
set render.pom.horizon_debug 0
set viewer.debug.debug_view_mode 0
cam 380 20.25 1600 400 20.25 1550
wait_idle 8 120
wait_frames 150
stats settled-grazing-pom
shot C:/tmp/matter-mountain-pom-v3/grazing-pom.png
set render.pom.enabled false
wait_frames 60
stats settled-grazing-flat
shot C:/tmp/matter-mountain-pom-v3/grazing-flat.png
set render.pom.enabled true
render_path native_rt
wait_frames 90
shot C:/tmp/matter-mountain-pom-v3/grazing-native-rt.png
quit
'''
(out/'capture-v3-shots.txt').write_text(commands)
with (out/'driver-v3.log').open('w') as driver:
    process=subprocess.Popen(['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-Command',command],cwd=repo,stdout=driver,stderr=subprocess.STDOUT)
    while process.poll() is None:
        now=time.monotonic()
        if not released and now>=next_probe and (run/'cmd.txt').exists():
            probe+=1;next_probe=now+5
            with (run/'cmd.txt').open('a') as fifo:fifo.write(f'stats readiness-{probe}\n')
        text=(run/'log.txt').read_text(errors='replace') if (run/'log.txt').exists() else ''
        matches=re.findall(r'STATSVT,readiness-\d+,active=(\d+),variants=(\d+)/[^\n]*?queue=(\d+),',text)
        if matches and not released:
            active,count,queue=map(int,matches[-1]);state=(len(matches),count)
            if state!=last_count:
                if active and count>100 and queue==0 and last_count and last_count[1]==count:
                    if settled_since is None:settled_since=now
                else:settled_since=None
                last_count=state
            if settled_since is not None and now-settled_since>=15:
                released=True
                print(f'Settled after {now-start:.1f}s: {count} variants, queue=0; recording comparisons',flush=True)
                with (run/'cmd.txt').open('a') as fifo:fifo.write(commands)
            elif now-start>700:
                released=True
                with (run/'cmd.txt').open('a') as fifo:fifo.write('quit\n')
        time.sleep(1)
    code=process.wait()
(out/'saved-props-v3.json').write_bytes(props.read_bytes());props.write_bytes(original)
dest=out/'v3';shutil.copytree(run,dest)
text=(dest/'log.txt').read_text(errors='replace')
audit={'editor_exit':code,'seconds':time.monotonic()-start,'editor_sha256':binary,'binary_unchanged':sha(editor)==binary,
       'source_changes':[p for p,h in manifest.items() if sha(repo/p)!=h],
       'missing_shots':[name for name in names if not (dest/(name+'.png')).is_file() or not (dest/(name+'.png.done')).is_file()],
       'validation_errors':[line for line in text.splitlines() if 'Validation Error' in line],
       'command_failures':[line for line in text.splitlines() if re.search(r'unknown prop|set:.*(?:invalid|failed|unknown)|render_path:.*unavailable',line)],
       'stats':[line for line in text.splitlines() if line.startswith(('STATS,','STATSVT,'))],
       'isolation':'Frozen r2 editor PID 9056 was independently open; functional/visual evidence only.',
       'lighting':'Temporary neutral diagnostic lighting; original scene properties restored.',
       'files':{p.name:sha(p) for p in dest.iterdir() if p.is_file()}}
(dest/'audit.json').write_text(json.dumps(audit,indent=2)+'\n')
print(json.dumps({k:v for k,v in audit.items() if k not in ('files','stats')},indent=2),flush=True)
raise SystemExit(0 if code==0 and audit['binary_unchanged'] and not any(audit[x] for x in ['source_changes','missing_shots','validation_errors','command_failures']) else 1)
