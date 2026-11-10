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
  - [ ] Next: shadows for trackside objects; shadow strength by time of day
    (fainter in rain/overcast and at night).

### Races/locations

- [x] Double the distance that headlights light up when on. Add smoother light,
  more realistic than before light that lights environment more naturally.
  - Done: the headlights' patch reaches twice as far and is 1.4x as wide
    (`NFS_HEADLIGHT_REACH`, `NFS_HEADLIGHT_WIDTH`), and fades softly at its
    sides and far end instead of ending in a box (`NFS_SOFT_EFFECTS`).
  - [ ] Next: light on trackside objects (fences, walls) from the headlights.

### Oppponents and AI

- [ ] Research how the opponent AI works and what the AI features/decisions are
  and how they're made.
- [ ] Currently all opponents have the same 'rubberband' AI. Each opponent
  should be given a slightly differnet AI given his vehicle (i.e. finer
  steering, more prone to accelerating if a fast vehicle, some occcasionally try
  to ram player to stay in corner, some drive smoothly and some use brakes more
- [ ] Each difficulty should be a notch harder than what it currently is.
