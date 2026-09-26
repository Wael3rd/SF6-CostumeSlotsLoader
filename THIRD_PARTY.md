# Third party code

Vendored sources and their licences. Everything else in this repository is under the MIT licence in `LICENSE`.

## zstd decompressor — `loader/third_party/zstddeclib.c`

Single file decompression build of [zstd](https://github.com/facebook/zstd) by Meta Platforms, dual licensed
BSD 3-Clause and GPLv2. Used here under the BSD 3-Clause licence. The pak entries of RE Engine are zstd
compressed, so the loader needs a decompressor to read them.

## miniz — `loader/third_party/miniz.{c,h}`

[miniz](https://github.com/richgel999/miniz) by Rich Geldreich and contributors, MIT licence. Provides the
deflate and zip handling used when a costume mod is delivered as an archive.

## LZMA SDK — `loader/third_party/lzma/`

The 7z decoder of the [LZMA SDK](https://www.7-zip.org/sdk.html) 23.01 by Igor Pavlov, **public domain**.
Only the files the decoder needs are included, unmodified. It reads `.7z` costume archives.

## UnRAR — `loader/third_party/unrar/`

UnRAR 7.3.1 by Alexander Roshal, from https://www.rarlab.com/rar_add.htm , under its own licence
(`loader/third_party/unrar/license.txt`). It reads `.rar` costume archives. Two guarded changes make it
safe to run inside `DllMain` (no worker threads, no on-demand `Crypt32.dll`); they are described in
`loader/third_party/unrar/SF6_PATCHES.txt`. As its licence requires:

> UnRAR source code may be used in any software to handle RAR archives without limitations free of
> charge, but cannot be used to develop RAR (WinRAR) compatible archiver and to re-create RAR
> compression algorithm, which is proprietary. Distribution of modified UnRAR source code in separate
> form or as a part of other software is permitted, provided that full text of this paragraph, starting
> from "UnRAR source code" words, is included in license, or in documentation if license is not
> available, and in source code comments of resulting package.

This code is not under the MIT licence of the rest of the repository.

## REFramework plugin API — `plugin/include/reframework/API.{h,hpp}`

Headers from [REFramework](https://github.com/praydog/REFramework) by praydog, MIT licence, taken at tag
v1.5.8. They define the C interface a native plugin uses to reach the game through REFramework.

## AMD AGS

The loader's proxy forwards every export to `amd_ags_x64_real.dll`, which is the copy of
[AMD AGS](https://github.com/GPUOpen-LibrariesAndSDKs/AGS_SDK) shipped with the game, MIT licence. No AMD code
is included in this repository.
