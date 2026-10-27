# HVX Intrinsics Cheatsheet (C) — Hexagon v79

> **Mục đích:** nhìn bài toán C → chọn phép SIMD → tự đọc / tra tên `Q6_...` → viết và kiểm tra HVX kernel.
>
> **Phạm vi:** C intrinsics, vector **128 byte** trên HVX v79. Không phải cheatsheet assembly thuần; các tên dưới đây được đối chiếu với *Qualcomm Hexagon v79 HVX Programmer Reference Manual* (80-N2040-61 AB, 2025) và header Clang. Các biến thể phụ thuộc version kiến trúc / toolchain.

## 0. Mindset: khi nào nghĩ đến HVX?

```text
Dữ liệu có N phần tử cùng kiểu
   └─ Mỗi phần tử làm cùng phép toán?
      ├─ KHÔNG → cần đổi thuật toán / không nhất thiết hợp SIMD
      └─ CÓ → có dependency giữa các phần tử?
               ├─ CÓ → nghiên cứu scan/reduction/shuffle, không vector hóa ngây thơ
               └─ ÍT / KHÔNG → ứng viên tốt cho HVX
```

Ví dụ tốt: cộng pixel, threshold, clamp, average, dot product theo nhóm, filter trên nhiều pixel. **Đừng kết luận HVX luôn nhanh hơn**: kernel quá nhỏ, memory traffic, alignment, tail và chi phí gọi DSP đều ảnh hưởng.

## 1. Cú pháp tổng quát `Q6_...`

**Mẫu tư duy (không phải grammar tuyệt đối):**

```text
Q6_<KIỂU_ĐẦU_RA>_<TÊN_PHÉP_TOÁN>_<CHỮ_KÝ_TOÁN_HẠNG>[_MODIFIER]
 │         │             │                  │                 │
 │         │             │                  │                 └─ sat, rnd, ...
 │         │             │                  └─ kiểu/tổ hợp các input (và đôi khi accumulator)
 │         │             └─ vadd, vsub, vmax, vcmp_gt, vmux, ...
 │         └─ Vub, Vw, Wuh, Q, V, ...
 └─ tiền tố intrinsics Hexagon
```

**Phân rã ví dụ 1:**

```c
HVX_Vector c = Q6_Vub_vadd_VubVub_sat(a, b);
//              ^^ ^^^ ^^^^ ^^^ ^^^ ^^^
//              |   |   |   |   |   +-- saturate (chặn vào [0,255])
//              |   |   |   +---+------ 2 đầu vào unsigned byte vector
//              |   |   +-------------- vector add
//              |   +------------------ kiểu phần tử output unsigned byte
//              +---------------------- output là một HVX_Vector
```

Assembly tương ứng (trong manual): `Vd.ub = vadd(Vu.ub,Vv.ub):sat`.

**Phân rã ví dụ 2:**

```c
HVX_VectorPred q = Q6_Q_vcmp_gt_VubVub(a, b);
//                    ^ ^^^^^^^ ^^^^^^
//                    |    |       +---- so sánh từng unsigned byte
//                    |    +------------ compare greater-than
//                    +----------------- output predicate (mask Q)
```

**Phân rã ví dụ 3 (widening):**

```c
HVX_VectorPair wide = Q6_Wuh_vzxt_Vub(a);
//                       ^^^ ^^^^ ^^^
//                       |     |   +-- input 128 unsigned bytes
//                       |     +------ zero extend 8 → 16 bit
//                       +------------ output pair 2 × 128 byte
```

### 1.1 Bảng ký hiệu trong tên intrinsic

| Thành phần | Đọc như thế nào | Ví dụ |
|---|---|---|
| `Q6_` | prefix của intrinsics Hexagon | `Q6_V_vzero()` |
| `V` | một vector, thường `HVX_Vector` | `Q6_V_vand_VV` |
| `W` | **vector pair**, thường `HVX_VectorPair` | `Q6_Wh_vmpy_VbVb` |
| `Q` | predicate/mask (`HVX_VectorPred`) | `Q6_Q_vcmp_gt_VubVub` |
| `R` | nguồn scalar register (`Word32`/integer scalar) | `Q6_Vb_vsplat_R` |
| `I` | immediate (hằng được encode trong lệnh) | `Q6_V_valign_VVI` |
| `b`, `ub` | signed / unsigned byte, 8-bit | `Vb`, `Vub` |
| `h`, `uh` | signed / unsigned halfword, 16-bit | `Vh`, `Vuh` |
| `w`, `uw` | signed / unsigned word, 32-bit | `Vw`, `Vuw` |
| `_sat` | saturation, nếu instruction hỗ trợ | `..._sat` |
| `_rnd` | rounding, nếu instruction hỗ trợ | `..._rnd` |
| `acc` | dạng tích lũy, thường có accumulator là đối số đầu | `Q6_Vuw_vrmpyacc_VuwVubVub` |

**Cực quan trọng:** `Vub`, `Vh`, `Ww` mô tả **cách diễn giải các lane**, không phải các kiểu C riêng như `uint8_t[128]`. Đa số toán hạng `Vb`, `Vub`, `Vh`, `Vw` trong C đều có cùng kiểu `HVX_Vector`.

**Không được tự chế tên.** Ví dụ `Q6_Vub_vadd_VubVub_sat` có trong manual, nhưng từ đó **không suy ra** `Q6_Vub_vadd_VubVub` (không `_sat`) cũng tồn tại. Phải tra prototype thật.

### 1.2 Cách giải một tên lệnh bất kỳ trong 5 giây

1. Bóc `Q6_`.
2. Trước dấu `_` tiếp theo: **output** là `V`, `W`, `Q`? Có hậu tố `ub/h/w` không?
3. Đọc mnemonic: `vadd`, `vcmp_gt`, `vmux`, `vmpy`, ...
4. Đọc input signature theo thứ tự: `VubVub`, `QVV`, `VR`, `VVI`, ...
5. Đọc modifier (`sat`, `rnd`, ...), **sau đó đối chiếu prototype** trong header/manual.

## 2. Register / datatype / memory

HVX v79 dùng 128-byte vector (1024 bit):

| Nhìn mỗi phần tử là | Số lane / `HVX_Vector` | Khoảng giá trị |
|---|---:|---|
| `b` signed 8-bit | 128 | -128..127 |
| `ub` unsigned 8-bit | 128 | 0..255 |
| `h` signed 16-bit | 64 | -32768..32767 |
| `uh` unsigned 16-bit | 64 | 0..65535 |
| `w` signed 32-bit | 32 | -2^31..2^31-1 |
| `uw` unsigned 32-bit | 32 | 0..2^32-1 |

```c
HVX_Vector      v;  // 128 byte
HVX_VectorPair  w;  // 256 byte (hai vector)
HVX_VectorPred  q;  // predicate mask, KHÔNG phải mảng byte 0/1 bình thường
```

**Alignment:** với ví dụ cast kiểu `*(HVX_Vector*)ptr`, đảm bảo `ptr` aligned 128 byte. Truy cập unaligned có instruction/kiểu riêng; **cast không tự biến địa chỉ lệch thành aligned**. Trong memory instruction assembly, `vmem` (aligned) khác `vmemu` (unaligned). Trên v79, VMEM aligned có thể bỏ các bit địa chỉ thấp, nên đừng coi đây chỉ là tối ưu hiệu năng.

## 3. Bảng lệnh thường dùng: chọn theo bài toán

Các tên C bên dưới là những **prototype có thật** trong manual v79. `a`, `b` mặc định là `HVX_Vector`, `q` là `HVX_VectorPred`, `acc` tương ứng loại output.

| Muốn làm gì? | Intrinsic C | Hiểu theo khỉ |
|---|---|---|
| Tạo vector zero | `Q6_V_vzero()` | 128 byte = 0 |
| Tạo vector pair zero | `Q6_W_vzero()` | 256 byte = 0 |
| Cộng signed byte (wrap) | `Q6_Vb_vadd_VbVb(a,b)` | từng byte cộng theo 8 bit |
| Cộng unsigned byte, chặn 255 | `Q6_Vub_vadd_VubVub_sat(a,b)` | `min(a[i]+b[i],255)` |
| Trừ unsigned byte, chặn 0 | `Q6_Vub_vsub_VubVub_sat(a,b)` | `max(a[i]-b[i],0)` |
| Cộng signed 16-bit | `Q6_Vh_vadd_VhVh(a,b)` | cộng 64 cặp halfword |
| Cộng signed 32-bit | `Q6_Vw_vadd_VwVw(a,b)` | cộng 32 cặp word |
| Average byte | `Q6_Vub_vavg_VubVub(a,b)` | trung bình integer |
| Average byte có rounding | `Q6_Vub_vavg_VubVub_rnd(a,b)` | trung bình, có làm tròn |
| Max unsigned byte | `Q6_Vub_vmax_VubVub(a,b)` | lấy lớn hơn |
| Min unsigned byte | `Q6_Vub_vmin_VubVub(a,b)` | lấy nhỏ hơn |
| Absolute signed halfword | `Q6_Vh_vabs_Vh(a)` | `abs`, lưu ý giá trị biên |
| Absolute có saturation | `Q6_Vh_vabs_Vh_sat(a)` | chặn trường hợp min signed |
| Nhân bản scalar 8-bit | `Q6_Vb_vsplat_R(x)` | lặp byte thấp của x |
| Nhân bản scalar 16-bit | `Q6_Vh_vsplat_R(x)` | lặp halfword thấp của x |
| Bitwise AND | `Q6_V_vand_VV(a,b)` | AND mọi bit |
| Bitwise OR | `Q6_V_vor_VV(a,b)` | OR mọi bit |
| Bitwise XOR | `Q6_V_vxor_VV(a,b)` | XOR mọi bit |
| Bitwise NOT | `Q6_V_vnot_V(a)` | đảo mọi bit |
| Compare `a > b` unsigned byte | `Q6_Q_vcmp_gt_VubVub(a,b)` | trả mask Q |
| Compare `a == b` byte | `Q6_Q_vcmp_eq_VbVb(a,b)` | dấu của byte không ảnh hưởng tới equality |
| Chọn theo mask | `Q6_V_vmux_QVV(q,a,b)` | mask true → a; false → b |
| Zero-extend bytes → halfwords | `Q6_Wuh_vzxt_Vub(a)` | một V → một W |
| Sign-extend bytes → halfwords | `Q6_Wh_vsxt_Vb(a)` | giữ nguyên dấu |
| Pack signed halfword → unsigned byte (sat) | `Q6_Vub_vpack_VhVh_sat(a,b)` | hai V → một V, chặn [0,255] |
| Nhân signed bytes → signed halfwords | `Q6_Wh_vmpy_VbVb(a,b)` | kết quả rộng gấp đôi, trả W |
| Dot-product nhóm 4 unsigned bytes | `Q6_Vuw_vrmpy_VubVub(a,b)` | 4 tích byte cộng thành 1 word |
| Dot-product rồi tích lũy | `Q6_Vuw_vrmpyacc_VuwVubVub(acc,a,b)` | cộng dot-product vào acc |
| Align hai vector theo scalar | `Q6_V_valign_VVR(a,b,shift)` | căn chỉnh vùng byte |
| Align theo immediate | `Q6_V_valign_VVI(a,b,imm)` | `imm` phải phù hợp encoding |
| Deal bytes | `Q6_Vb_vdeal_Vb(a)` | gom / tái bố trí chẵn-lẻ |
| Shuffle bytes | `Q6_Vb_vshuff_Vb(a)` | xen / tái bố trí chẵn-lẻ |

**Cảnh báo:** phần shuffle/pack/multiply có mapping lane cụ thể, không nên suy luận thứ tự output chỉ từ tên lệnh. Mở mục tương ứng trong manual và tự thử với input `0,1,2,...` trước khi dùng xử lý ảnh.

## 4. Đọc các kiểu tên khó hơn

### Compare → predicate → mux

```c
HVX_VectorPred mask = Q6_Q_vcmp_gt_VubVub(a, b);
HVX_Vector result = Q6_V_vmux_QVV(mask, a, b);
```

- `Q6_Q...` tạo **mask**, không tạo vector pixel.
- `Q6_V_vmux_QVV`: đầu vào gồm `Q` + `V` + `V`; đầu ra `V`.
- Đây tương đương chọn `max(a[i],b[i])` theo byte. Nếu chỉ cần max, `Q6_Vub_vmax_VubVub(a,b)` đơn giản hơn.

### Widening: kích cỡ dữ liệu có thể tăng, **số byte không mất đi**

```c
HVX_VectorPair wide = Q6_Wuh_vzxt_Vub(a);
```

128 số 8-bit → 128 số 16-bit → cần 256 byte → `W` (pair). Không thể ép về một vector mà vẫn giữ toàn bộ phần tử.

### Accumulator đi vào input signature

```c
HVX_Vector acc = Q6_V_vzero();
acc = Q6_Vuw_vrmpyacc_VuwVubVub(acc, a, b);
```

`Vuw` đầu trong `VuwVubVub` chính là accumulator. Đây không phải hàm `vrmpy` nhận 2 tham số; nó nhận **3 tham số**.

## 5. Minimal kernel C: cộng 128 pixel có saturation

```c
#include <stdint.h>
#include <stdio.h>
#include <hexagon_types.h>
#include <hvx_hexagon_protos.h>

#define VL 128

static uint8_t a[VL] __attribute__((aligned(VL)));
static uint8_t b[VL] __attribute__((aligned(VL)));
static uint8_t out[VL] __attribute__((aligned(VL)));

int main(void) {
    for (int i = 0; i < VL; ++i) {
        a[i] = (uint8_t)i;
        b[i] = 200;
    }

    HVX_Vector va = *(const HVX_Vector *)a;  // LOAD
    HVX_Vector vb = *(const HVX_Vector *)b;  // LOAD
    HVX_Vector vc = Q6_Vub_vadd_VubVub_sat(va, vb); // COMPUTE
    *(HVX_Vector *)out = vc;               // STORE

    printf("%u %u %u\n", (unsigned)out[0],
           (unsigned)out[55], (unsigned)out[56]);
    // 200 255 255
    return 0;
}
```

Đây là idiom cast/load/store **dành cho Hexagon toolchain** (không phải C portable giữa mọi kiến trúc). Với dữ liệu ở địa chỉ không aligned, phải chọn hướng load/store phù hợp, không dùng cast aligned bừa bãi.

### Muốn xử lý N không chia hết cho 128?

```c
// Ý tưởng: bulk xử lý từng 128 byte, tail xử lý scalar.
for (size_t i = 0; i < n_aligned_down; i += 128) {
    // load → compute → store (địa chỉ phải aligned theo yêu cầu)
}
for (size_t i = n_aligned_down; i < n; ++i) {
    // xử lý phần đuôi, tránh đọc/ghi tràn buffer
}
```

`n_aligned_down = (n / 128) * 128`. Điều kiện địa chỉ đầu phải aligned được kiểm tra **riêng**; `n` chia hết cho 128 không nói lên alignment của pointer.

## 6. Compile và tự tra lệnh trên máy mày

```bash
# Thay v79 bằng architecture mà toolchain/thiết bị thực sự hỗ trợ.
hexagon-clang -mv79 -mhvx -mhvx-length=128B -O2 -c main.c -o main.o

# Kiểm tra macro bật HVX và vector length:
hexagon-clang -mv79 -mhvx -mhvx-length=128B \
  -dM -E -x c /dev/null | grep -E '__HVX__|__HVX_ARCH__|__HVX_LENGTH__'

# Tìm chính header mà compiler này đang dùng:
RESOURCE_DIR="$(hexagon-clang -print-resource-dir)"
HEADER="$RESOURCE_DIR/include/hvx_hexagon_protos.h"

# Liệt kê các biến thể cộng hoặc compare:
grep -n 'Q6_.*vadd' "$HEADER" | head -30
grep -n 'Q6_.*vcmp_gt' "$HEADER" | head -30

# Tra đúng một intrinsic + xem khối comment Assembly Syntax / Prototype:
grep -n -B 6 -A 3 'Q6_Vub_vadd_VubVub_sat' "$HEADER" | head -20
```

`HVX_Vector` **không được khai báo**? Trước tiên kiểm tra `-mhvx`, `-mhvx-length=128B`, `-mvXX` và header. `HVX_Vector` được điều kiện hóa bởi HVX macro / vector length trong `hexagon_types.h`.

Nếu compile được nhưng `./main` trên Ubuntu không chạy, đó là **binary Hexagon DSP**, không phải binary x86. Chạy với **Hexagon simulator** tương thích hoặc thiết bị DSP thực, theo setup SDK của mày.

### Cách tra đúng (khỏi đoán mò)

1. Từ bài toán tìm **mnemonic assembly** (`vadd`, `vcmp.gt`, `vpack`, `vrmpy`).
2. Mở mục *Intrinsics* trong manual v79, xem cột **Assembly Syntax ↔ C Intrinsic Prototype**.
3. Kiểm tra lại đúng tên trong `hvx_hexagon_protos.h` **của compiler đang sử dụng**.
4. Xem return type: `HVX_Vector`, `HVX_VectorPair`, `HVX_VectorPred`? Có bao nhiêu parameter, kiểu gì?
5. Xem version guard `#if __HVX_ARCH__ >= ...`, modifier, alignment, các quy tắc packing.
6. Test mảng số nhỏ + đối chiếu scalar reference **trước khi benchmark**.

## 7. Checklist trước khi chọn intrinsic

- [ ] Input là 8-bit, 16-bit hay 32-bit? Signed hay unsigned?
- [ ] Output có thể overflow không? Cần wrap, saturation, round hay widening?
- [ ] Kết quả cần một vector (`V`), vector pair (`W`) hay mask (`Q`)?
- [ ] Toán hạng là vector, scalar register (`R`) hay immediate (`I`)?
- [ ] Các phần tử độc lập không? Có cross-lane permutation/reduction không?
- [ ] Pointer aligned? Tổng kích thước có tail?
- [ ] Intrinsic này thật sự tồn tại trong header của toolchain?
- [ ] Kết quả có khớp implementation scalar ở input biên không?
- [ ] Sau đó mới đo: speedup, stalls, băng thông, overhead FastRPC.

## 8. Mini bài tập tự tra (không có đáp án sẵn)

**Bài A:** Viết kernel `out[i] = min(a[i], b[i])` với `uint8_t`. Chọn đúng intrinsic.

**Bài B:** Viết kernel `out[i] = (a[i] > 100 ? a[i] : 0)`. Gợi ý tra `vsplat`, `vcmp.gt`, `vmux`.

**Bài C:** Tại sao `128 byte → 128 halfword` không thể chứa trong một `HVX_Vector`? Chọn loại output và tên intrinsic.

**Bài D:** Tìm bằng `grep` một intrinsic có hậu tố `_rnd_sat`, chép prototype, giải mã tên, và giải thích nguyên nhân cần cả rounding + saturation.

## 9. Nguồn tra cứu

- **Nguồn chính của cheatsheet:** *Qualcomm Hexagon v79 HVX Programmer Reference Manual*, 80-N2040-61 AB, Jan 2025, tài liệu người học đã cung cấp. Tra đặc biệt: Ch. 3 Registers, Ch. 4 Memory, Ch. 5 Vector instructions, Ch. 8 HVX instruction set → các bảng **Intrinsics**.
- **Compiler header có prototype thực:** [Clang `hvx_hexagon_protos.h`](https://clang.llvm.org/doxygen/hvx__hexagon__protos_8h_source.html). Bản header trên web có thể khác bản SDK cài máy, nên ưu tiên header **đúng toolchain hiện tại**.
- **Ví dụ C người học đang theo:** [Hexagon DSP programming / 01-hvx-example.c](https://github.com/asvkarthick/Hexagon_DSP_programming/blob/master/06-hvx-example/01-hvx-example.c).

**Một câu chốt:** Tên `Q6_...` là *bản tóm tắt chữ ký instruction*, không phải ngôn ngữ lắp ráp Lego. Đọc để định hướng, **tra prototype để xác nhận**.
