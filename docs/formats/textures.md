# Textures in game archives

Implementation: `src/assets/texture.{h,cpp}`. Tool: `texdump ARCHIVE [NAME OUT.png]`. Tests: `tests/texture_test.cpp`.

## Detecting the format

The format is taken from the file contents, never from the name:

| Test | Format |
|---|---|
| starts with `DDS ` | DDS |
| starts with `BM` | BMP |
| an 18-byte header whose every field is plausible (image type 1/2/3/9/10/11, sizes non-zero, legal bit depths, no interleave bits) | TGA (has no magic, so it is tried last) |

Names are unreliable: 21,289 files named `.bmp` hold DDS data (every texture of `qeynos2.s3d`, for example), and 16 files named `.dds` hold something else (see Failures). OpenEQ also sniffs the magic for the same reason.

## Corpus survey (2026-10-20)

The survey ran `texdump` on every `.s3d` (1,177) and `.eqg` (1,532) archive in the root of the RoF2 install (no archives live in subdirectories). It covered every member named `.bmp`, `.dds` or `.tga`, plus any other member that starts with DDS or BMP magic. That is 170,192 members: 94,949 named `.dds`, 75,226 `.bmp`, 16 `.tga`, and 1 with no extension (`…_10ndds`, a DDS file). 170,175 decode. The 17 failures are bad data.

Stored formats (the `[...]` column of `texdump`):

| Stored format | Count | Decoded as |
|---|---:|---|
| DDS DXT1 | 60,909 | BC1 |
| BMP 8-bit palettised (40-byte header) | 53,838 | RGBA8 |
| DDS DXT5 | 40,076 | BC3 |
| DDS A8R8G8B8 (32-bit, masks ff0000/ff00/ff/ff000000) | 11,677 | RGBA8 |
| DDS A4R4G4B4 | 1,414 | RGBA8 |
| DDS R8G8B8 (24-bit) | 1,045 | RGBA8 |
| DDS DXT3 | 960 | BC2 |
| BMP 16-bit (BI_RGB, X1R5G5B5) | 56 | RGBA8 |
| DDS DXT1 cube map | 48 | BC1, +X face only |
| DDS DXT5 cube map | 38 | BC3, +X face only |
| DDS A8R8G8B8 cube map | 21 | RGBA8, +X face only |
| DDS `Q8W8V8U8` (D3DFORMAT 63 in the FourCC field) | 15 | RGBA8, biased |
| DDS X8R8G8B8 | 10 | RGBA8 |
| BMP 24-bit | 10 + 6 (V5 header) | RGBA8 |
| TGA type 2 (uncompressed) 32-bit / 24-bit | 10 / 5 | RGBA8 |
| BMP 8-bit with V4/V5 header | 7 + 5 | RGBA8 |
| BMP 32-bit BI_BITFIELDS (V5 header) | 6 | RGBA8 |
| DDS R5G6B5 | 5 | RGBA8 |
| BMP 32-bit BI_RGB (`oldblackburrow.eqg` `stone_ext06_*`) | 5 | RGBA8, alpha 255 |
| BMP 4-bit | 1 + 3 (V5 header) | RGBA8 |
| DDS A1R5G5B5 | 2 | RGBA8 |
| TGA type 10 (RLE) 24-bit | 1 | RGBA8 |
| DDS A8L8 (luminance flag, 16-bit) | 1 | RGBA8 |
| DDS `CxV8U8` (D3DFORMAT 117) | 1 | RGBA8, biased, derived Z |

Not found in the corpus: BMP RLE8/RLE4, 1-bit BMP, OS/2 core headers, top-down BMPs, colour-mapped or greyscale TGA, DX10 DDS headers, volume textures. The decoder still supports all of them; a DX10 header is read for R8G8B8A8 and BC1–BC5 and BC7 (sRGB or not), the formats texture packs use, and other DXGI formats are refused. The synthetic tests cover RLE8 and top-down BMPs.

Mip levels: 112,296 DDS files have one level. The rest carry full or partial chains of 4 to 11 levels. Every DDS mip count in the corpus matches the data present, so the clamp described below never applies. 92 textures have dimensions that are not powers of two, the largest 1108 pixels on a side. The largest texture overall is 1024×1024.

### Failures (bad data)

| Members | Contents |
|---|---|
| 16 `.dds` members in `arelis`, `eastsepulcher`, `sepulcher`, `westsepulcher`, `morellcastle` (`concrete_bump_n.dds`, `metal01_n.dds`, `coverage_tree.dds`, …) | 60–100 bytes of `EQLOD` text: an LOD file stored under a texture name |
| `sarithcity.eqg` `sarith_wave_02_c.dds` | a Photoshop document (`8BPS`) |

They are reported and skipped.

## DDS

Layout, from Microsoft "DDS_HEADER" / "DDS_PIXELFORMAT": `"DDS "`, a 124-byte header (height at +12, width at +16, mip count at +28, pixel format at +76: size 32, flags, FourCC, bit count, R/G/B/A masks, caps2 at +112), then the levels, largest first. Each level is tightly packed.

- DXT1/DXT2/DXT3/DXT4/DXT5 are kept compressed as BC1/BC2/BC2/BC3/BC3 (DXT2 and DXT4 are the premultiplied variants; none occur). `ATI1`/`BC4U` and `ATI2`/`BC5U` are kept as BC4 and BC5, and a DX10 header's BC formats likewise (BC7 too); none occur in the game's files, they are for texture packs. A level holds `ceil(w/4)·ceil(h/4)` blocks, each 8 bytes (BC1, BC4) or 16 bytes, so sizes that are not multiples of 4 are rounded up to whole blocks.
- The mip count is `max(1, dwMipMapCount)`, clamped to the length of the full chain (`bit_width(max(w,h))`). Levels the data does not completely hold are dropped. A truncated level 0 is an error.
- Uncompressed formats (`DDPF_RGB`, `DDPF_LUMINANCE`, `DDPF_ALPHA`, 8/16/24/32 bits) are converted through their channel masks to RGBA8. Each field is scaled to 0–255 with rounding. The alpha mask is used only when `DDPF_ALPHAPIXELS`/`DDPF_ALPHA` is set, so X8R8G8B8 gets alpha 255. Luminance copies its value to R, G and B.
- Signed normal-map formats whose D3DFORMAT number is stored in the FourCC field: `Q8W8V8U8` (63) and `CxV8U8` (117). Each signed byte is stored plus 128 (`x ^ 0x80`), the usual unsigned normal-map encoding. For CxV8U8, B = `128 + 127·sqrt(1 − u² − v²)`, following the D3D9 definition of the format. The Q8W8V8U8 files hold real signed data (for example U=0x30, V=0x40, W=0x7d). The one CxV8U8 file (`clz.eqg` `wtr_zero_n.dds`) is all `0x80` bytes. That reads as −1 under the signed interpretation and decodes to (0,0,128). Its name suggests the author meant "zero" in a biased encoding.
- Cube maps (`DDSCAPS2_CUBEMAP`): `decode_texture` returns only the first face (+X) and its mip chain; `Image` has no faces. `decode_cube_as_octahedral` reads all six faces into one octahedral RGBA8 image. For volume textures only slice 0 of level 0 is returned. Cube maps occur only in water/environment textures (`water_e.dds`, `water_noswap_e.dds`, …).

### BC decoding (`to_rgba8`)

From Microsoft "Opaque and 1-Bit Alpha Textures" (D3D9) and "Texture Block Compression in Direct3D 11":

- Colour block: two RGB565 endpoints (`u16` little-endian), then 32 bits of 2-bit selectors, texel *t* (row-major within the 4×4 block) at bits `2t`. Endpoints expand by bit replication (`r<<3 | r>>2`, `g<<2 | g>>4`).
- BC1: if `c0 > c1` (compared as unsigned 16-bit integers) the palette is c0, c1, (2c0+c1)/3, (c0+2c1)/3, all opaque. Otherwise (**including `c0 == c1`**) it is c0, c1, (c0+c1)/2, and selector 3 is transparent black (0,0,0,0). This punch-through alpha matters here: 51 DXT1 members in 23 EQG archives with transparent texels (as OpenEQ found). The DDS alpha flag on all of them is clear (FOURCC only), so the rule must not depend on that flag. Our test `Real textures from an EQG archive decode` checks `broodlands.eqg` `swmp_canopy_trim.dds`: 22,977 transparent texels, the count OpenEQ reports.
- BC2/BC3 colour blocks always use the four-colour palette, whatever the endpoint order.
- BC2 alpha: 64 bits, 4 bits per texel (low nibble first), value × 17.
- BC3 alpha: endpoints a0 and a1, then 48 bits of 3-bit selectors. If `a0 > a1` the palette is a0, a1 and six interpolants `((7−i)a0 + i·a1)/7`. Otherwise it is a0, a1, four interpolants `((5−i)a0 + i·a1)/5`, then 0 and 255.
- BC4 is one BC3 alpha block (red), BC5 two (red, green); BC7 blocks are decoded by bcdec (THIRD_PARTY.md).
- Interpolation uses integer division on the expanded 8-bit values. GPUs may differ by about one step. The renderer uploads BCn directly when the device supports it, so this CPU path serves GPUs without BC support and tools.

## BMP

From the Microsoft BITMAPFILEHEADER / BITMAPINFOHEADER / BITMAPV4HEADER / BITMAPV5HEADER documentation:

- File header: `"BM"`, size, reserved, pixel-data offset (+10). Then the info header, whose size is 12 (core), 40, 52, 56, 108 or 124.
- Palette (1/4/8-bit): `biClrUsed` entries (0 means 2^bits) of B,G,R,reserved (B,G,R for core headers), right after the info header. The reserved byte is not alpha. Entries that would overlap the pixel data are ignored. Missing entries are black.
- `TextureOptions::palette_index0_transparent` gives palette index 0 alpha 0. Masked classic materials use this. The WLD material decides when to set it.
- Rows run bottom-up unless the height is negative. Each row is padded to 4 bytes. The final row's padding may be missing.
- 16-bit BI_RGB is X1R5G5B5. 24-bit is B,G,R. 32-bit BI_RGB is B,G,R,x, read with alpha 255. BI_BITFIELDS uses the masks: after a 40-byte header, or inside V2+ headers, with an alpha mask only from V3+ headers or BI_ALPHABITFIELDS.
- RLE8/RLE4 (bottom-up only): encoded runs, and escapes 0 (end of line), 1 (end of bitmap), 2 (delta) and ≥3 (absolute run, padded to 16 bits). Pixels never written get palette index 0. A bitmap larger than 128 pixels per byte of its data (plus 512 bytes) is refused: runs fill at most 255 pixels per two bytes, and only deltas or an early end of bitmap could leave more unwritten (no RLE bitmap is in the corpus).
- BMP images have a single level. Mips are generated by the renderer.

## TGA

From the Truevision TGA 2.0 specification. Types 1/9 (colour-mapped, raw/RLE), 2/10 (truecolour) and 3/11 (greyscale) are supported. Truecolour is B,G,R[,A] or 16-bit A1R5G5B5. Alpha is used only when the descriptor's attribute bits (low nibble) are non-zero. Descriptor bit 5 means rows run top-down; bit 4 means right-to-left.

In the corpus, `sky.s3d` holds 13 TGA files (Planes of Power and Bastion of Thunder sky layers, `glare.tga`). The 32-bit ones declare 8 alpha bits, but every alpha value is 0 (`pofiresky1.tga`, for example). Their blending must therefore ignore texture alpha or be additive. That depends on the sky material, not on this decoder. `bgo.eqg`, `djm.eqg` and `wizardhatpointytraditional.eqg` hold one stray TGA each.

## Robustness

All reads go through `ByteReader`, which throws on a read past the end. `decode_texture` catches every exception and returns `nullopt` with the message. The decoder refuses dimensions over 16384 and more than 8192 × 8192 texels in all (as the PNG reader does; a 16384² BC1 file would decode to 1 GiB of RGBA, and the game's largest texture is 1108²), and checks that the data can cover the image before it allocates, so a forged header cannot make it allocate a huge buffer. The decoder was fuzzed with truncations and byte mutations of real DDS/BMP/TGA files under ASan and UBSan, with no findings.

## Sources

- Microsoft Learn: "DDS file layout", "DDS_HEADER", "DDS_PIXELFORMAT", "Opaque and 1-Bit Alpha Textures (Direct3D 9)", "Texture Block Compression in Direct3D 11", "D3DFORMAT", "BITMAPINFOHEADER", "BITMAPV5HEADER", "Bitmap Compression" (RLE).
- Truevision TGA File Format Specification, version 2.0.
- OpenEQ's DDS BC1 alpha survey and its `texture.rs` (sniffing magic rather than names). No OpenEQ code was ported.
