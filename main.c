
#include <stdio.h>
#include <stdint.h>
#include <hexagon_types.h>
#include <hvx_hexagon_protos.h>

#define WIDTH  300
#define HEIGHT 3
#define STRIDE 384
#define VL 128

static uint8_t input[HEIGHT][STRIDE]
    __attribute__((aligned(128)));

static uint8_t output[HEIGHT][STRIDE]
    __attribute__((aligned(128)));

static uint8_t tmp_output[HEIGHT][STRIDE]
    __attribute__((aligned(128)));

__attribute__((noinline))
void max_filter_hvx(const uint8_t *src, uint8_t *dst, int n)
{
    if (n <= 0) return;

    int i = 0;
    
    for (; i + VL <= n; i += VL) {
        uint8_t left_edge = src[i == 0 ? 0: i - 1];
        uint8_t right_edge = src[i + VL < n ? i + VL: n - 1];

        HVX_Vector cur = *(HVX_Vector *)(src + i);
        HVX_Vector prev = Q6_Vb_vsplat_R(left_edge);
        HVX_Vector next = Q6_Vb_vsplat_R(right_edge);

        
        HVX_Vector left = Q6_V_vlalign_VVI(cur, prev, 1);
        HVX_Vector right = Q6_V_valign_VVI(next, cur, 1);

        HVX_Vector tmp = Q6_Vub_vmax_VubVub(left, cur);
        HVX_Vector max = Q6_Vub_vmax_VubVub(right, tmp);

        *(HVX_Vector *)(dst + i) = max; // if not devivided to 128, degrade into the first 128 byte alignment -> dst
    }
    // tail handling
    for (; i < n; i++) {
        uint8_t l = src[i == 0 ? 0 : i - 1];
        uint8_t c = src[i];
        uint8_t r = src[i + 1 < n ? i + 1 : n - 1];

        uint8_t max = l > c ? l : c;
        dst[i] = max > r ? max : r;
    }
}


__attribute__((noinline))
void vertical_max_filter(
    const uint8_t input[HEIGHT][STRIDE],
    uint8_t output[HEIGHT][STRIDE],
    int width)
{
    for (int y = 0; y < HEIGHT; y++) {

        int top_y = (y == 0) ? 0 : y - 1;
        int bot_y = (y == HEIGHT - 1) ? y : y + 1;

        int x = 0;

        for (; x + VL <= width; x += VL) {

            HVX_Vector top =
                *(const HVX_Vector *)(input[top_y] + x);

            HVX_Vector mid =
                *(const HVX_Vector *)(input[y] + x);

            HVX_Vector bot =
                *(const HVX_Vector *)(input[bot_y] + x);

            HVX_Vector tmp =
                Q6_Vub_vmax_VubVub(top, mid);

            HVX_Vector result =
                Q6_Vub_vmax_VubVub(tmp, bot);

            *(HVX_Vector *)(output[y] + x) = result;
        }

        // tail handling
        for (; x < width; x++) {

            uint8_t top = input[top_y][x];
            uint8_t mid = input[y][x];
            uint8_t bot = input[bot_y][x];

            uint8_t max = top > mid ? top : mid;
            output[y][x] = max > bot ? max : bot;
        }
    }
}


static uint8_t reference[HEIGHT][STRIDE]
    __attribute__((aligned(128)));

void max_filter_3x3_reference(
    const uint8_t src[HEIGHT][STRIDE],
    uint8_t dst[HEIGHT][STRIDE])
{
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {

            uint8_t max_val = 0;

            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {

                    int yy = y + dy;
                    int xx = x + dx;

                    // Replicate border vertically
                    if (yy < 0) yy = 0;
                    if (yy >= HEIGHT) yy = HEIGHT - 1;

                    // Replicate border horizontally
                    if (xx < 0) xx = 0;
                    if (xx >= WIDTH) xx = WIDTH - 1;

                    uint8_t pixel = src[yy][xx];

                    if (pixel > max_val) {
                        max_val = pixel;
                    }
                }
            }

            dst[y][x] = max_val;
        }
    }
}

__attribute__((noinline))
static void run_hvx_pipeline(void)
{
    for (int y = 0; y < HEIGHT; y++) {
        max_filter_hvx(input[y], tmp_output[y], STRIDE);
    }

    vertical_max_filter(tmp_output, output, STRIDE);
}


#define REPEAT 100

int main(void)
{
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            input[y][x] =
                (uint8_t)((x * 37 + y * 83 + (x ^ y)) & 255);
        }
    }

    input[0][0]   = 255;
    input[1][127] = 240;
    input[1][128] = 50;
    input[2][255] = 220;
    input[2][256] = 30;
    input[2][299] = 250;

    // Replicate pixel cuối vào vùng padding
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = WIDTH; x < STRIDE; x++) {
            input[y][x] = input[y][WIDTH - 1];
        }
    }

    for (int k = 0; k < 3; k++) {
        run_hvx_pipeline();

        // Buộc compiler coi output là dữ liệu được sử dụng
        __asm__ __volatile__(
            "" : : "m"(output) : "memory"
        );
    }

    for (int k = 0; k < REPEAT; k++) {
        run_hvx_pipeline();

        __asm__ __volatile__(
            "" : : "m"(output) : "memory"
        );
    }

    unsigned checksum = 0;

    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            checksum += output[y][x];
        }
    }

    printf("Checksum = %u\n", checksum);
    printf("out[1][127] = %u\n",
           (unsigned)output[1][127]);


    // ===== VERIFY HVX AGAINST C REFERENCE =====
    max_filter_3x3_reference(input, reference);

    int errors = 0;
    unsigned ref_checksum = 0;

    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {

            uint8_t hvx_val = output[y][x];
            uint8_t ref_val = reference[y][x];

            ref_checksum += ref_val;

            if (hvx_val != ref_val) {
                if (errors < 10) {
                    printf(
                        "FAIL [%d][%d]: HVX=%u REF=%u\n",
                        y, x,
                        (unsigned)hvx_val,
                        (unsigned)ref_val
                    );
                }
                errors++;
            }
        }
    }

    printf("Reference checksum = %u\n", ref_checksum);

    if (errors == 0) {
        printf("PASS! All %d pixels match.\n",
            WIDTH * HEIGHT);
    } else {
        printf("FAIL! %d pixels differ.\n", errors);
        return 1;
    }

    return 0;
}