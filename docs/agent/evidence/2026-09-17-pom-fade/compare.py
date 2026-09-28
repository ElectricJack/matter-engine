"""Matched settled StreamMountain captures for the POM fade-check reordering."""
import hashlib,json,re,shutil,statistics,subprocess,sys,time
from pathlib import Path
repo=Path(__file__).resolve().parents[4];out=Path(__file__).resolve().parent
label,prefix=sys.argv[1:3];assert label in ('before','after')
run=Path(f'/mnt/c/tmp/matter-pom-work-{label}');assert not run.exists()
manifest=json.loads((out.parent/'2026-09-16-shared-vt-pixels'/f'{prefix}-sources.json').read_text())
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert not [p for p,h in manifest.items() if sha(repo/p)!=h]
props=repo/'projects/world_demo/scenes/streaming/StreamMountain/props.json';original=props.read_bytes()
editor=Path('/mnt/c/tmp/matter-pom-baseline/editor.exe') if label=='before' else repo/'MatterEditor/build/windows-msvc/editor.exe'
binary=sha(editor)
win_editor='C:/tmp/matter-pom-baseline/editor.exe' if label=='before' else 'D:/Shared With Desktop/AI/matter-engine-cpp/MatterEditor/build/windows-msvc/editor.exe'
timeline=out/f'{label}-start.txt';timeline.write_text('wait_event bake.finished 900\n')
command=f'''Get-ChildItem Env:MATTER_* | Remove-Item; & py -3 MatterEngine3/tools/drive.py --world StreamMountain --timeline "D:/Shared With Desktop/AI/matter-engine-cpp/docs/agent/evidence/2026-09-17-pom-fade/{label}-start.txt" --out-dir C:/tmp/matter-pom-work-{label} --editor "{win_editor}" --timeout 1000 --hide-ui --env MATTER_HIDE_WINDOW=1 --env MATTER_VK_VALIDATION=1 --env MATTER_WINDOW_WIDTH=1280 --env MATTER_WINDOW_HEIGHT=800 --env MATTER_PROFILE_LOG=1 --env MATTER_BAKE_TRACE=C:/tmp/matter-pom-work-{label}/bake-trace.json; exit $LASTEXITCODE'''
start=time.monotonic();next_probe=start;probe=0;stable_since=None;last_state=None;phase='activation';cutoff=0
names=[f'{view}-{mode}' for view in ('overview','grazing') for mode in ('lit','albedo','normal')]
commands=[]
def send(value):
    commands.append(value)
    with (run/'cmd.txt').open('a') as fifo:fifo.write(value+'\n')
    (out/f'{label}-commands.txt').write_text('\n'.join(commands)+'\n')
def shots(view):
    lines=['set viewer.debug.debug_view_mode 0','wait_frames 90']
    for i in range(30):lines += ['wait_frames 5',f'stats {view}-sample-{i:02d}']
    lines += [f'shot C:/tmp/matter-pom-work-{label}/{view}-lit.png','set viewer.debug.debug_view_mode 4',
              'wait_frames 30',f'shot C:/tmp/matter-pom-work-{label}/{view}-albedo.png',
              'set viewer.debug.debug_view_mode 1','wait_frames 30',f'shot C:/tmp/matter-pom-work-{label}/{view}-normal.png',
              'set viewer.debug.debug_view_mode 0']
    return '\n'.join(lines)
with (out/f'{label}-driver.log').open('w') as driver:
    process=subprocess.Popen(['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-Command',command],cwd=repo,stdout=driver,stderr=subprocess.STDOUT)
    while process.poll() is None:
        now=time.monotonic()
        if phase in ('activation','overview-wait','grazing-wait') and now>=next_probe and (run/'cmd.txt').exists():
            probe+=1;next_probe=now+5;send(f'stats ready-{probe}')
        text=(run/'log.txt').read_text(errors='replace') if (run/'log.txt').exists() else ''
        matches=re.findall(r'STATSVT,ready-\d+,active=(\d+),variants=(\d+)/[^\n]*?queue=(\d+),',text)
        if phase=='activation' and matches and int(matches[-1][0]):
            # World activation has already applied its authored camera. Reapply
            # all comparison settings only now, so the camera cannot be reset.
            send('''render_path raster
cam 380 90 1600 420 55 1420
set render.lighting.sun_elevation_deg 70
set render.lighting.sun_azimuth_deg 0
set render.lighting.sun_tint [1,1,1]
set render.lighting.sky_tint [1,1,1]
set render.lighting.sun_multiplier 1
set render.lighting.exposure_ev 0
set render.lighting.day_ambient_multiplier 0.5
set render.lighting.sky_irradiance_multiplier 0.7
set render.pom.enabled true
wait_frames 90''')
            phase='overview-wait';cutoff=len(matches);stable_since=None;last_state=None
        if phase in ('overview-wait','grazing-wait') and len(matches)>cutoff:
            active,count,queue=map(int,matches[-1]);state=(len(matches),count)
            if state!=last_state:
                if active and count>100 and queue==0 and last_state and last_state[1]==count:
                    if stable_since is None:stable_since=now
                else:stable_since=None
                last_state=state
            if stable_since is not None and now-stable_since>=15:
                view=phase.split('-')[0];send(shots(view));phase=view+'-shots'
                print(f'{label}: {view} settled after {now-start:.1f}s, variants={count}, queue=0',flush=True)
        if phase=='overview-shots' and (run/'overview-normal.png.done').is_file():
            send('cam 380 20.25 1600 400 20.25 1550\nwait_frames 90')
            phase='grazing-wait';cutoff=len(matches);stable_since=None;last_state=None;next_probe=now+5
        if phase=='grazing-shots' and (run/'grazing-normal.png.done').is_file():send('quit');phase='shutdown'
        if now-start>850 and phase!='shutdown':send('quit');phase='shutdown'
        time.sleep(1)
    code=process.wait()
(out/f'{label}-saved-props.json').write_bytes(props.read_bytes());props.write_bytes(original)
dest=out/label;assert not dest.exists();shutil.copytree(run,dest)
text=(dest/'log.txt').read_text(errors='replace')
audit={'editor_exit':code,'seconds':time.monotonic()-start,'editor_sha256':binary,'binary_unchanged':sha(editor)==binary,
       'source_changes':[p for p,h in manifest.items() if sha(repo/p)!=h],
       'missing_shots':[n for n in names if not (dest/(n+'.png')).is_file() or not (dest/(n+'.png.done')).is_file()],
       'validation_errors':[line for line in text.splitlines() if 'Validation Error' in line],
       'command_failures':[line for line in text.splitlines() if re.search(r'unknown prop|set:.*(?:invalid|failed|unknown)|render_path:.*unavailable',line)],
       'stats':[line for line in text.splitlines() if line.startswith(('STATS,','STATSVT,'))],
       'isolation':'Frozen r2 editor PID 9056 independently open; matched development comparison, not isolated acceptance.',
       'files':{p.name:sha(p) for p in dest.iterdir() if p.is_file()},'timings':{}}
for view in ('overview','grazing'):
    rows=[line.split(',') for line in text.splitlines() if line.startswith(f'STATS,{view}-sample-')]
    values=[float(row[-1]) for row in rows]
    audit['timings'][view]={'samples':len(values),'gbuffer_median_ms':statistics.median(values) if values else None,
                            'gbuffer_p95_ms':sorted(values)[int(.95*(len(values)-1))] if values else None}
(dest/'audit.json').write_text(json.dumps(audit,indent=2)+'\n')
print(json.dumps({k:v for k,v in audit.items() if k not in ('files','stats')},indent=2),flush=True)
raise SystemExit(0 if code==0 and audit['binary_unchanged'] and not any(audit[x] for x in ('source_changes','missing_shots','validation_errors','command_failures')) else 1)
