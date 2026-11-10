# TODOs

Features to implement in the future. Generally try to implement them with
low-cost solutions. We want the game to be playable on low-end CPUs too.

### Graphics/immersion

- [ ] More realistic snow/mud/ground and finer particles that react to speed
  (density/height/profile reaction) and fall with gravity.
- [x] Road texture looks too blocky and fake, it shoul be more realistic and a
  bit smoother but still detailed some.
  - Done: mipmaps made for every texture, trilinear + 8x anisotropic
    filtering (`NFS_ANISOTROPY`, `NFS_TEXTURE_FILTER`), and a fine tiling grain
    on the road and ground that fades with distance and stays off the cars
    (`NFS_ROAD_DETAIL`).
- [x] Weather e.g. rain should affect road texture.
  - Done: in the rain (not on snow tracks) the road is darker, puddled and
    reflects the last frame -- mirrored about the horizon, streaked under
    lights, strongest at grazing angles, the HUD kept out (`NFS_WET_ROADS`).
  - [ ] Next: splashes/ripples where rain hits puddles; drier line where cars
    drive; wet look on the cars' paint.
- [ ] Add natural shadows given a light position, both for cars and environment.
  - Partly done: each car's shadow is stretched away from the track's sun,
    turning as the car turns, and its edges are soft (`NFS_SHADOW_STRETCH`,
    `NFS_SOFT_EFFECTS`). Real shadows (shadow maps) are too costly here.
  - Done: shadows fainter in the rain/overcast and at night
    (`NFS_SHADOW_RAIN`, 0.55; `NFS_SHADOW_NIGHT`, 0.4).
  - [ ] Next: shadows for trackside objects.
- [x] More details environment (buildings, grass, fences, rocks, etc.) textures,
  better sampled maybe?
  - Done: textures seen larger than they are drawn Catmull-Rom instead of
    bilinear (`NFS_TEXTURE_SHARP`), and a fine grain on the rest of the world's
    opaque surfaces (`NFS_OBJECT_DETAIL`, 0.3).
  - [ ] Next: ground seen at a grazing angle is still the GPU's anisotropic
    blur; real detail there needs higher-resolution track textures.
- [x] More detailed car models/less blocky-looking in races and better paint
  properties. Maybe like this: Car paint: tag car-body triangles in the native car drawing code. Then add an angle-based shine and a sharper highlight curve on top of the game’s existing reflection map.
  - Done (paint): each reflecting vertex's facing, from the reflection's own
    view-space normals, rides with the vertex; the reflection pass is shaped
    by it -- weaker face-on, stronger at the edges, sharper highlights
    (`NFS_CAR_SHINE`).
  - [ ] Next (models): the meshes are the game's own; less blocky needs new
    or subdivided models (smooth-normal subdivision of car.fce at load).
- [x] Bug: at night the headlights' pool on the road had a white noise over
  it. Done: the world's grain (`NFS_OBJECT_DETAIL`) is kept off additive draws.
- [ ] Whole-image effects in the final blit: 2–4× anti-aliasing, a light bloom on headlights and sun, and a per-track colour grade. Each is cheap on desktop and can be turned off for weaker machines.
- [ ] Haze: fog worked out per pixel with a sky-coloured gradient instead of a flat fog colour gives more depth for almost nothing.

### Races/locations

- [x] Double the distance that headlights light up when on. Add smoother light,
  more realistic than before light that lights environment more naturally.
  - Done: the headlights' patch reaches twice as far and is 1.4x as wide
    (`NFS_HEADLIGHT_REACH`, `NFS_HEADLIGHT_WIDTH`), and fades softly at its
    sides and far end instead of ending in a box (`NFS_SOFT_EFFECTS`).
  - [ ] Next: light on trackside objects (fences, walls) from the headlights.
- [ ] Emissive light from environmental models that emit light. Note that this
  is partially implementned by the original game but check if it can improved

### Oppponents and AI
- [x] Bug: a quick race against Class A opponents had police cars (and cars
  of other classes) in it.
  - Done: the port's copy of the game's car cache was never emptied when the
    car list was re-sorted (`sub_4399f0`), so each picked opponent was raced
    as the car that had stood at its place before. Now emptied with it.

- [x] Research how the opponent AI works and what the AI features/decisions are
  and how they're made.
  - Starting points found: the opponents' driving is already decompiled in
    `src/nfs3hp/native_ai.h` -- `aiPull` (`sub_4064f0`, speed/pull, likely
    where the rubber band is) and `aiHeading` (`sub_4070c0`, steering) --
    switched in by `native_ai.cpp` (`NFS_NATIVE_AI=0` for the original) and
    checked bit-exact by `tools/native_ai_checks.py`. Per-opponent fields the
    `[BOTFORCE]` log in `nfs3hp_main.cpp` prints: car `+0x1f0`, `+0x1fc`,
    `+0x7b0`, `+0x7c4`; globals `0x6fd50c`, `0x6fd4f0`, `0x6fd518`,
    `0x6fd508`. Per-car personalities and harder difficulties most likely
    start as per-opponent changes to `aiPull`'s targets.
  - Done (how they drive, what goes wrong and why): see TECHNICAL.md, "The
    opponents' driving, tuned".
- [x] Currently all opponents have the same 'rubberband' AI. Each opponent
  should be given a slightly differnet AI given his vehicle (i.e. finer
  steering, more prone to accelerating if a fast vehicle, some occcasionally try
  to ram player to stay in corner, some drive smoothly and some use brakes more
  - Done: each driver (by racer slot, whatever car it drives) has a character:
    pull, corner speed, patience when stuck, how long it backs off -- hothead,
    smooth, late braker, steady, bully, cautious, opportunist.
  - [ ] Next: ramming the player and braking styles need the steering/throttle
    code (`sub_4076b0`) decompiled first.
- [x] Each difficulty should be a notch harder than what it currently is.
  - Done: opponents pull 6% harder and aim 3% faster (`NFS_AI_PACE`), on top
    of each driver's own character.
- [x] Some blatant AI weaknesses/bugs I noticed: often many opponents crash
  together and clump and get stuck. Sometimes when there's a splitting turn with
  a wall in between they don't know where to turn and crash on the wall. I
  noticed it in rainy locations, idk if it's the case everywhere. Sometimes they
  don't know how to react if they turn by 90 or 180 degrees and don't know how
  to continue it seems.
  - Done: a stuck opponent steers for another line (away from the car ahead,
    or the other half of the road at a fork's wall), then reverses on a timer
    collisions cannot cancel, and after three tries is put back on the road at
    once instead of after half a minute; clumps come apart (`NFS_AI_TUNE`).
  - [x] Next: forks are only handled once a car has hit the wall; choosing the
    branch ahead of time needs the track's fork data read.
    - Done: each track node's word at +0xc is the road across it in slices; a
      gap in it is a split (fork, island, pillar). Opponents look ~2.5 s
      ahead and, if their line runs into a gap, keep to the nearer way round
      until past it. A car that does hit something now steps a few metres
      aside (twice) before reversing, instead of crossing to the far edge.
    - Also fixed: the "car ahead/behind" check read the wrong field (+0x130,
      a direction, not the position, +0x98) and saw nothing real.
- [x] Opponent signaling: opponents currently only hit horn. when an opponent is stuck behind player (not other
  opponents) for some time (like 2 sec) slipstreaming player, he aggressively
  switches headlines between normal and full scale a view times before going
  back to default headlight scale. Then waits a few more sec (3 sec) and if
  still behind player, repeat.
  - Done: 2 s held up right behind the player (3-16 m, same lane, wanting to
    go faster, no other car in between) -> four quick flashes to full beam
    (+0x660) and back; 3 s later, still there, again. Not in network races.

### UI
- [ ] Figure out how to add/create menu elements without breaking the visual
  style, make them seamless with the style of the game. Can you find any menu
  assets?
- [ ] Currently the sides of screen are padded with black as the game was made
  for narrower resolution. Fill the whole screen all the time (menus: expand
  main screen, move fields towards the edges, rage: expand the main screen/view)
  without however stetching/scaling things sideways.
- [ ] Add those added graphics enhancements in graphics menu in game (e.g. as
  toggleable)
- [ ] Add difficulty slider instead of just normal and agressive (presets in
  slider will still be normal and agressive but we can slide in between to
  adjust opponent AI)

### Driving
- [ ] Nitro and nitro UI. However if you add nitro, opponents also need to know
  how to use it (e.g. to prevent them from staying inside the turn or blast it
  when they see enough straight road ahead, however they need to take crashes
  into account too). Note that if you nitro, it should be right of the speed
  meter as a vertical bar if gauge is selected to be on the top part of screen,
  else as an outer circle around it.

### Music
- [ ] In a brief popup in whosse visual style is consistent with game, show
  cyrretnly plating song when it starts and then slide the popup away.
- [ ] Music control: what tracks play in game, skip current track button.
- [ ] Radio: be able to add your own tracks.

### Career mode
- [ ] Add one more game mode other than single player, hot pursuit, etc called
  Career. There we race, unlock cars, earn money, get body damage on car, fix
  car, etc.

### Performance
- [ ] Races take quite long to load by defaul. Find out if you and reduce
  loading times by caching anything etc.
