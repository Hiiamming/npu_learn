# Hexagon HVX Tutorial: From Intrinsics to a 3×3 Max Filter

> Practical notes on Qualcomm Hexagon HVX (v68, 128-byte vectors), collected while learning intrinsics, debugging C code, validating results against a scalar implementation, and optimizing performance with `hexagon-sim`.
>
> **Current status:** A 3×3 max filter with *replicate-border* handling, tested on a 3 × 300 grayscale image with `STRIDE = 384`. The padded implementation processes three full HVX blocks per row and **matches the C reference for all 900 real pixels** in the documented test case.

## Table of Contents

1. [Environment and How to Run](#1-environment-and-how-to-run)
2. [HVX Concepts Covered](#2-hvx-concepts-covered)
3. [3×3 Max Filter: From Algorithm to HVX](#3-33-max-filter-from-algorithm-to-hvx)
4. [C Reference and Regression Testing](#4-c-reference-and-regression-testing)
5. [Debugging Pitfalls and Lessons Learned](#5-debugging-pitfalls-and-lessons-learned)
6. [Benchmarking, Profiling, and Results](#6-benchmarking-profiling-and-results)
7. [Checklist Before Further Optimization](#7-checklist-before-further-optimization)
8. [Next Steps](#8-next-steps)

---

## 1. Environment and How to Run

### 1.1. Tested setup

| Component | Configuration |
| --- | --- |
| Hexagon architecture | `v68` |
| Simulator configuration | `v68n_1024` |
| HVX vector length | `128 bytes = 1024 bits` |
| Language | C with Hexagon HVX intrinsics |
| Compiler | `hexagon-clang` |
| Optimization level | `-O3` |
| Simulator | `hexagon-sim` |
| Profiler | `hexagon-gprof` |
| Example source file | `main.c` |

Required headers:

```c
#include <stdio.h>
#include <stdint.h>
#include <hexagon_types.h>
#include <hvx_hexagon_protos.h>
```

Image dimensions and vector width used in the current exercise:

```c
#define WIDTH  300   // Actual pixels per row
#define HEIGHT 3     // Actual number of rows
#define STRIDE 384   // Allocated bytes per row, including padding
#define VL     128   // Bytes per HVX_Vector
```

Compile from the directory containing `main.c`:

```bash
hexagon-clang -mv68 -mhvx -mhvx-length=128b -O3 -Wall main.c -o main
```

Compile with debug symbols for more readable profiling reports:

```bash
hexagon-clang -mv68 -mhvx -mhvx-length=128b -O3 -g -Wall main.c -o main
```

Run the program with simulator timing:

```bash
hexagon-sim --timing ./main
```

Collect and inspect a function-level profile:

```bash
hexagon-sim --timing --profile ./main
ls -l gmon*
hexagon-gprof main gmon.t_0
```

> In the tested environment, profiling produced `gmon.t_0`. If that file does not exist, inspect the generated `gmon.*` files. Remove stale profiles when changing builds so that an old report is not mistaken for a new run.

If a `Makefile` is already configured, use `make run`. A minimal example:

```makefile
HEXAGON_CLANG := hexagon-clang
CFLAGS := -mv68 -mhvx -mhvx-length=128b -O3 -g -Wall

.PHONY: all run profile clean
all: main

main: main.c
	$(HEXAGON_CLANG) $(CFLAGS) $< -o $@

run: main
	hexagon-sim --timing ./main

profile: main
	hexagon-sim --timing --profile ./main
	hexagon-gprof main gmon.t_0

clean:
	rm -f main gmon.*
```

### 1.2. Verified output

For the test data in Section 4:

```text
Checksum = 206234
out[1][127] = 240
Reference checksum = 206234
PASS! All 900 pixels match.
```

This is the **observed result on the simulator** for `HEIGHT = 3` and `WIDTH = 300`. It is not proof that the implementation works for every image size, stride, or input distribution.

---

## 2. HVX Concepts Covered

### 2.1. Vectors, registers, and memory

- An `HVX_Vector` is **128 bytes** long. For an 8-bit grayscale image, one vector represents **128 pixels**.
- A vector is a register-level data type; a `uint8_t` array lives in memory. Loading and storing are separate operations.
- `Q6_V_vzero()` creates a vector filled with zeros.
- Typical vector load/store operations in this exercise:

```c
HVX_Vector v = *(const HVX_Vector *)(src + i);
*(HVX_Vector *)(dst + i) = v;
```

**Important:** `src + i` and `dst + i` must meet the alignment requirements of these vector accesses. In this exercise, each row starts on a 128-byte boundary and `i` takes the values `0`, `128`, and `256`.

### 2.2. Reading intrinsic names

A useful naming heuristic, although not every intrinsic follows it perfectly:

```text
Q6_<output type>_<operation>_<input types>[_modifier]
```

Intrinsics encountered during the exercises:

| Intrinsic | Purpose |
| --- | --- |
| `Q6_V_vzero()` | Create a zero vector |
| `Q6_Vb_vsplat_R(x)` | Broadcast a byte pattern across the vector |
| `Q6_Vub_vadd_VubVub_sat(a,b)` | Add unsigned bytes with **saturation** at 255 |
| `Q6_Q_vcmp_gt_VubVub(a,b)` | Compare unsigned bytes lane by lane; produce a predicate mask |
| `Q6_V_vmux_QVV(mask,a,b)` | Select values from `a` or `b` according to a mask |
| `Q6_Wuh_vunpack_Vub(v)` | Widen unsigned bytes to unsigned halfwords, producing a vector pair |
| `Q6_Vub_vpack_VhVh_sat(...)` | Pack halfwords into unsigned bytes with saturation |
| `Q6_V_vlalign_VVI(...)` | Align/combine vectors to obtain left-neighbor pixels |
| `Q6_V_valign_VVI(...)` | Align/combine vectors to obtain right-neighbor pixels |
| `Q6_Vub_vmax_VubVub(a,b)` | Lane-wise unsigned-byte maximum |
| `Q6_Vb_vdeal_Vb(v)` | Reorder/deinterleave even and odd elements |
| `Q6_Vb_vshuff_Vb(v)` | Shuffle/interleave elements, complementing `vdeal` in the studied examples |

If the project includes `HVX_Intrinsics_Cheatsheet_V79.md`, it can be used as an additional reference. **Version warning:** the cheat sheet concerns v79, whereas these exercises compile for **v68**. Do not assume every v79 intrinsic or feature is supported on v68.

### 2.3. Saturation vs. overflow/wraparound

Consider adding two bytes:

```text
250 + 20 = 270
```

- **Saturating unsigned-byte addition:** result is `255`.
- **8-bit wrapping addition (modulo 256):** result is `14`.

Wraparound is not saturation. For operations such as increasing image brightness, choosing the wrong addition changes the resulting pixel values.

### 2.4. Comparisons, masks, mux, and widening

- `vcmp_gt` creates a lane-wise predicate mask; `vmux` uses that mask to choose between corresponding elements of two vectors.
- `vunpack` widens bytes to halfwords for intermediate computations; `vpack...sat` narrows results while clamping out-of-range values.
- In the C toolchain used here, **`v[0]` on an `HVX_Vector` does not mean byte/pixel 0**. The indexed element belongs to the compiler's vector representation; indexing once was observed to return a value formed from multiple bytes.
- To inspect individual bytes reliably, store the vector into a byte array:

```c
uint8_t debug[128] __attribute__((aligned(128)));
*(HVX_Vector *)debug = v;
printf("pixel 14 = %u\n", (unsigned)debug[14]);
```

### 2.5. `valign`, `vlalign`, `vdeal`, and `vshuff`

The key data-movement patterns explored:

- `valign(next, cur, 1)` constructs a vector of **right neighbors**: starting at `cur[1]` and ending with the first byte of `next`.
- `vlalign(cur, prev, 1)` constructs a vector of **left neighbors**: starting with the last byte of `prev`, followed by `cur[0]`, etc.
- `vdeal` and `vshuff` help rearrange interleaved data, such as even/odd elements or stereo channels.
- Alignment and shuffle instructions do **not** understand image borders. The programmer must supply the correct border values.

---

## 3. 3×3 Max Filter: From Algorithm to HVX

### 3.1. Definition

For each input pixel `input[y][x]`, produce the **maximum value in its surrounding 3×3 neighborhood**:

```text
+---------+---------+---------+
| y-1,x-1 | y-1,x   | y-1,x+1 |
+---------+---------+---------+
| y,x-1   | y,x     | y,x+1   |
+---------+---------+---------+
| y+1,x-1 | y+1,x   | y+1,x+1 |
+---------+---------+---------+
```

**A crucial misunderstanding resolved during the tutorial:**

- **3×3** describes the nine input pixels contributing to **one output pixel**.
- **128 bytes** describes how many output pixels an HVX vector can process **in parallel**.
- These are different dimensions of the problem. A 128-byte vector does not limit the filter to a 128×1 neighborhood.

### 3.2. Split the filter into two passes

The maximum over a 3×3 neighborhood can be computed as two separable passes:

```text
Input  ── horizontal max 3×1 ──> tmp_output
                                    │
                                    ▼
                              vertical max 3×1
                                    │
                                    ▼
                                  Output
```

Horizontal max at position `x`:

```c
h[x] = max(src[x - 1], src[x], src[x + 1]);
```

Vertical max at position `(y, x)`:

```c
out[y][x] = max(tmp[top_y][x], tmp[y][x], tmp[bot_y][x]);
```

These expressions describe the algorithm. At the image boundaries, adjust the indices before accessing memory.

### 3.3. Horizontal HVX: left, center, and right vectors

Core logic from the implementation used in the exercises:

```c
uint8_t left_edge  = src[i == 0 ? 0 : i - 1];
uint8_t right_edge = src[i + VL < n ? i + VL : n - 1];

HVX_Vector cur  = *(const HVX_Vector *)(src + i);
HVX_Vector prev = Q6_Vb_vsplat_R(left_edge);
HVX_Vector next = Q6_Vb_vsplat_R(right_edge);

HVX_Vector left  = Q6_V_vlalign_VVI(cur, prev, 1);
HVX_Vector right = Q6_V_valign_VVI(next, cur, 1);

HVX_Vector mx = Q6_Vub_vmax_VubVub(left, cur);
mx = Q6_Vub_vmax_VubVub(mx, right);

*(HVX_Vector *)(dst + i) = mx;
```

For each block, `left_edge` is the byte immediately before the block (or the first pixel at the left image boundary), and `right_edge` is the byte immediately after the block (or the last pixel at the right boundary).

### 3.4. Vertical HVX: top and bottom borders

With *replicate-border* handling:

```c
int top_y = (y == 0) ? 0 : y - 1;
int bot_y = (y == HEIGHT - 1) ? y : y + 1;
```

| Output row | `top_y` | `mid_y` | `bot_y` |
| --- | --- | --- | --- |
| Row 0 | 0 | 0 | 1 |
| Row 1 | 0 | 1 | 2 |
| Row 2 | 1 | 2 | 2 |

For `HEIGHT = 3`, the first output row repeats input row 0 as its missing top neighbor; the last output row repeats input row 2 as its missing bottom neighbor.

Load three vectors at the same horizontal offset `x`, then compute lane-wise maxima:

```c
HVX_Vector top = *(const HVX_Vector *)(input[top_y] + x);
HVX_Vector mid = *(const HVX_Vector *)(input[y] + x);
HVX_Vector bot = *(const HVX_Vector *)(input[bot_y] + x);

HVX_Vector mx = Q6_Vub_vmax_VubVub(top, mid);
mx = Q6_Vub_vmax_VubVub(mx, bot);

*(HVX_Vector *)(output[y] + x) = mx;
```

### 3.5. `WIDTH` vs. `STRIDE`: padding away the scalar tail

Current configuration:

```c
WIDTH  = 300;  // 300 real pixels
STRIDE = 384;  // 3 × 128 bytes per row
VL     = 128;
```

**Initial implementation:** two 128-byte HVX blocks (256 pixels), followed by a **44-pixel scalar tail**.

**Optimized implementation:** allocate 384 bytes per row and fill the padding with copies of the last real pixel.

```text
Block 0: [  0..127]  — real pixels
Block 1: [128..255]  — real pixels
Block 2: [256..383]  — 44 real pixels + 84 padding bytes
                    ^
            real pixels end at 299
```

Fill the padding **after all real pixels have been initialized**:

```c
for (int y = 0; y < HEIGHT; y++) {
    for (int x = WIDTH; x < STRIDE; x++) {
        input[y][x] = input[y][WIDTH - 1];
    }
}
```

Thus `input[y][300]` through `input[y][383]` equal `input[y][299]`.

The pipeline calls become:

```c
__attribute__((noinline))
static void run_hvx_pipeline(void)
{
    for (int y = 0; y < HEIGHT; y++) {
        max_filter_hvx(input[y], tmp_output[y], STRIDE);
    }

    vertical_max_filter(tmp_output, output, STRIDE);
}
```

**Why didn't the two kernels need to be rewritten?** They already use the `n` and `width` parameters. Passing `384` instead of `300` allows the HVX loops to process three complete blocks, leaving no scalar tail.

> **Essential distinction:** `STRIDE` describes row storage, while `WIDTH` describes the actual image. Compare pixels, compute checksums, and export images using columns `0..WIDTH-1` only. Padding exists for vectorization; it is not new image content.

---

## 4. C Reference and Regression Testing

> **KEEP THIS SECTION.** The scalar reference was once deleted during optimization and had to be reconstructed. It is the straightforward correctness baseline that catches errors even when the HVX kernel does not crash.

### 4.1. Input data used for the last passing test

```c
for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
        input[y][x] =
            (uint8_t)((x * 37 + y * 83 + (x ^ y)) & 255);
    }
}

// Make image borders and vector-block boundaries easy to test.
input[0][0]   = 255;
input[1][127] = 240;
input[1][128] = 50;
input[2][255] = 220;
input[2][256] = 30;
input[2][299] = 250;

// This must run AFTER setting all real input pixels.
for (int y = 0; y < HEIGHT; y++) {
    for (int x = WIDTH; x < STRIDE; x++) {
        input[y][x] = input[y][WIDTH - 1];
    }
}
```

### 4.2. Copy-ready scalar C reference implementation

Add this near the other global buffers and functions in `main.c`:

```c
static uint8_t reference[HEIGHT][STRIDE]
    __attribute__((aligned(128)));

void max_filter_3x3_reference(
    const uint8_t src[HEIGHT][STRIDE],
    uint8_t dst[HEIGHT][STRIDE])
{
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            uint8_t best = 0;

            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int yy = y + dy;
                    int xx = x + dx;

                    if (yy < 0) yy = 0;
                    if (yy >= HEIGHT) yy = HEIGHT - 1;
                    if (xx < 0) xx = 0;
                    if (xx >= WIDTH) xx = WIDTH - 1;

                    uint8_t pixel = src[yy][xx];
                    if (pixel > best) best = pixel;
                }
            }

            dst[y][x] = best;
        }
    }
}
```

Properties of this implementation:

- Written in regular C, examining all nine neighborhood positions.
- Replicates the **actual image borders** using `WIDTH` and `HEIGHT`.
- Does not rely on padded input to compute the 900 real output pixels.
- Initializing `best = 0` is valid because input pixels are **unsigned 8-bit** values (`0..255`).

### 4.3. Compare every real pixel

Run this **after** the HVX pipeline, outside the benchmark loop:

```c
max_filter_3x3_reference(input, reference);

unsigned hvx_checksum = 0;
unsigned ref_checksum = 0;
int errors = 0;

for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
        uint8_t got = output[y][x];
        uint8_t expected = reference[y][x];

        hvx_checksum += got;
        ref_checksum += expected;

        if (got != expected) {
            if (errors < 10) {
                printf("FAIL [%d][%d]: HVX=%u REF=%u\n",
                       y, x, (unsigned)got, (unsigned)expected);
            }
            errors++;
        }
    }
}

printf("Checksum = %u\n", hvx_checksum);
printf("Reference checksum = %u\n", ref_checksum);

if (errors == 0) {
    printf("PASS! All %d pixels match.\n", WIDTH * HEIGHT);
} else {
    printf("FAIL! %d pixels differ.\n", errors);
    return 1; // Place this verification block inside main().
}
```

**Do not rely on matching checksums alone:** two different images can have the same sum. Check **every pixel**.

For the input above, scalar verification and the observed passing simulator run produced:

```text
Checksum = 206234
Reference checksum = 206234
PASS! All 900 pixels match.
```

Useful spot-check values:

| Pixel | Scalar reference |
| --- | ---: |
| `output[0][0]` | 255 |
| `output[0][1]` | 255 |
| `output[1][127]` | 240 |
| `output[1][128]` | 240 |
| `output[1][255]` | 220 |
| `output[1][256]` | 220 |
| `output[2][299]` | 250 |

Before treating the implementation as general-purpose, also test all-zero images, all-255 images, gradients, checkerboards, reproducibly randomized inputs, and different combinations of `WIDTH` and `HEIGHT`.

---

## 5. Debugging Pitfalls and Lessons Learned

### 5.1. Use `<=` for a full block, but `<` for the byte after it

```c
// Are there ENOUGH bytes for a full 128-byte HVX_Vector?
for (; i + VL <= n; i += VL) {

    // Does a byte exist IMMEDIATELY AFTER this block?
    uint8_t right_edge =
        src[i + VL < n ? i + VL : n - 1];
}
```

For `n = 256` and `i = 128`:

- `128 + 128 <= 256` is true: block `[128..255]` is valid.
- `128 + 128 < 256` is false: there is **no** `src[256]`.
- Using `<=` for the `right_edge` condition would read past the valid range.

**Rule of thumb:** "enough bytes for a 128-byte block" is not the same as "there is a 129th byte."

### 5.2. `WIDTH` is not `STRIDE`

- `WIDTH = 300`: real pixel indices are `0..299`.
- `STRIDE = 384`: allocated per-row byte indices are `0..383`.
- `384 - 300 = 84` padding bytes.
- For aligned vector loads/stores on `input[HEIGHT][STRIDE]`, each row must start at a suitable alignment boundary. Choosing a stride divisible by 128 guarantees that rows remain aligned when the base is aligned.
- Initialize padding according to the border policy **before running the kernel**.
- Only include the `WIDTH` real pixels when comparing results or computing the image checksum.

### 5.3. Vector load/store addresses must identify the right block and be aligned

Correct:

```c
*(HVX_Vector *)(output[y] + x) = result;
```

Incorrect or unsafe:

```c
*(HVX_Vector *)(dst + 2) = result;     // Not 128-byte aligned
*(HVX_Vector *)output[1][x] = result; // Treats a pixel value as an address
```

An incorrectly aligned access is not valid for the vector-pointer casts used in this code. Hardware/simulator behavior can also make it appear to access an unexpected aligned block; never depend on such behavior.

### 5.4. Correct 2D image indexing and pointer types

```c
input[y][x]       // Pixel value (byte)
input[y] + x      // Pointer to that pixel
output[1][127]    // Value at row 1, column 127
```

Do not use `output[127]` to print a pixel. That expression denotes a row, and with `HEIGHT = 3`, row 127 is out of bounds.

### 5.5. Replicate borders independently along each axis

- Pixel `(0, 0)` needs both **top** and **left** border replication.
- Pixel `(1, 0)` in a 3-row image needs **left** replication only.
- Vector-block boundaries, such as columns 127/128, are **not image boundaries**. Neighboring pixels should come from the adjacent block.
- Column 299 is the actual right edge of an image whose width is 300.

### 5.6. Scalar tails and padded vector blocks

With `n = 300`:

```text
[128 HVX][128 HVX][44 scalar]
```

With replicated padding and `n = STRIDE = 384`:

```text
[128 HVX][128 HVX][128 HVX]
```

After the vector loop, `i == 384`, so `i < n` is false and the scalar tail does **not** execute. There is no need to delete the scalar loop: it remains useful for calls without padding.

### 5.7. A 3×3 filter does not mean three pixels per vector

Each vector lane calculates an output pixel from **its own** 3×3 neighborhood. These neighborhoods overlap; vector alignment operations provide the required left and right neighbors for each lane.

### 5.8. `-O3` may eliminate benchmark work whose result is not used

One early profile reported **100 calls to `run_hvx_pipeline` but only 388 self cycles**. The likely cause was an unobserved `output`, allowing the compiler to optimize away most of the intended work.

To make each benchmark iteration's output observable to the compiler:

```c
for (int k = 0; k < REPEAT; k++) {
    run_hvx_pipeline();
    __asm__ __volatile__("" : : "m"(output) : "memory");
}
```

Outside the loop, consume the output by computing a checksum and/or comparing it with the scalar reference. This is a **benchmarking technique**, not part of the max-filter algorithm itself.

### 5.9. `noinline` helps profile functions separately; it is not a universal optimization

```c
__attribute__((noinline))
void max_filter_hvx(/* ... */);
```

- If both kernels are inlined into the pipeline, `gprof` may show only `run_hvx_pipeline`.
- `noinline` makes it easier to identify individual function costs.
- It can also change code generation and introduce call overhead. Do not attribute a measured speedup to one algorithmic change if the compilation conditions differ.

### 5.10. In-program cycle-counter reads returned zero on this simulator setup

Both of the following were tried:

```c
__builtin_readcyclecounter();
```

and:

```c
__asm__ __volatile__("%0 = C15:14" : "=r"(cycles) : : "memory");
```

Both `Start` and `End` were `0`, even with `--timing`. This **does not mean the kernel took zero cycles**.

The method that **worked in practice** was running `hexagon-sim --timing --profile` and inspecting `hexagon-gprof`, along with the simulator's total `Pcycles`.

### 5.11. Whole-program `Pcycles` is not cycles per kernel

The `Pcycles` line at the end of a simulator run includes startup, `printf`, memory setup, kernel execution, cleanup, and other work. In initial profiles, `hexagon_start_init` consumed a substantial amount of cycles.

For function-level analysis, inspect **self cycles**, **call counts**, and the **call graph**. Use self-plus-children costs when the function's work happens in other functions.

---

## 6. Benchmarking, Profiling, and Results

### 6.1. Reproducible benchmark workflow

1. Build with the same architecture (`-mv68`, `-mhvx`, `-mhvx-length=128b`) and optimization level (`-O3`).
2. Initialize the same input data and use identically aligned buffers.
3. When testing a padded version, fill replicate padding after all real pixels are initialized.
4. Warm up three times, then run 100 benchmark iterations, ensuring their results cannot be optimized away.
5. Record horizontal, vertical, and pipeline self cycles separately.
6. After the benchmark, use the C reference to verify **all 900 real output pixels**.
7. Keep compiler settings and profiler configuration as consistent as possible between versions.

Benchmark pattern used:

```c
#define REPEAT 100

for (int k = 0; k < 3; k++) {
    run_hvx_pipeline();
    __asm__ __volatile__("" : : "m"(output) : "memory");
}

for (int k = 0; k < REPEAT; k++) {
    run_hvx_pipeline();
    __asm__ __volatile__("" : : "m"(output) : "memory");
}

// After benchmarking: checksum and verification against C reference.
```

### 6.2. Observed profiling results

These numbers came from the 3×300 exercise, with **103 pipeline calls** (3 warm-up + 100 benchmark iterations). Both horizontal and vertical kernels were marked `noinline` for the measurements shown below.

| Metric | Before: 2 HVX blocks + scalar tail | After: 3 padded HVX blocks |
| --- | ---: | ---: |
| `max_filter_hvx`: self cycles | 114,822 | **20,429** |
| `vertical_max_filter`: self cycles | 112,321 | **10,765** |
| `run_hvx_pipeline`: self cycles | 1,403 | **2,427** |
| **Total self cycles for these functions** | **228,546** | **33,621** |
| Average over 103 calls | ~2,219 cycles/image | **~326 cycles/image** |
| Whole-program `Pcycles` | 302,275 | 108,507 |

Speedup based on the **combined self cycles of the three functions**:

```text
Speedup ≈ 228546 / 33621 ≈ 6.80×
```

**How to interpret these results:**

- This is an **observed simulator speedup for the tested configuration and data**, not a promise for every device or image.
- Whole-program `Pcycles` do not fall by exactly 6.80× because they include costs outside the pipeline.
- The speedup may involve differences in `-O3` code generation as well as elimination of the scalar tail; it should not be attributed exclusively to padding.
- `noinline` was consistently used in the two listed profiles, but measurements should be repeated whenever benchmark conditions change.
- The averages include the three warm-up calls; they are not measurements of the benchmark-only interval.

### 6.3. Reading the profiler output

Example for the padded implementation:

```text
max_filter_hvx       calls=309   self cycles=20429
vertical_max_filter  calls=103   self cycles=10765
run_hvx_pipeline     calls=103   self cycles=2427
```

Why are there 309 horizontal calls?

```text
3 rows/image × 103 pipeline executions = 309 horizontal calls
1 vertical call/image × 103 executions  = 103 vertical calls
```

**Do not compare** horizontal and vertical self cycles per call without accounting for the fact that horizontal runs three times per image. To compare their **per-image** costs, divide each function's total cycles by `103`.

---

## 7. Checklist Before Further Optimization

- [x] Understand that 128 bytes is the vector's parallel processing width, not the 3×3 filter size.
- [x] Implement horizontal max with `vlalign`, `valign`, and two `vmax` operations.
- [x] Implement vertical max across three rows with replicate borders.
- [x] Align rows on 128-byte boundaries using `STRIDE = 384`.
- [x] Replace the scalar tail with a third full 128-byte HVX block.
- [x] Replicate the last real pixel (column `299`) into the row's padding.
- [x] Run `hexagon-sim --timing --profile` and inspect `hexagon-gprof`.
- [x] Prevent `-O3` from optimizing away benchmark work.
- [x] Keep a scalar C 3×3 reference and compare **individual pixels**.
- [x] Verify **900/900 pixels** on the documented test input.
- [ ] Test additional pixel patterns, random seeds, and image dimensions.
- [ ] Re-profile after every algorithmic change and keep a reproducible baseline.
- [ ] Inspect generated assembly when profiler results look suspicious.
- [ ] Continue optimizing memory traffic, load/store counts, and vector reuse across blocks.

---

## 8. Next Steps

**Suggested next lesson: Sliding Windows and Vector Reuse.**

Adjacent blocks and image rows reuse a lot of overlapping data. Rather than loading or rebuilding everything for every block, look for opportunities to **reuse vectors already in HVX registers** and reduce memory traffic.

Suggested development workflow:

1. Commit the current version that passes all tests as a baseline.
2. Use profiling to determine whether loads/stores, arithmetic, or call overhead dominates.
3. Change **one thing at a time** (for example, reuse a vector from the previous block).
4. Re-run the C reference comparison to catch border and block-transition errors.
5. Profile again before deciding whether to keep the optimization.

### General lesson

> **Correctness first. Profile second. Optimize based on evidence.**
>
> SIMD can process *more* bytes while running *faster*, and an optimizing compiler can turn a poorly designed benchmark into a meaningless result. Keep a scalar reference, use challenging input data, ensure the output is genuinely observed, and make performance measurements reproducible.

---

*These notes document progress through the passing 3×3 Max Filter implementation on Hexagon v68. Sliding-window and vector-reuse optimizations have not yet been implemented in the current code.*
