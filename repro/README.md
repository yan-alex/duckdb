# Build
```
make relassert && 
c++ -std=c++17 -O0 -g -I src/include \
  repro/native_repro.cpp \
  build/relassert/src/libduckdb.dylib \
  -Wl,-rpath,"$(pwd)/build/relassert/src" \
  -o repro/native_repro
```

# Run
```
repro/native_repro
```
