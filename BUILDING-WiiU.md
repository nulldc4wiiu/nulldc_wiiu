# nullDC (Wii U): building

```
docker build -t nulldc-wiiu wiiu
docker run --rm -v "$PWD":/src nulldc-wiiu wiiu/build-wiiu.sh
```

Run both from the repo root. The build files (`Dockerfile`, `build-wiiu.sh`) live in `wiiu/`. Output:
`build-wiiu/nulldc.wuhb`. Install it as described in [Installing](README-WiiU.md#installing).

The build also writes `wiiu/release/`, the files to publish: the wuhb, `LICENSE-nullDC4Wii.txt`,
`THIRD-PARTY-NOTICES.txt` and `SHA256SUMS`. Both license files are also bundled inside the wuhb.
`wiiu/make-notices.sh` generates them; it fails the build if a license text is missing.

The shaders are checked in compiled (`wiiu/shaders/*.gsh`). After editing a `.vert` or `.frag`, rebuild them with
`wiiu/shaders/build.sh /path/to/glslcompiler` (CafeGLSL).
