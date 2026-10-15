# Costume Quest for Android: the GPU-specific APKs

Costume Quest for Android (Double Fine, v1.1.7, versionCode 121030, package `com.doublefine.cq`)
was sold through the Humble Bundle as four APKs, one per GPU family of the time, because each GPU
supported a different texture compression format. This page records what differs between them and
how the game picks a texture format. Measured 2026-10-15 from the four copies in the archive.org
item `costume-quest-android`. No game files are in this repository.

## The four APKs

| APK | Size (bytes) | MD5 | SHA-256 |
|---|---|---|---|
| CostumeQuestAdreno.apk | 678,097,459 | `6ca9db6ee5235857d2fa1a59c0a62ef5` | `189250d464fe04b606b3027a107a882aa7421b0527e1a382b059e03756a70b66` |
| CostumeQuestMali.apk | 1,073,832,854 | `72c328118cd3f33fcd50ded07ce7277b` | `0dd44f763730a30d93f75b958c39d4a380a32ad6afd2e0c12034e707e25b177e` |
| CostumeQuestPowerVR.apk | 687,775,789 | `b7035be5e0af2a733cdc1bf5ea0de5f2` | `e402a22cde98441ca0240078175f1d6550dd70247c686911b282974e9dabe809` |
| CostumeQuestTegra.apk | 636,468,793 | `b263b00baea2708a70b9732cfb340c2a` | `20bd46426a498da624c23ffd6db4196bf451e24bfd53c395cf9fda8cf7487f7c` |

All four: versionName 1.1.7, versionCode 121030, each signed separately (only `META-INF/` differs
outside the data pack).

## What is the same

- **Code:** `lib/armeabi-v7a/` is byte-identical in all four:

  | Library | Size | MD5 |
  |---|---|---|
  | libCq.so (the game) | 12,907,304 | `52e0a8bd49fa2602dd9bdfe24df47020` |
  | libfmodex.so | 850,580 | `19d605d2494f76c94534a2f5a998b0a5` |
  | libfmodevent.so | 370,664 | `a96ba6c54240055e7121aad109734342` |
  | libfmodexD.so / libfmodeventD.so (debug FMOD) | 5,868,564 / 3,568,344 | `1dfed283…` / `c81b5065…` |
  | libfaad.so | 88,945 | `69927226b8259b4b8216fcb08385fbb9` |

- `classes.dex`, `res/`, `resources.arsc` and the manifest are identical, so the variant can't be
  told from the code: only the data decides.
- The archive.org copies all carry the same repacker additions in `assets/` (`support_classes.jar`,
  `config.bin`, `CMG_config.meta~*`, `d_data_gstore.dat*`, `feedInfo.dat*`), which are not part of
  the game; the CQ64 Android build strips them.

## What differs: the data pack

`assets/ant.mp3` is not audio: it's a zip (stored, uncompressed, inside the APK) of the engine's
cooked resources, 35,321 entries in every variant, under `OGLES/Munged/`, `Data/` (the Flash UI
movies, `*.gfx`) and `ant/`.

| Variant | `ant.mp3` size | `ant.mp3` MD5 | Textures (`*.Txtr~c`, 3,898 files) |
|---|---|---|---|
| Adreno | 667,678,446 | `79f17736c9e4d6a09851d808ee1e7813` | 196,253,979 bytes |
| Mali | 1,063,413,842 | `717374c8584a290a76a86dd1433889bc` | 591,992,136 bytes |
| PowerVR | 677,356,778 | `806e08bc7361dd0fcc749ff126ee018a` | 205,932,783 bytes |
| Tegra | 626,049,785 | `7d756e1ceb8e8477b4575acf84357411` | 154,614,001 bytes |

- 3,388 of the 3,898 textures differ between variants; the other 510 are the same in all four.
- Everything else is the same size in every variant, but a few other resources differ in content:
  698 rig bodies (`*.RgBdy~c`), 169 inventory items (`*.ItemR~c`), 24 world objects
  (`*.StObj~c`), the 4 string tables (`*.Story~c`), `Stamp.RStmp~c`, `visualtypes.VsTyp~c` and 5
  `*.bchm~*` files. Most likely each variant's pack was cooked separately; these were not compared
  further.
- Textures are the engine's `*.Txtr~c` resources: a 64-byte header (magic `dfcf`) followed by
  zlib data holding a DDS image. `*.Txtr~d` only holds the resource's name (e.g.
  `particles/textures/test/null:Texture`). The Mali textures are about 5x the
  size of the Adreno ones (e.g. `arena_rocky`: 687,781 vs 131,253 bytes).

## Texture formats the game uploads

Counted with an instrumented build of the CQ64 host (texture uploads by GL format) during a new
game's opening on each variant:

| Variant | Formats |
|---|---|
| Adreno | **ATC** with interpolated alpha (`GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD`, `0x87EE`), ~2,500–3,000 uploads; the rest uncompressed |
| Mali | **uncompressed** only: RGBA8 (~3,300), luminance+alpha (~1,550), luminance, RGB565 |
| PowerVR | **PVRTC 1**: 4 bpp RGB (`0x8C00`) and RGBA (`0x8C02`), some 2 bpp (`0x8C01`, `0x8C03`) |
| Tegra | **S3TC**: DXT1 (`0x83F1`, ~1,900) and DXT5 (`0x83F3`, ~2,300) |

## How the game picks a GL format

`TextureInternal::FromDDS` → `DFtoGLrawFormat` in libCq.so. Its GL wrapper (`glue_init_extensions`)
sets one flag per extension name it finds in `glGetString(GL_EXTENSIONS)` (whole-word match), e.g.
`GLUE_AMD_compressed_ATC_texture`, `GLUE_IMG_texture_compression_pvrtc`,
`GLUE_NV_texture_compression_s3tc`, `GLUE_EXT_texture_compression_dxt1`. The engine's format 65 is
DXT1, 66 DXT3, 67 DXT5, 70 ETC1, 73 ATC; formats above 63 count as compressed. A compressed texture
whose extension flag is missing is uploaded as `glCompressedTexImage2D(GL_RGBA)`, which every GL
rejects: the texture stays black. So each APK only works where its format is advertised, and the
Mali APK (no compressed textures) works everywhere.

The game checks about 140 extension names in all (they are the `GLUE_*` strings in libCq.so), and
several besides the texture formats change how it draws (float render targets, instancing, buffer
mapping), so a GPU that offers more than a 2013 phone did can put the game on paths it was never
tested on.

## The reference phone

The Pixel 8 Pro's GPU (Mali-G715, OpenGL ES 3.2, driver r54p3) offers only ETC1 and ASTC texture
compression: no ATC, PVRTC or S3TC. So of the four, only the Mali data runs on it as shipped; the
CQ64 test phone uses the Mali data pack (`cqdata.zip`, 1,063,414,020 bytes).

## What CQ64 does

The phone's GPU decides. The Mali APK works everywhere. The Adreno APK's ATC textures are decoded
to RGBA8 by the host when the GPU lacks ATC (`atc_decode` in `host/src/thunks_gl.cpp`), but only if
the game is told ATC exists, which the Android build does not do yet (so on a non-Adreno phone the
Adreno APK would most likely draw a black world; not tested). The PowerVR and Tegra APKs need
their formats on the GPU (PVRTC: PowerVR only; S3TC: rare on phones).

The Humble Bundle article that listed which devices take which APK
(support.humblebundle.com/hc/en-us/articles/204372938, archived 2016) could not be fetched from
the Wayback Machine at the time of writing (rate limited).
