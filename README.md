# max-lilypond

## Building

```
# Configure
cmake -B build           \
    -S .                 \
    -G Ninja             \
    -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build
```

This assembles a complete Max package under `build/package` (with the external
in `build/package/externals/`) rather than writing into the source tree.
Override with `-DMAX_SDK_PACKAGE_OUT_OF_TREE=OFF` to build into the source tree
instead.
