# ITCH 5.0 order book

A low-latency, single-instrument ITCH 5.0 order book. The parser maps a binary
feed and decodes messages directly from its mapped bytes. Orders link directly
within price levels; price ticks index contiguous level storage.

## Build and validate

```sh
cmake -S . -B build -DITCH_BUILD_TESTS=ON -DITCH_BUILD_BENCHMARKS=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/itch_benchmark
```

For Linux core isolation and counters:

```sh
taskset -c 2 ./build/itch_benchmark
perf stat -e cache-misses,cache-references taskset -c 2 ./build/itch_benchmark
perf c2c record -- taskset -c 2 ./build/itch_benchmark
```

Download a public BX or PSX sample from Nasdaq's ITCH archive, then pass its
path to `itch::MappedFile` and `itch::Parser`. The feed remains on disk and is
never copied into a parsing buffer.
