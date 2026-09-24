# Third party code

Vendored sources and their licences. Everything else in this repository is under the MIT licence in `LICENSE`.

## zstd decompressor — `loader/third_party/zstddeclib.c`

Single file decompression build of [zstd](https://github.com/facebook/zstd) by Meta Platforms, dual licensed
BSD 3-Clause and GPLv2. Used here under the BSD 3-Clause licence. The pak entries of RE Engine are zstd
compressed, so the loader needs a decompressor to read them.

## miniz — `loader/third_party/miniz.{c,h}`

[miniz](https://github.com/richgel999/miniz) by Rich Geldreich and contributors, MIT licence. Provides the
deflate and zip handling used when a costume mod is delivered as an archive.

## REFramework plugin API — `plugin/include/reframework/API.{h,hpp}`

Headers from [REFramework](https://github.com/praydog/REFramework) by praydog, MIT licence, taken at tag
v1.5.8. They define the C interface a native plugin uses to reach the game through REFramework.

## AMD AGS

The loader's proxy forwards every export to `amd_ags_x64_real.dll`, which is the copy of
[AMD AGS](https://github.com/GPUOpen-LibrariesAndSDKs/AGS_SDK) shipped with the game, MIT licence. No AMD code
is included in this repository.
