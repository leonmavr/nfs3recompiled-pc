# minimp3

An MPEG audio Layer III decoder, public domain (CC0 1.0, `LICENSE`), by
[lieff](https://github.com/lieff/minimp3). Vendored here so the port can play
the player's own MP3s in a race (the radio, `src/nfs3hp/native_radio.cpp`):
the game's own decoder only knows EA's XA ADPCM, which is what its music and
movies are made of.

- `minimp3.h` is upstream's, unchanged:
  <https://github.com/lieff/minimp3/blob/142823796ab8efa55e1bf11406028f3be77ecceb/minimp3.h>
- `minimp3.c` is the port's: the implementation switched on, Layer III only.
- `LICENSE` is upstream's CC0 1.0 Universal.

To update: replace `minimp3.h` and `LICENSE` with a newer upstream revision and
say which one in this file.
