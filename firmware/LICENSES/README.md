# Licence texts

Glag's own code (`glag/`, `host/`, `web/`, and the Glag changes in `app/`) is MIT. This folder
holds the licence texts of everything else in `firmware/` or in a built image.

| File | Holder | Covers |
|---|---|---|
| `Wakes-MIT.txt` | Adara Barami, Worldwave | `app/src/sp1_*` and `app/src/main.c` (Wakes' SP-1 base: power, watchdog, standby, battery, controls, LEDs, console, audio bring-up), `app/Kconfig`, `app/prj.conf`, `app/app.overlay`, `tools/ci/check_image.py`, `west.yml`. Changed for Glag where marked `CONFIG_SP1_GLAG`. `Wakes-NOTICE.txt` is Wakes' own attribution, kept as it was. |
| `MIT-sp1-tape-looper.txt` | chattock | the board definition, power-off and watchdog sequence, and codec bring-up that Wakes adapted from chattock/sp1-tape-looper |
| `MIT-SP-1-dev.txt` | Tim Knapen | the SP-1 pin map and hardware documentation from timknapen/SP-1-dev |
| `MIT-sp1-midi.txt` | Eric Lewis | the original `stem_player` Zephyr board that the board definition descends from |
| `Apache-2.0.txt` | the Zephyr Project contributors | `boards/teenageengineering/stem_player/Kconfig.defconfig`, `Kconfig.stem_player` and `stem_player_defconfig` (from Zephyr's board template), `zephyr-patches/`, `app/src/sp1_usbd.c` (from Zephyr's USB sample helper), and the Zephyr RTOS compiled into every image |

A built image also contains other Zephyr modules (CMSIS, Nordic's `hal_nordic`) under their own
licences.

Glag takes no code from AMB-1 (MPL-2.0), mdkit or the LXR firmware; AMB-1 is its inspiration only.
