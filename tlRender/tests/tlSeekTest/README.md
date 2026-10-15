# Seek and cancellation regression checks

Enable `TLRENDER_SEEK_TESTS=ON` when configuring tlRender, then build
`tlSeekTest`. This option is independent of the legacy test suite and its
checks remain enabled in Release builds.

Run `ctest -R tlSeekScheduling --output-on-failure` from the build directory
for deterministic tests of pending/in-flight cancellation, selective
cancellation, release of an occupied request slot, and forward/reverse
playhead priority. The test reader deliberately keeps an old future
unresolved, so slot release cannot pass merely by waiting for old work.

With FFmpeg enabled, pass a 30 fps movie containing at least 900 frames:

```sh
tests/tlSeekTest/tlSeekTest /absolute/path/to/movie.mp4
```

For example, generate a fixture with FFmpeg:

```sh
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -t 30 \
  -c:v libx264 -g 300 -keyint_min 300 -sc_threshold 0 -bf 3 fixture.mp4
```

The movie checks cancel queued reads, verify known metadata does not wait
behind decoding, compare target RGB pixels against an independent reader,
exercise forward/backward frame steps and rapid seeks, then resume and pause
playback. A movie with audio additionally checks that audio samples after
cancellation match independent decoding. Movie arguments are optional so the
deterministic tests do not require external fixtures or codec support.

`tlSeekBenchmark` uses the same 900-frame fixture to report six core seek
latencies as JSON lines:

```sh
tests/tlSeekTest/tlSeekBenchmark /absolute/path/to/movie.mp4 forward 5 0.5
tests/tlSeekTest/tlSeekBenchmark /absolute/path/to/movie.mp4 stop 5 0.5
```

The last arguments are the read-ahead and read-behind windows in seconds.
It waits for the target image, then lets the player run for 40 ms before the
next seek. This measures decoded image availability, not on-screen latency.
Compile both versions being compared with the same build settings; an old
installed archive is not a reliable baseline for the current source.
