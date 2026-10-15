# Costume Quest for Android: the GPU-specific APKs

Costume Quest for Android (v1.1.7, versionCode 121030) was sold through the Humble Bundle as four
APKs, one per GPU family, because each GPU of the time supported a different texture compression
format. The game code is the same in all four; only the 1 GB data pack (`assets/ant.mp3`, a zip of
the engine's cooked resources) differs. Measured 2026-10-15 from the copies on archive.org
(`costume-quest-android`) by running each through the Windows build with `CQ64_GL_STATS=1`, which
counts the texture uploads by GL format during the new game's opening:

| APK | Size | MD5 | `ant.mp3` | Textures the game uploads |
|---|---|---|---|---|
| CostumeQuestAdreno.apk | 678,097,459 | 6ca9db6e… | 667 MB | **ATC** with interpolated alpha (`0x87EE`), ~2,500–3,000; the rest uncompressed |
| CostumeQuestMali.apk | 1,073,832,854 | 72c32811… | 1,063 MB | **uncompressed** only: RGBA8 (~3,300), luminance+alpha (~1,550), luminance, RGB565 |
| CostumeQuestPowerVR.apk | 687,775,789 | b7035be5… | — | **PVRTC 1**: 4 bpp RGB (`0x8C00`) and RGBA (`0x8C02`), some 2 bpp (`0x8C01`, `0x8C03`) |
| CostumeQuestTegra.apk | 636,468,793 | b263b00b… | — | **S3TC**: DXT1 (`0x83F1`, ~1,900) and DXT5 (`0x83F3`, ~2,300) |

- `lib/armeabi-v7a/` holds the same game and FMOD libraries; `libCq.so` is 12,907,304 bytes
  (MD5 52e0a8bd… in the Mali APK). All four copies carry the same repacker additions
  (`assets/support_classes.jar`, `config.bin`), which the Android build strips.
- Textures are the engine's `*.Txtr~c` resources (header magic `dfcf`), with `*.Txtr~d`
  descriptors holding only the texture's path. The Mali textures are about 5x the size of the
  Adreno ones (e.g. `arena_rocky`: 687,781 vs 131,253 bytes).
- How the game picks a GL format (`TextureInternal::FromDDS` → `DFtoGLrawFormat` in libCq.so):
  its GL wrapper (`glue_init_extensions`) sets one flag per extension name it finds (whole-word
  match, e.g. `GLUE_AMD_compressed_ATC_texture`, `GLUE_IMG_texture_compression_pvrtc`,
  `GLUE_NV_texture_compression_s3tc`, `GLUE_EXT_texture_compression_dxt1`). The engine's format 65
  is DXT1, 66 DXT3, 67 DXT5, 70 ETC1, 73 ATC; formats above 63 are "compressed". A compressed
  texture whose extension flag is missing is uploaded as `glCompressedTexImage2D(GL_RGBA)`, which
  every GL rejects: the texture stays black. So each APK only works where its format is advertised.

## What CQ64 does

- **Android**: the phone's GPU decides. The Mali APK works everywhere. The Adreno APK's ATC
  textures are decoded to RGBA8 by the host when the GPU lacks ATC, but only if the game is told
  ATC exists, which the Android build does not do yet (so on a non-Adreno phone the Adreno APK
  would most likely draw a black world; not tested). The Pixel 8 Pro test phone runs the Mali data.
- **Windows** (ANGLE on Direct3D 11): the extension list given to the game adds ATC and PVRTC
  (decoded to RGBA8 by the host) and the standard S3TC names (ANGLE supports DXT1/3/5 natively).
  **All four APKs render correctly.**

The Humble Bundle article that listed which devices take which APK
(support.humblebundle.com/hc/en-us/articles/204372938, archived 2016) could not be fetched from
the Wayback Machine at the time of writing (rate limited).
