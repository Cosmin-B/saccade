# Memory and resolution

Resolution, queue depth, tensor shape, and retained history are separate memory costs.
Frame rate changes bandwidth and compute pressure. It does not increase steady-state
memory unless more frames are kept in flight.

## Captured frames

For a packed image, one frame costs:

```text
frame bytes = width * height * bytes per pixel
resident capture bytes = frame bytes * queue depth
```

BGRA8 and RGBA8 use four bytes per pixel. Common full-frame costs are:

| Resolution | Pixels | One frame | Two frames | Three frames |
|---|---:|---:|---:|---:|
| 1920 x 1080 | 2,073,600 | 7.91 MiB | 15.82 MiB | 23.73 MiB |
| 2560 x 1440 | 3,686,400 | 14.06 MiB | 28.12 MiB | 42.19 MiB |
| 3840 x 2160 | 8,294,400 | 31.64 MiB | 63.28 MiB | 94.92 MiB |
| 5120 x 2880 | 14,745,600 | 56.25 MiB | 112.50 MiB | 168.75 MiB |
| 7680 x 4320 | 33,177,600 | 126.56 MiB | 253.12 MiB | 379.69 MiB |

Row padding, native-surface metadata, and driver residency add to these values.
Imported GPU surfaces count under `device_imported`; provider-created textures count
under `device_owned`. The same bytes must not be reported in both categories.

## Frame rate and bandwidth

Reading each packed frame once gives a lower bound:

```text
bytes per second = frame bytes * frames per second
```

At 4K BGRA8, that is about 0.95 GiB/s at 30 Hz and 1.85 GiB/s at 60 Hz. At 8K it is
about 3.71 GiB/s at 30 Hz and 7.42 GiB/s at 60 Hz. Copies, color conversion, cache
misses, and intermediate tensors add traffic. Zero-copy import avoids a frame copy but
does not make the surface consume zero memory or bandwidth.

The 120 Hz interaction path does not read a full image on every tick. It consumes the
latest immutable scene and small input state. Capture and neural work advance on the
independent scene clock.

## Tensor memory

A dense tensor costs:

```text
tensor bytes = width * height * channels * bytes per element
```

For a feature pyramid, sum that expression for every level and multiply by the number
of simultaneous activation sets. FP32 uses four bytes per element, FP16 uses two, and
INT8 uses one. Framework workspaces and compiled graphs are reported separately as
`framework_opaque` when their exact allocation cannot be attributed.

Full-scope inference does not require a native-resolution tensor throughout the model.
A provider may tile, downsample, or build compact features, but every visible pixel must
still be able to affect the qualified output.

## Time and retained history

A newest-frame mailbox has constant memory. When a new frame arrives, it replaces the
pending frame instead of extending a queue. With one frame in inference and one pending,
steady-state frame count is two regardless of how long Saccade runs.

Recording is different:

```text
history bytes = frame bytes * frames per second * retained seconds
```

Ten seconds of uncompressed 4K BGRA8 at 30 Hz would require about 9.27 GiB. Normal
operation therefore keeps no screenshot history. Test recordings and profiler captures
belong outside the product source tree and are loaded explicitly.

## Accounting contract

Every provider reports:

- committed and reserved host bytes;
- imported and provider-owned device bytes;
- opaque framework residency;
- bytes copied across ownership domains;
- a high-water total.

Reports are sampled at defined lifecycle points. Release gates compare the provider
report with operating-system measurements and record any unavoidable opaque difference.
The deterministic providers return configured counters. The scalar CPU provider reports
its fixed storage and cumulative serialized output bytes.
