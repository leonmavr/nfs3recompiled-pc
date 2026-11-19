# TODOs

Features to implement in the future. Generally try to implement them with
low-cost solutions. We want the game to be playable on low-end CPUs too.

### Graphics/immersion

- [x] More realistic snow/mud/ground and finer particles that react to speed
  (density/height/profile reaction) and fall with gravity. I tihnk currently the
  game uses static images for it. If you find a cheap procudural approach that
  makes them look organic (e.g. not just round blobs), use this. More realistic and
  dust particles. 
  - Done (`NFS_PARTICLES`): the dirt thrown off the road is the port's own --
    small ragged grains thrown up and falling back under gravity, and soft
    uneven dust clouds -- more of them and higher the faster, coloured like
    the ground they came from (sand, snow, mud).
- [x] Rain on the windscreen/camera: drops and streaks that clear at speed.
  Independent of the game's own particles that landed on the view (cleared
  every frame, see The game's own sprites are not drawn): an effect of its own
  on top of the rendering, in the whole-picture pass, and cheap -- a few
  procedural drops refracting the picture behind them, sliding and streaking
  off with speed, none under a roof (the sky test the weather uses).
  - Done (2026-11-17, `NFS_LENS_RAIN`): two grids of procedural drops in the
    composite, magnifying what is behind them, streaking off at speed, the
    lens wetting and drying with the sky test (Rain on the lens).
- [x] Sun glare by day with no weather: a haze and lens ghosts round the sun,
  dimmed behind what stands in front of it (`NFS_SUN_GLARE`).
- [x] The weather's own flakes and drops drawn as blocky, translucent squares.
  - Done (`NFS_WEATHER_PARTICLES`, on): kinds 7 and 8 are the port's own now --
    a raindrop a short bright streak, a snowflake a small soft speck, falling as
    the game threw them, rain or snow by the track's weather. The game's
    pictures had no alpha and were cut out by nothing, so a flake near the lens
    was a box of dimmed picture over the view. `NFS_SPRITE_TRACE=1` prints what
    every kind of draw is cut out by, for the other squares.
- [x] Wheel spray in the rain: mist behind every car on wet roads.
  - Done (`NFS_WHEEL_SPRAY`, on): a pale mist that billows out behind the
    wheel and hangs a moment, and fine drops thrown up, both more the faster.
    It was off while the game's own sprites still showed as turned squares;
    the game's particle sprites are not drawn any more, so the port's spray is
    what a wet road throws.
- [x] Frost and snow spray off tyres on snow: sharp ice chips and a lot of
  light spray, white/cyan/light grey, lighter and spreading more than dirt.
  - Done (`NFS_SNOW_SPRAY`, on): every wheel on snow over 6 m/s throws small
    faceted ice chips (`EffectParticle::Frost`, three to six flat sides,
    tumbling, gone where they land) and many fine light grains plus now and
    then a soft cloud -- their own white, cyan and light grey, never the
    ground's. The game's own dust on snow is taken over as frost too
    (`throwFrost`, `frostFromWheels`).
  - Denser (2026-11-19): four times the grains a burst and twice the chips and
    clouds, the air's frost kept under 6000 of a pool grown to 9600 so the
    smoke and the weather keep their room.
- [x] The turned squares over the view in the rain and snow (the game's own
  particle sprites).
  - Done (`NFS_GAME_SPRITES`, off): nothing the game draws of its particle set
    is drawn any more -- neither the pool's quads, which its pictures (565,
    no alpha) and the pool's own spin turned into squares of dimmed picture,
    nor the drops that land on the camera, which were turned rectangles of
    blurred view (the dark rectangles in the screenshot of 2026-11-17).
    Everything the port draws itself (dust, grass, spray, smoke, exhaust
    flames, weather) is unaffected; the game's are still drawn where it leaves
    a kind to the game (its particles off, a switch off, split screen), and
    `1`/`on` brings them back everywhere for a comparison.
  - Why it took four tries: `particlesNative` and `dropsNative` asked for the
    race's own single precision (`singlePrecision`), and a race's FPU is
    extended by default, so the generated code did the drawing in every race
    and none of the fixes in the native versions ever ran. They now draw under
    `drawingPrecision` (drawing only, no simulation state) -- worth remembering
    for anything else that only draws.
- [x] Turned boxes round the spray and the snowflakes/drops, moving, in rain
  and snow (2026-11-17, after the fixes above).
  - Found: not a picture at all but a hole. A faint blended square of the
    world's wrote its whole square into the depth buffer (only fragments under
    alpha 0.08 were kept out), nearer than what was behind it, and the port's
    spray and weather -- depth-tested against that buffer -- were cut away in
    its shape. With the particles drawn untested the boxes went; with a higher
    cut for the world's batches alone they went too, and not with it for the
    cars' alone. A world batch now writes depth only where it is at least half
    opaque (`kDepthCutWorld`, `thrashrenderer.cpp`); the cars' keep 0.08.
- [x] A turned black rectangle over the road in some races (2026-11-17).
  - Found: the port's own car-shadow mask, a **rounded square** -- `shad`'s
    picture is opaque to within three texels of its edge, and a squircle of it
    is still a square the size of a car, so a car near the camera cast a turned
    translucent rectangle on the road instead of a shadow. Its alpha is a soft
    ellipse now, and `NFS_CAR_SHADOW=0` leaves the cars with no shadow at all.
    (`NFS_SPRITE_TRACE` + `NFS_TEXTURE_DUMP=/tmp/tex` trace every sprite the
    game draws, with its box and its picture -- the tools that found it.)
- [x] Tyre marks that last the lap, darker and softer than the game's.
  - Done (`NFS_TYRE_MARKS`, `native_skids.cpp`): a minute by default
    (`NFS_TYRE_MARKS_LIFE`, seconds, fading over the last six), a ring of 8192
    blocks where the game had 64 (one shared ring, so the opponents' and cops'
    churn used to fill a small one in seconds), darker (the game's were 28 of
    255) and a little wider the faster the car laying them, uneven along their
    length with a faint gap now and then. The game marked only the player's
    slides, so the port lays a sliding car's marks itself -- every car, the
    opponents' and cops' too (`skidPedals`, on pavement, 30 cm a piece).
- [ ] Marks of their own on loose ground -- dirt, sand, gravel, mud, grass,
  snow -- where the asphalt's black rubber is not what a tyre would leave.
  - Researched (2026-11-19): a piece keeps only a shape and two alphas --
    one of four `skd` pictures, black RGB with the shape in the alpha, drawn
    by `sub_4c6f00`. Black blended over the ground darkens it, but darkening
    is not colour: over Redrock's sand it is dark tan, not the burnt orange a
    dug rut is; over grass it is green only as much as the ground already was;
    over snow only grey. A terrain mark wants a hue of its own.
  - The game already has the slot for it. `sub_4c6f00` builds each piece's
    polygon colour as `alpha << 24 | 0xffffff` (0x4c705e): a per-piece RGB
    that is simply always white. The world shader mixes the texture's RGB
    into that only while `v_combine.s` is on (`thrashrenderer.cpp`). So:
    - let the four `skd` batches leave the picture's RGB out of the mix (the
      records are the four at 0x8b41c8 once the sfx textures load, so the
      renderer can know them; the alpha still shapes the mark), and
    - patch that one colour build to OR in an RGB from a small palette
      instead of `0xffffff`.
    The surface has to reach the draw: the piece's byte at +0x310 holds the
    shape in its low two bits and has six to spare, so `skidLaid` (which runs
    at the end of every piece, the game's and the port's) writes
    `tint << 2 | shape` from `groundAt` at the piece, the texture lookup masks
    the low two, and the patched colour build uses the rest. No new pictures,
    no new draws, no shadow array -- one byte already stored and a palette
    lookup a piece.
  - Palette (a rut's own colour, dark because it is a rut; the alpha still
    follows the speed, the handbrake and the surface scales below):
    - pavement `#000000`, the black rubber it is now;
    - dirt and gravel `#6a3d1c`, Redrock sand `#7a4a22` -- burnt orange-brown,
      not the ground washed down to grey;
    - grass `#223d18` -- a dark green of its own;
    - mud `#2e2114` -- a wet, nearly black brown;
    - snow `#b9c6cf` -- now that the colour is the vertex RGB and not the
      black picture, a pale packed-snow line is possible; a low alpha, or no
      ring at all (see below);
    - other `#4a3a2a`.
    If dug-up dry soil should read lighter than a wet rut, the same tint can
    be lightened by the wheel's slip; one palette entry a surface is enough
    for a first cut.
  - Plan, cheapest first:
    - Dirt, sand and gravel (kind 2; surfaces 8, 9, 13) and mud (kind 4):
      real marks through the same ring and picture, the palette's tint, alpha
      x 0.5 and x 0.7 of the asphalt's, a little narrower (x 0.8) -- a turned
      rut over brown ground, a deep one over mud -- with the clods and the
      dust the wheels already throw on top.
    - Grass (kind 1): the dark green tint, alpha x 0.3 and a life a third of
      the asphalt's, a flattened dark strip rather than a scar; the blades
      carry the rest.
    - Snow (kind 3): the pale tint at a low alpha (about 0.2) for a few
      seconds, or no ring and the frost chips and spray alone -- those are
      the digging effect already.
    - Other (kind 5): as dirt, alpha x 0.4.
  - Colour by rpm or gear: no. The terrain sets the hue; rpm, slip and the
    handbrake set how much (the alpha already follows the speed and the
    handbrake). One palette entry a surface, not one a frame.
  - Sketch: the table in `native_skids.cpp` becomes kind -> { on, tint, alpha
    scale, width scale, life scale }; `skidPedals` and `skidLaid` look it up
    where they skip `groundUnder(...) != 0` today, multiply the piece's alpha,
    cap its life, and stamp the tint into +0x310. The ring, the fade, the
    smoke hook and the particle side stay as they are -- the wheels already
    throw the right dirt, blades and frost on those grounds. One lookup and
    one multiply a piece, no new draws and no new particles.
  - If patching the draw's colour build proves awkward, the fallback is four
    pre-tinted copies of each of the four pictures (twenty tiny textures),
    selected by the same tint bits -- same ring, same byte, no shader or
    vertex change, a little more to load.
  - Tests: Country Woods' dirt, Summit's snow, Atlantica's grass, a mud
    stretch (and `NFS_SURFACE_TEST` for each); a launch, a lock-up and a slide
    at 30 and 100 km/h. Watch that a tinted mark does not read as paint, that
    a low alpha still reads at a grazing angle, and the game's lift (`kLift`)
    so a rut does not z-fight the terrain.
- [x] Tyre smoke on wheelspin and lock-up, taken over from the game's kind 6
  and made procedural.
  - Done (`NFS_TYRE_SMOKE`, on): dark clouds of its own, ragged by noise. It is
    heavy now -- it hangs low, falls, lies on the road and is gone by six
    seconds (More realistic tire smoke below). On grass the same kind is still
    grass thrown up. It comes whenever a tyre is scrubbed, which is whenever the
    game lays a tyre mark: sliding sideways, a wheel spun up from a standstill,
    a wheel locked under braking.
- [ ] Heat haze over the road on desert tracks by day.
- [x] Brake discs glowing faintly orange after hard braking at night.
  - Done (2026-11-17, `NFS_BRAKE_GLOW`): braking hard above 8 m/s, a pool of
    orange light on the road at each wheel and a glow at its hub, up in 0.35 s
    and out over 1.25 s after; every car, fronts brighter, night only (Brake
    discs glowing at night).
- [x] Two broken shadows behind the rear wheels (2026-11-17): the tail lights'
  glow squares marked the road as car in the picture's alpha; light leaves it.
- [x] Wet road dry for frames at a time in the rain (2026-11-17): a flat draw
  of the game's between the sky and the road was taken for the HUD's start, so
  the reflection and the whole-picture pass came before the road. The HUD's
  start now waits for the road.
- [x] Tail lights' red on a wet road at night (2026-11-18): darker solid patches
  round it, and the streak half bright, half dark. The game's 4444 strips were
  counted as a car's (no haze under them) and taken for the HUD's start (the
  whole-picture pass between them); now drawn as light, and never the HUD's.
- [x] Wider, sharper reflections on the cars from the sky already captured.
- [x] Camera shake on big impacts and hard landings, by the hit's speed.
  - Done: `NFS_CRASH_SHAKE` (Crashes you feel).
- [x] Grass particles: off the road on grass, slightly elongated flying blades,
  more with speed, beside the dirt grains.
- [ ] 3D background objects, such as trees. 
- [x] If possible, make sparks organic and procedural too.
  - Partly done (sparks): every burst twice as many (`NFS_SPARKS`, 2), and
    walls throw them on a hit over 5 m/s and when scraped from 6 m/s (the
    game only above 22 m/s), stone and concrete too, bounced off the wall
    instead of into it.
  - Done (`NFS_PARTICLES`): each spark a white-hot streak cooling to red as
    it falls, skipping off the ground.
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
  - [ ] Rain puddles in scattered placed on the road in rainy weather
    (randomlized size and location) - shouldn't have any grip effect, just
    splash more water particles and more dense water particles than normal,
    higher than normal ones (2x higher), depending on speed.
- [ ] Add natural shadows given a light position, both for cars and environment.
  - Partly done: each car's shadow is stretched away from the track's sun,
    turning as the car turns, and its edges are soft (`NFS_SHADOW_STRETCH`,
    `NFS_SOFT_EFFECTS`). Real shadows (shadow maps) are too costly here.
  - Done: shadows fainter in the rain/overcast and at night
    (`NFS_SHADOW_RAIN`, 0.55; `NFS_SHADOW_NIGHT`, 0.4).
  - [x] Next: shadows for trackside objects.
    - Done: cast from the depth buffer towards the sun onto level ground
      (`NFS_OBJECT_SHADOWS`, 0.45); not by cars, which have their own.
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
- [x] Whole-image effects in the final blit: 2–4× anti-aliasing, a light bloom on headlights and sun, and a per-track colour grade. Each is cheap on desktop and can be turned off for weaker machines.
- [x] Haze: fog worked out per pixel with a sky-coloured gradient instead of a flat fog colour gives more depth for almost nothing.
  - Done: `NFS_HAZE`, `NFS_BLOOM`, `NFS_GRADE`, `NFS_AA` (FXAA), each on by
    default, `0` for off. Next: in the game's Graphics menu.
- [ ] Texture rework. Inspect the texture assets and rework them so that they
  are of high quality, then bake them in game again. This can include grass,
  buildings, desert, snow, dirt, fences, trees, etc.
- [x] softer emissive light glow from where lights glow (headlights, tail
  lights)
  - Done: the game's star-shaped lamp glow (`glw0`/`glw3`, both its 32- and
    16-bit copies) redrawn as a soft core and falloff, with a halo round each
    lamp from a layer of the lamps alone (`NFS_LIGHT_GLOW`, 1).
- [x] Motion blur (that gets stronger with higher speeds) on the environemtn
  mostly (not as much on cars) or other cheap but effective speed-feel
  enhancement techniques.
  - Done: the world smeared away from the screen's centre by speed over its
    distance (from the depth buffer), from 10 m/s, whole from 35; the cars
    stay sharp (`NFS_MOTION_BLUR`, 2).
- [x] Exhaust flames; when gearing up transmission, eject exhaust flames behind
  each exhaust of each cars. However before doing that, exhaust positions
  (notice that exhausts are symmetrical in terms of width axis) will need to be
  marked by a human; number and size of each and flame color. We will need a car
  model visualization tool for that. Stored as exhaust data ideally packed 
  within the car folder. If no data, no flames. 
  Then you can work out the flames and light emission.
  Flames can also appear at the beginning of race when revving and torque is at
  the top 20% or 15% highest.
  - Done: the marking tool (`tools/car_model.py exhausts`, or "Mark
    exhausts..." in `car_perf.py edit`) keeps each car's exhausts -- place
    and radius in the model -- as `exhausts.json` in its folder, with an
    optional `flame` colour (else `#f2380a`, lighter to near white in the
    middle). Shifting up throws one flame per tip, out of a ball of noise that
    grows for 0.18 s, holds and trembles for 0.8, and is gone by its one
    second; it stays at the pipe, going with the car (`NFS_EXHAUST_FLAMES`).
    A car with no file throws none. Every car of the race throws them, the
    opponents' too. Revving at a standstill with the engine in the top
    seventh of its revs (the start line) pops one out of an exhaust every
    0.15-0.35 s instead, a limiter's backfire.
    A car with nothing marked gets two default pipes with
    `NFS_EXHAUST_DEFAULTS=1`, so every car can be seen throwing them before it
    is marked.
  - Done: each flame pools a soft disc of its light on the road under the car,
    as wide as the ball is big, coming up, flickering and going out with it
    (`NFS_FLAME_LIGHT`).
  - Done: a hard downshift, and lifting off at the top of the revs, pop half a
    flame out of one exhaust at a time -- a real car's overrun crackle.
- [x] The headlights light up in front of the car well but show some explicit
  narrow beam brighter cones. These narrow cones shouldn't show up. The tail
  lights are also draw as 'wet trailing spots' on the road when they're turned
  off - disable this.
  - Done: the headlights' pool (`glw2`) was the two lamps' hot spots in a glow,
    laid on the road as two brighter beams: blurred now (and its smaller
    levels rebuilt from it), the pool smooth. The shader's own cone on what
    stands by the road is wide and soft, and gives the road next to nothing.
    The lamps' streak on a wet road (`glw4`) is not drawn by day, when the
    lamps are off (`unlitStreak`, `native_thrash.cpp`).
  - Done (2026-11-17): the red and dark smears behind every car in the rain by
    day were the game's other wet-road light (a 4444 strip) and a skipped
    batch's vertices drawn by the next one; both gone by day.
- [x] Tyre marks easier to make (2026-11-17): wheelspin pulling away and a brake
  held over a second lay their own; handbrake marks much darker; marks show on
  a wet road. Car shadows in rain/snow stronger (`NFS_SHADOW_RAIN` 0.8).
- [x] Tail lights dark after the start, no glow or trails (2026-11-17): the
  weather-box depth cut was discarding the softened glows; lamps exempt now.
- [x] Car shadows back (2026-11-17): the ellipse that took the square away left
  them almost unseen -- the car stands over all of it. A rounder rectangle now
  (`kShadowRound`, `kShadowSolid`, `kShadowDarkness`, `thrashrenderer.cpp`).
- [x] Rain falling again, and showers and lulls (2026-11-17,
  `NFS_WEATHER_MAX` 2.5): the port makes the rain and snow round the camera,
  at the game's rate on average.
- [x] No rain or snow in tunnels and under bridges (2026-11-17): the sky's share
  of the top of the picture, from the depth buffer.
- [x] Tyre marks under the brake past half and the handbrake, at once
  (2026-11-17).
- [x] Auto-exposure. Smooth brightness from the frame's average luminance in 
  the final blit, next to gamma. Tunnel mouths and dusk stops being a black flash. 
  - Done (2026-11-17, `NFS_EXPOSURE` 0.7): in the race's whole-picture pass
    rather than the final blit, so the HUD and menus are untouched. Metered on
    the GPU, centre-weighted; the picture follows the scene against the
    track's own usual brightness, so each track keeps its look and only dark
    or bright stretches are evened out.
- [ ] Rain on lens refinements: make particles of rain drops on lens slowly
  slide down to simulate gravity (however keep their shape) - moving by about
  a total of 1/60th of the  screen's height in 1 sec. This can be
  computationally additive (?) (try to make it cheap) therefore make 2/3 of
  current number of rain particles appear on screen to make up for it. Also the
  number of particles on screen should depend on current density of rain - the
  2/3 current number should be the max and at lowest rain density we get 1/2 of
  it.
- [x] Snow particles effect on screen; similar to rain, but pre-baked hexagons/snowflakes
  /other shapes. Please don't make them look cheap. They should have various sizes. 
  and there's no need for refraction on them - only semi-transparency.
  There are mostly transparent - towards the edges they're more transparent but
  their main lines are less transparent. Note that snow particles will be
  pre-rendered assets that just fall onto the windshield at different sizes.
  - Done (2026-11-18): pixel-art clumps of wet snow (nine big with gaps and
    crumbs, nine small), twelve tones from a light middle to a darker wet edge,
    falling down the view in the final composite, each column at its own speed
    within a fifth of the others, with no refraction. Each lives 6 s and melts
    to half its size over its last 2 s through six pre-baked shrink stages that
    keep its outline. No more flicker: the density (from how many flakes are
    alive, and the speed) is eased, and a flake leaving it fades. Their density
    is the weather's by up to 2.5 times at 300 km/h, and under a roof (a
    tunnel) they ease off the glass over a couple of seconds.
- [x] BUG: exhaust flames: they are visible from the front of the car too (e.g. 
  if you look back or change replay camera). The bug here is that exhaust flame
  does not get occluded by driver's car (get occluded by other objects though). 
  This way also side exhaust flames ghost through the rear bumper -> this is
  especially visible on cars with side exhausts.
  - Done: a flame no longer leans on the depth buffer to be hidden by the car
    it burns on. Each car with an `exhausts.json` also has its body read (the
    high-detail parts of its `car.fce`, wheels left out, out of `car.viv` as
    the game reads it), and each frame, in each view, five rays from the camera
    to the flame (its middle and four points round it) are tried against those
    triangles in the car's own space: the share that get through is how much
    of the flame shows, and its pool of light on the road goes with it
    (`exhaustBodyLetsThrough`, `native_exhausts.cpp`). A hit within 12 cm of the
    flame is its own pipe, and a camera inside the body (an in-car view) cuts
    nothing. About 6 us a flame on the heaviest body (the Hellcat's 6900
    triangles). The far-side test tried before (9ecd13f, reverted) took the
    view's translation for the camera's place; the camera is -t M^T.
- [ ] BUG: when press b to look back in rainy weather, the rain drops appear as
  lines (possibly due to rotation of camera view) - fix it.
- [ ] After extracting assets, make 3d models such as cops animating (e.g when
  fined/arrested) or be able to add custom model in whatever format the game
  uses (e.g. girls/crowd before race begins)

### Races/locations

- [x] Double the distance that headlights light up when on. Add smoother light,
  more realistic than before light that lights environment more naturally.
  - Done: the headlights' patch reaches twice as far and is 1.4x as wide
    (`NFS_HEADLIGHT_REACH`, `NFS_HEADLIGHT_WIDTH`), and fades softly at its
    sides and far end instead of ending in a box (`NFS_SOFT_EFFECTS`).
  - [x] Next: light on trackside objects (fences, walls) from the headlights.
    - Done: walls, fences, trees, buildings lit by the player's headlights in
      the shader (`NFS_HEADLIGHT_OBJECTS`, 1.6).
- [ ] Emissive light from environmental models that emit light. Note that this
  is partially implementned by the original game but check if it can improved
- [ ] High Stakes-only tracks (Park, UK, GT1-3): the road loads now, the race
  then faults in the collision code because the `.col` is another place's.
  Found and fixed on 2026-11-18, in this order, with `tools/port_track.py` and a
  race driven from the frontend by `NFS_TOUCH_SCRIPT` (which makes this
  testable without a pair of hands):
  - **High Stakes' `SKY.QFS` stops the whole track's setup**, silently: this
    game's horizon loader (`sub_478340`, before `sub_41d960`) sets the load's
    flag `[0x55e88c]` and the road is never read at all -- `[VIEW] 1 blocks`,
    then the frontend faults on the way back.  The port now keeps the game's
    own `sky.fsh` and puts it back if an earlier run left High Stakes' there
    (`--sky` still asks for High Stakes').
  - **The track arena overflows.** `sub_419bc0` returns 0 past the end of the
    3 211 264-byte arena and the read that follows copies into address 0:
    `[FRD] the arena is full: 10920 wanted with 3200428 used of 0x310000`, and
    the game runs a race with no track in it.  High Stakes carries ~10x this
    game's road polygons (Park's medium set alone is 800 a block against Rocky
    Pass's 107), so `port_track.py` now sheds detail until the track fits 85%
    of the arena -- the levels this game never uses (1, 3, 5, 7..10), then the
    high road set (4), then the coarsest (0), then the lanes (6), then the
    scenery.  Park lands at 85% with its medium road and lanes.
  - **Animated objects were written in a layout the loader cannot read**: the
    keyframes have to sit in the length-prefixed block the loader reads
    (`sub_4198e0`), not inline.  One mis-framed object read as 196608 vertices.
    The tool now reads and writes objects in the loader's own order and
    `loader_reads()` walks a ported file exactly as the game will, refusing one
    it would not read whole (all nine retail files read exactly, so the walk is
    known good).
  - **`--borrow` takes the .col from the snapshot**, not from the slot, which
    may already hold a ported track.
  - **The road's positions are the next thing.** Every retail block carries
    exactly eight `posData` records (8 bytes each) and a `start_positions`, and
    `port_track.py` writes 0 for both: HS's block header has a `positions` count
    but its block data has no such array where the reader looks, so the
    converter has nothing to copy.  With a ported road and *nothing else of
    Park's* (`--road-only`) the race still faults in the drawing, in
    `sub_41e6a0` — the function that turns a road polygon's four u16 corner
    indices into pointers into the block's 12-byte vertex records — which is
    what an empty positions array looks like once it is used as one.
- [ ] The `.col` of an HS-only track has to be generated (its vroad block
    is one 36-byte record a slice, in the road vectors' order at 1.3x, so it is
    mechanical; the walls are not).  Until then the port says so and the race
    faults where the road and the borrowed collision meet.
- [ ] Track editor. Tool to copy, edit, add new tracks with custom terrain and
  environemtn too.
- [ ] Replace the 3,2,1,GO spinning text when race is beginning with a move race-relaced UI,
  such as a LED panel with circular lamps lighting 3,2,1,GO.
- [ ] When race is beginning, instead of directly panning/zooming into player's
  car, camera moves along the opponents car's, reaching player's car and
  adjusting behing it, in the same duration as before.
- [ ] More and moving sky effects. For example  in summit, there are already
  northern lights at night which nicely glow on track. Make them more
  pronounced, higher quality and smoothly and proceduraly slowly move and glow.
  At day time in hometown and rocky pass u can add moving clouds which occlude 
  sun and reduce glare when in front of it.

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
- [x] Traffic with a full grid of opponents (a third of the usual), and the
  number of opponents unlocked in Hot Pursuit.
  - Done: `NFS_GRID_TRAFFIC` (two traffic cars with a full grid for one
    player) and `NFS_PURSUIT_GRID` (Hot Pursuit's full grid: six opponents,
    the most the cops' radio banks allow). The opponents' tuning no longer
    pushes traffic and cops about.
- [ ] Better police AI; based on chased player's style, how far he is, whether
  he is in their vision or not, adjust number of vehicles, strategy (e.g. rush
  to catch up, ram from side, trap from sides like a block, brake/drift in front
  of player to block him, smarter road blocks, spike strip -> not sure if we
  have a spike strip graphic, etc). Also in general, cops should be twice as
  likely to throw spike strips. Road blocks by cops should consist of 2-3 cars,
  not one.
- [x] In full grid hot pursuit, number of cop cars should increase 3x of what it
  nomally is -> partly done. Each race has 16 cars slots, full grid hot pursuit
  implies 5 total races, so more available slots for cops.
- [ ] Hot Pursuit: cops ahead of the player for a while drop a spike strip
  (the game never deploys them). Not too fast to drop, never right onto a
  car: far enough ahead to be seen and dodged. A cop car waits idle at one
  side of the strip and rejoins the chase if the target misses it or after a
  while. A hit cuts the target's acceleration and top speed for a time, by how
  many tyres went over it (1-4).
- [x] Hot Pursuit: cop cars 1.25x heavier, their performance and handling
  otherwise unchanged.
  - Done (`NFS_COP_WEIGHT`), and the cops faster: their cars tuned up 10-20%
    in pull and 8-15% in top speed by model (`NFS_COP_PACE`).
- [x] Cops should deploy their high-end cars (depending on location) on high
  heat levels, e.g. pursuit McLaren, Corvette, or Diablo. -> partly done by
  marking each slot with a car. However they never deploy McLaren (it's an added
  cop car).
- [ ] SERIOUS BUG: in the rain, opponents often lose control on turnining
  (possibly when braking, not sure?) causing them to turn 90 or even 180
  degrees, slow down dramatically, and then they have to re-align with their trajectory.
  Then they essentially lose the race as they have to spend entire seconds
  restarting from near-zero speed. Opponents should know better how to drive in rain and avoid such blunders.
  - No longer seen (Leo, 2026-11-14), after the AI and cop pace changes.
  - Re-opening this: happens only at beginner difficulty. Happens on weather=on.
  However still needs fixing.
- [x] Traffic: for each traffic car, halve its weight. This way they don't
  complely slow you down when crashing.
  - Done: mass and inertia halved (`NFS_TRAFFIC_WEIGHT`, 0.5).
- [ ] More persistent cops: currently they just chase you for a relatively short
  amount of time, disappear, rinse and repeat. Each cop car should: chase their target for much
  longer, disappear from maps if it has lost target, wait (cool down) for some
  period (e.g. 10-15 sec) and respawn in a position pretty close to target to
  chaseagain.

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
  - Done, the first half: every `NFS_*` switch the port reads now lives in
    `nfs3hp.ini` beside the game data, written out on the first run at its
    default and read once before the game starts (`settingsLoad`,
    `native_settings.cpp`). What is left is the menu: `settingsSet` and
    `settingsSave` are there for it to call.
- [ ] Add difficulty slider instead of just normal and agressive (presets in
  slider will still be normal and agressive but we can slide in between to
  adjust opponent AI). You already have some slider asset in volume control.
- [x] Currently in 'Control' menu, setting a key for brake other than arrows
  doesn't work. Bind x key to brake by keyfault ('keyboard x')
- [x] Fix brake key binding.
  - Done: letters (and digits, punctuation) reach the Controllers screen as
    on Windows (WM_CHAR), and X is the keyboard's default brake ("Reset to
    Defaults" to get it in a settings file already written).
- [ ] In performance view/head to head comparison of each car, have an
  'Advanced...' option which shows acceleration curve, top speed and 0-100
  acceleration for each car (like car_perf.py tool does but in game's UI).
- [ ] Minimap: instead of showing opponents/cops as big blobs, show them as                                                                  
  pseudo-voxel triangles, i.e. they consist of an invest Y shape with the bottom                                                             
  of Y as the top and lines that connect the tips. Note just flat single-color 
  triangles but pseudo-shaded. Cop triangles should ofc pulse in cop light color 
  scheme (red and blue). Also instead of a solid grey light for a road, have a
  grey straight line about 12px thick with darker grey outline on each side
  that's 3px thick.

### Driving/Gameplay
- [x] Tire grip by surface: less on snow, sand and grass than on asphalt. Each
  terrain must have its own grip properties (not just more slippery/less
  slipepry) that have an effect on player - e.g. slow down, unstable driving,
  slippery on turns etc and each terrain will have one main grip propery and 1-2
  secondary properties. These properties should be so that they're easy to
  implement. These properties do NOT affect opponent cars so opponent AI can
  disregard the terrtain - they only affect player.
  - Done (`NFS_SURFACE_GRIP`): gently, for the player only -- drag, softer
    steering, a little slip kept on loose ground, a light shake -- by the
    share of wheels on grass, sand/dirt/gravel, snow or mud.
- [x] By default, make grip properties apply to opponents too (switch:
  NFS_GRIP_ON_OPPONENTS=1 by default)
  - Done: every car in the race feels the ground, their driving unchanged.
- [ ] Nitro and nitro UI. However if you add nitro, opponents also need to know
  how to use it (e.g. to prevent them from staying inside the turn or blast it
  when they see enough straight road ahead, however they need to take crashes
  into account too). Note that if you draw the nitro ui, it should be seamlessly integrated 
  in the HUD; it should be right of the speed
  meter as a vertical  bottle-shaped bar if gauge is selected to be on the top part of screen,
  else as an outer circle around it.
- [ ] More means/more effective stratgies of police to chase you; e.g. at higher heat levels; helicopter
- [x] Mark exhausts in the side view too (some cars have side exits).
  - Done: `tools/car_exhausts.py` marks in any of its three views -- from
    behind, from the side and from above -- each picking the body's surface along
    the axis it looks down (`pick`), with mirroring and middle-click removal
    working in all of them. (`pick` was also wrong before: it mixed the corners
    up, so a click only landed by luck.)
- [x] Torque drag: the curve under the pointer disappeared; it is a ghost now.
  - Done: while a point is dragged the original curve stays drawn faintly
    (`GHOST_ALPHA`) so what it is changing from is in sight, and the new curve
    is drawn over it.
- [ ] Car visual customization; vinyls (at least some simple ones, like stripes,
  stars, checkflag flags, tribals)
- [ ] Car visual customization; bodykits and spoiler and side vents, rims etc - these
  also change the performance/weight distribution/traction.
- [x] More immersive crashes: small vibration on crash with other drivers, even biger
  vibration on wall. Vibration depends on speed.
- [x] More immersive crashes: different sounds, sound depending on crash speed. 
  - Done: the crash as loud as the hit and pitched by it (`NFS_CRASH_SOUND`).
- [x] Make speeding more immersive, i.e. gently pull camera back on pressing
  acceleration button up to some small distance , pull back in when releasing
  it.
  - Done: the chase camera eases back up to 1.2 m by the throttle, and in
    again when let go (`NFS_CAMERA_PULL`).
- [x] If you tackle tail light and headlight emission, upon speeding if it's
  light, leave a few trailing headlight tracks with speeding.
  - Done: from 80 km/h every car's tail lights leave a trail of fading glow
    behind it, longer the faster (`NFS_TAILLIGHT_TRAILS`, 1); and a short
    streak under each lamp on the screen from 10 m/s (`NFS_LIGHT_TRAILS`, 1).
- [ ] Rack of achievements: collect all kinds of achievements; e.g. reach
  250/300/350 in each car (a long as it's below its top speed), do a lap of less than
  XX:XX  in track Y, end up first after being behind last driver for n seconds,
  n seconds of air time, not caught after XX:XX minutes of total hot pursuit
  gameplay, 1st in track X with car Y, each A-class opponents with C class car
  on normal/aggressive difficulty, not crash with walls/drivers for n seconds
  while going over 100 kph, etc. Each can have a badge and classification. 
  Other achievements you can think of too.
- [x] Flipping after ramps: often when cars drive over ramps of ground, they
  jump high. But they turn two twist soo much sideways in the air, causing them
  to often flip upside down and then having to reset the car. Twisting like that should be
  much harder to happen - possibly set twisting effect via a switch. Jump up is
  high but twisting sideways is annoying.
  - Done (`NFS_AIR_ROLL_DAMP`): in the air a car's roll dies away, every car.
- [ ] Slipstreaming: slipstreaming (driving behind another driver) should be
  possible and emulate less air resistance, possibly by increasing acceleration
  (?). While slipstremaing a flickering faded icon bottom left shows up and
  showsthe slipstreaming time like XX.Y sec. Also add subtle visual cues, like
  'air line' trails left and right on player's only car that follow the car's
  path. Opponents can slip stream too and opponent AI knows it but it's not
  their priority. If slipstreaming AI is a lot of effort, just skip it.
  - Partly done (`NFS_SLIPSTREAM`, `NFS_SLIPSTREAM_TRAILS`): the push behind
    another car, for every car, and the air trails beside the player's car.
    Still to do: the icon with the slipstream's time.
- [x] More car visual feedback based on actions; e.g. Slight body roll during cornering.
      Front-end compression under hard braking.
      Rear-end compression during acceleration.
      Suspension compression when landing after a jump.
      Small vertical movements when driving over uneven terrain.
      Different suspension responses depending on the vehicle. No need to re-do the
      physics here, just tilt, or vibrate the body kit slightly based on motion.
  - Done (2026-11-17, `NFS_BODY_MOTION`): discreet roll in corners, tilt on
    slopes and a settle on landing, every car, as drawn only (Bodies on their
    springs). Still to do: uneven ground and a response per vehicle.
- [x] Trees and lamp posts shouldn't be immovable; they should be able to be
  destroyed the same way signposts get destroyed and respawn after 10 sec.
  - Done: knocked down like the signs, falling slower, slowing the car by
    their mass, standing again after 10 s (`NFS_KNOCK_TREES`,
    `NFS_TREE_MASS`, `NFS_LAMP_MASS`, `NFS_KNOCK_RESPAWN`). Which objects are
    trees and lamp posts is guessed from their models' size.
- [ ] More paint types; iridescent, glossy, chrome, matte. However when
  selecting paint > custom, you will need a UI arrow and list to choose it.
- [x] BUG: some cars like CLK-GTR have 3 entries or duplicates, remove
  duplicates. zip them and move them to /tmp first just in case i wanna
  re-import them. also modify the car import script to list /remove model(s)
  (for art files and gamedata). Particularly for CLK-GTR, one is plain (no
  decals etc) and 2 have racing vinyls, i wanna keep only of the vilyn ones and
  remov the plain. Datsun and Camaro are also duplicated and I think Lister too.
  - Done: `tools/import_car.py --list` prints every car with its code, ID and
    make/model and marks duplicates; `--remove` zips each folder to `--backup`
    (the temporary folder by default) and takes it and its `carorder.txt` line
    away. Removed: the plain CLK-GTR (`clk2`) and a second copy of the liveried
    one (`clk3`, `clkg` kept), the Camaro `chev` (`cama` kept), the Datsun
    `d24z` (`dats` kept) and the Lister Storm `lstm` (`lst2` kept) -- 49 cars
    down to 44.
  - Done again (2026-11-17), with the tool doing it: `--dedupe` finds every
    pair and keeps the copy installed first (the lowest car ID -- the game's own
    car where one of the pair is one), zipping the rest to `--backup`; 54 folders
    to 48 cars, 0 duplicates. Out: `cc5r` (C5R, `chev` kept), `rena` (Clio,
    `clio`), `clkg` (CLK-GTR, `merc`), `nfs3` (F50, `ff50`), `impw` (Impreza,
    `suba`) and `p206` (206 WRC, `peug`).
  - Still to do: the removed folders' showcase art and slides are in
    `fedata/art` (and their menus' entries), so a duplicate's picture can still
    be chosen for a slot the car it belonged to no longer fills.
- [ ] S-class: some cars are way too fast for A-class, such as murcielago and
  ferrari f40. create a new S-class and be able to select S class as opponents
  as A+S class. I know these need a slide for the menu but don't worry - i'll
  create it later and just use a placeholder for it.
- [ ] Lift the limit of 50 cars for usable cars.
- [x] Remove airborne slowdown
  - Done (`NFS_AIR_SPEED`, on): off its wheels a car is dragged down by the
    game -- a jump costs speed the ramp never took. Each tick the speed it left
    the ground at is put back on its velocity (sideways and forward, the fall
    left to gravity), from `airSpeed` in `native_surface.cpp`; a hit in the air
    still loses what a hit loses, and on its wheels nothing is put back.
- [ ] Police mod: call police backup when pressing H on a player - other cops
  spawn near u and chase him.
- [ ] Better weight distribution consideration in collision: most cars have the
  engine on the front so most of the weight (consider 3/4?) is distributed on the
  front. Some higher-class cars like ferraris and lambos and the A-class jaguar,
  the clk gtr etc have the engine on theback so it's the opposite. Traffic cars
  also have engine on the front. Question: should this only apply to cops? Would
  it cause too much cause on regular players (opponents ramming each other like
  crazy). Maybe we need a NFS_ switch for each?
- [x] Better control in the air - at least be able to turn left right slightly
  (not with as much control as the ground, with some damping)
  - Done (`NFS_AIR_STEER`, `airSteer` in `native_surface.cpp`): the player's
    car turns about its up axis by the steering while in the air, easing to
    0.9 rad/s at full lock over a third of a second, back to none when let go.
- [ ] Be able to select any number of laps (1-99), not just 2,4,8.
- [ ] Opponents choose always: one copy of player's car and game's default cars.
  They should also be able to choose from added cars (given that class
  restrivtions are repsected)
- [ ] misbehavior: since air control got better, now when a car is toppled
  sideways, is still drives but very slow, doesn't reset but keep driving. i
  know it's against physics but when a car is in two sideways wheels, it should
  have some artificial gravity to bring it back to 4 wheels. To be more precise:
  if two or more wheels lose contact with ground and the sideways tilt is close to 90 degrees
  (around 70 to 90) roll it back to the ground smoothy so all 4 wheels make
  contact. If it is moving too slow (<60kph) on two or more wheels of contact and it's toppling ( >
  90 degrees between base of car and horizon) for more than 0.5 sec, auto-reset
  it (like pressing r key).
- [ ] Gimmicky but fun: windshield wiper + graphic that wipes off snow/raindrops
  from screen.
- [x] More realistic tire smoke: tire smoke particles and color area great, but
   tire smoke shouldn't spread as rise as it does now - it's
  supposed to be kind of heavy and fall on the ground pretty quickly. Also it's
  generated either at high RPM when car is still or barely moving (e.g. start line, slamming the 
  acceleration at low speed), whenenever the games plays
  the time squeek sound (it's probably easy to figure out the branch for it), 
  or whenever tire marks are generated. It also takes a while to disappear (if
  it hasn't hit the ground) and gradually disappears by 6 sec after being
  generated. Density look of smoke (not necessarily number of particles - be
  careful since way too many particles could drop perfomance) depends on either
  (whatever is easier to implement): decelleration mangitude + turn sharpness OR speed
  and break duration - we have different scales depending on speed - [0, 50],
  [50, 100], [100, 150], [150, 200], [200, inf] kph and duration of braking [0, 1],
  [1, 2], [1,2], [2,4], [2, inf]. Choose the easier to implement approach.
  - Done: the smoke is heavy now -- it leaves the tread with almost no upward
    speed, drags hard, falls and lies on the road, and is gone in 6.5-8 s from
    being made (fading gradually over its last seconds). Its life, size and
    height-to-width vary a little from cloud to cloud, and the gap between two
    clouds is 3-6 ticks a car. It comes off every piece of tyre mark
    (`tyreMarkSmoke`, from `skidLaid`), a wheel spun up at low speed, the
    player's brake and handbrake, and the game's own kind 6. Holding its speed
    for a second while smoking switches a car to the drifting clouds: they
    start smaller, billow instead of falling, and grow unevenly to saturation,
    about 1.5x as wide and 2x as tall as the ordinary ones. Its look is the
    speed + braking-duration table -- 0-50/1 s, 50-100/2 s, 100-150/2 s,
    150-200/4 s, 200+/4 s, with a third at once -- and the throttle or the
    slide's sideways drag where there is no brake (`throwSmoke`, `tyreSmoke`,
    `native_particles.cpp`; `NFS_TYRE_SMOKE`).

### Music/Sound
- [x] Sound quality is great and sound works well but volume is still too
  low/supressed.
  - Done: the plain limiter gave most of the lift back (a race is wall-to-wall
    peaks); now the peaks are rounded off and a look-ahead limiter catches the
    rest (`NFS_VOLUME`, 1.8 by ear).
- [x] In a brief popup on the bottom left whose visual style is consistent with game, show
  cyrretnly plating song when it starts and then slide the popup away.
  Background same as HUD background, on the let there's the nfs icon attached
  that's found also in main menu when selecting single play, two player, hot
  pursuit, etc, song is like: <artist: yellow letters> - <song: while letters>
  and around the popup there's yellow glowing outline. The popup 'slides in' and
  'slides out'.
  - Done: a second into the race the panel slides in from the left -- the HUD's
    navy, a yellow glow round it, the NFS emblem of the HUD's own art
    (`hud98.fsh`), the artist in yellow and the song in white in the HUD's font
    (`cp16bi.ffn`) -- stays 5 s and slides out (`NFS_NOW_PLAYING`,
    `native_now_playing.cpp`, `ThrashRenderer::drawOverlay`).
- [ ] Music control: what tracks play in game, skip current track button.
  - Done: skip to the next song, `]` by default, bound under Controls (Next
    song; the list scrolls now, a triangle at either end), the panel showing
    it (`NFS_NEXT_SONG_KEY`).
- [ ] Radio: be able to add your own tracks.
- [x] Crash sounds by how hard the hit was; metal scraping rising with speed.
- [ ] Doppler Effect: Passing rival cars, tunnel walls, and roadside barriers 
creates rapid whooshing sounds that rise and fall in pitch as you fly past them. 
Obviously this effect depends on speed.


### Career mode
- [ ] Add one more game mode other than single player, hot pursuit, etc called
  Career. There we race, unlock cars, earn money, get body damage on car, fix
  car, etc.

### Performance
- [x] Races take quite long to load by defaul. Find out if you and reduce
  loading times by caching anything etc.
  - Done (`NFS_FAST_LOAD`): the loading picture's flips were each held to the
    frame rate; unpaced and mostly not drawn, Atlantica loads in about 0.3 s
    here instead of 13.5.

### Infrastructure
- [ ] Asset extractor (roads, signposts, lamps including emmission, buildings, etc) and re-injecter. Figure
  out a way to export assets and view/edit them in a tool, or add custom assets
  and place them in locations in games. 3D model (such as for cops etc) extractor.
- [ ] Create a track editor tool which can: edit/remove track blocks to design
  the road, add textures, objects, etc with live preview.
- [ ]  Make a way to convert 3d models .obj files into game environment ones OR 
  directly proceduraly make models and insert them.
- [x] Car tool: drag the torque curve, rebuild the AI's acceleration table on Save, see the
  acceleration the tune makes as you drag.
  - Done: on the editor's torque chart each of the curve's 41 points is a mark to
    drag (`CurveDrag`), and the box below Save says whether Save rebuilds the
    game's ACC acceleration table from what was tuned (`derive_tables`), its
    level anchored on the car's own figures so an add-on's optimistic engine data
    cannot hand the AI an impossible table. Fixed with it: `car_perf.py derive`
    refused any car whose ACC section is not 14 numbers long (an add-on's can be
    a whole 98-sample curve -- `clkg`'s is), so `write_accel` now fills each
    section to its own length.
  - The acceleration chart shows the game's table solid and the tune's own curve
    dashed over it -- for a car with unsaved edits only, since the model's curve
    and EA's differ even on an untouched car and drew dashes on every car in the
    list (and left them after Cancel) -- live while a point is dragged (the two charts are frozen and
    the moving curves drawn over them -- under 5 ms a step, where drawing the
    charts again was over 100). Both meet exactly after a rebuild, which is also
    the check that the preview and Save agree.
- [x] Peformance curve and other stuff like transmission visualizer based on the data of each car model
  - Done: `tools/car_perf.py report` (`tools/car_report.py`): a sheet per car
    (torque/power, gearbox, push in each gear, acceleration, specs), every car
    compared, and a CSV.

- [x] Port tracks from Need For Speed: High Stakes (`nfshs/.../data/TRACKS`).
  - Done: `tools/port_track.py NAME [--slot N] [--out DIR] [--borrow N]
    [--force]`. The FRD is converted (High Stakes' layout read, this game's
    written: road vectors, block headers and payloads reordered, 13-byte
    polygons to 14, per-polygon road vectors collected into each block's list);
    the road's objects (`.CAN`), heights (`HEIGHTS.SIM`), textures (`TR0.QFS`)
    and sky (`SKY.QFS`) are the same formats and are copied; the four `TR*.INI`
    become the four `.hrz` files, key for key -- Hometown's horizon comes out
    with this game's own numbers. Nine of High Stakes' nineteen tracks are this
    game's own (`trk000` home, `trk005` Lost Canyons, `trk006` Aquatica, ...;
    the art table at `0x559620` and one-for-one road-vector counts), so
    `--slot N --force` replaces the retail track in place. Every conversion is
    read back with this game's layout and index-checked.
  - Fixed on the way: **High Stakes' world is this game's divided by 1.3**. The
    same road vectors in the same order, the same block centres, heights 1/1.3
    -- `HEIGHTS.SIM` x 1.3 is `simhts.dat` to float noise on all nine shared
    tracks. Everything converted is scaled by `SCALE` now; before that the
    ported tracks sat 23% small under this game's cars.
  - Undone by the tool itself: the first write into `gamedata/tracks` takes
    `gamedata/tracks-default.tar.gz` (the tracks as shipped, verified byte for
    byte against the copy taken before any port) and `--reset [SLOT ...]` puts
    them back -- every file of every slot, and away with any track that is not
    the game's -- with `--status` to see how each slot stands and `--dry-run`
    to look first.
  - Left: **collision**, and it is portable. `trNN.col` is `COLL` v11: a texture
    table, a struct3D of collidable meshes, object tables, and a road-vector
    table -- 36-byte records, one per slice, the same count and order as High
    Stakes' own road vectors at 1.3x (959 for Hometown). The road half is
    therefore a re-encoding: `refPt` as 16.16 fixed, the three vectors as
    signed bytes x128, `leftWall`/`rightWall` as 16.16 but *not* High Stakes'
    walls x a factor (this game's is High Stakes' + about 3.4, +-0.7, so a
    generator wants the road's own width instead). Still to do: calibrate the
    walls, map the texture ids in the texture table (same numbering the
    polygons need), and build struct3D meshes from High Stakes' FRD objects.
    For the nine shared tracks `--borrow` is exact and is the default.
  - Left: the ten tracks this game never had (Coastal, Snowy, Hills, Park,
    France, Germany, UK, GT1-3) have no slot the menus offer: slots 9 to 13 are
    empty in the exe's art/place tables, but `location.mnu`'s `tracks` list is
    filled for nine, so showing a tenth needs the menu and its art.
  - **Fixed (2026-11-18): the FRD's opening word.** Every retail track opens
    `0x0135` then `0xbeef`; every High Stakes one `0x0058` then `0xbeef`. Keeping
    High Stakes' made the game load *no* track at all -- a race that came up
    with `[VIEW] 1 blocks`, default fog, every bot `d0`, then fell back to the
    menu and crashed there on `slides/-1_00.qfs`. The converter now writes
    `0x0135`.
  - **Crashed (2026-11-18)** on the first drive of ported Aquatica, and the
    likely cause is found: High Stakes' repeated objects (its per-block
    `polyobj` list) index the polygon banks that are HS's *extra* chunks 7 to
    10 -- which this game keeps as the four `polyobj` chunks per block, and the
    counts match exactly (HS `[28, 0, 10, 0]` to this game's 28, 10) -- and the
    converter was writing those banks empty while keeping the indices. Left out
    by default now (`--repeated-objects` to put them back); what is still
    missing is how HS splits a bank between its objects.
    `--road-only` / `--skip objects,textures,sky,heights,hrz` bisect a port that
    will not load. Ruled out on the way: texture ids (HS and retail Aquatica
    use the same 93, max 147), the polygon filler byte (retail's is 0xF9
    throughout, which is what is written), the polyobj record stride (20 bytes:
    all 120 block centres of the converted track land on the retail ones).
  - Unverified: no race has been driven on a ported track yet -- whether the
    renderer wants High Stakes' four dropped scenery chunks, whether its texture
    numbers land on the right pictures, and whether its roadside objects come
    out as the right ones: Hometown's first block asks for object kind 7 in High
    Stakes and 9 here, though the `.can` catalogues are the same file byte for
    byte.

- [x] The exhaust marker crawling on a heavy car -- the Chevrolet Corvette C5R
      (`chev`, 5282 faces) of a car that answered quickly (2026-11-18).
  - Done: a car of more than `HEAVY_FACES` (1200) is drawn as **one picture a
    view** instead of a `PolyCollection` of every face, the colours off
    included (`textured_view` takes `texture=None` and fills the body's grey a
    face at a time). The Diablo's 720 faces still go face by face, edges and
    all.
  - `NFS_TOOL_HANG=10 python3 tools/car_exhausts.py chev` dumps a stack every
    ten seconds if it still stops answering, so the next report says where.
