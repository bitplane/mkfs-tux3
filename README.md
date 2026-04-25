# tux3-standalone

Standalone userspace Tux3 image builder extracted from the `origin/hirofumi-user`
branch of the Linux Tux3 tree.

## Build

```sh
make
```

The build produces:

- `mkfs.tux3` - formats a Tux3 image and optionally populates it from a host path
- `libtux3.a` - copied Tux3 userspace/core code as a static library
- `libklib.a` - Linux compatibility shims used by the copied code

## Usage

```sh
./mkfs.tux3 -s 64M image.tux3 rootdir
./mkfs.tux3 -b 4096 -s 128M image.tux3
```

`rootdir` is optional. When provided, regular files, directories, symlinks, and
basic special nodes are copied into the new image.
