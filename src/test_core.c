/*
 * test_core.c -- bit-exactness test for the C port of the E0C6200 core.
 *
 * Runs 500000 dg_step() calls on the Digimon V1 ROM and compares, every
 * 50000 steps, the instruction counter / PC / SHA-256 of the 169-byte
 * dg_vram() view against golden_trace.json (produced by the Python
 * reference implementation).  Also hashes the little-endian 2-byte PC of
 * every step and compares against the golden PC-trace digest.
 *
 * Contains a self-contained SHA-256 (no OpenSSL).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "digimon_core.h"

/* ------------------------------------------------------------- SHA-256 -- */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  data[64];
    uint32_t datalen;
} sha256_ctx;

#define ROTR32(a, b) (((a) >> (b)) | ((a) << (32 - (b))))

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static void sha256_transform(sha256_ctx *ctx, const uint8_t data[64])
{
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    int i, j;

    for (i = 0, j = 0; i < 16; i++, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | (uint32_t)data[j + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROTR32(m[i - 15], 7) ^ ROTR32(m[i - 15], 18) ^ (m[i - 15] >> 3);
        uint32_t s1 = ROTR32(m[i - 2], 17) ^ ROTR32(m[i - 2], 19) ^ (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t s1 = ROTR32(e, 6) ^ ROTR32(e, 11) ^ ROTR32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t s0 = ROTR32(a, 2) ^ ROTR32(a, 13) ^ ROTR32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        t1 = h + s1 + ch + sha256_k[i] + m[i];
        t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(sha256_ctx *ctx)
{
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
}

static void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(sha256_ctx *ctx, uint8_t hash[32])
{
    uint32_t i = ctx->datalen;
    int j;

    ctx->bitlen += (uint64_t)ctx->datalen * 8u;
    ctx->data[i++] = 0x80;
    if (i > 56) {
        while (i < 64) {
            ctx->data[i++] = 0x00;
        }
        sha256_transform(ctx, ctx->data);
        i = 0;
    }
    while (i < 56) {
        ctx->data[i++] = 0x00;
    }
    for (j = 7; j >= 0; j--) {
        ctx->data[i++] = (uint8_t)((ctx->bitlen >> (j * 8)) & 0xFF);
    }
    sha256_transform(ctx, ctx->data);

    for (j = 0; j < 8; j++) {
        hash[j * 4 + 0] = (uint8_t)((ctx->state[j] >> 24) & 0xFF);
        hash[j * 4 + 1] = (uint8_t)((ctx->state[j] >> 16) & 0xFF);
        hash[j * 4 + 2] = (uint8_t)((ctx->state[j] >> 8) & 0xFF);
        hash[j * 4 + 3] = (uint8_t)(ctx->state[j] & 0xFF);
    }
}

static void sha256_hex(const uint8_t hash[32], char out[65])
{
    static const char hexd[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        out[i * 2] = hexd[(hash[i] >> 4) & 0xF];
        out[i * 2 + 1] = hexd[hash[i] & 0xF];
    }
    out[64] = '\0';
}

/* ------------------------------------------------------------ test data -- */

#define TOTAL_STEPS 500000u
#define STEP_STRIDE 50000u
#define CHECKPOINTS (TOTAL_STEPS / STEP_STRIDE)

/* ROM 不在本仓库里（Bandai 版权）。按以下顺序找：argv[2] > $DG_ROM > ./roms/digimon.bin */
static const char *k_rom_path = "roms/digimon.bin";

static void resolve_rom_path(int argc, char **argv)
{
    const char *env = getenv("DG_ROM");
    if (argc > 2)        k_rom_path = argv[2];
    else if (env)        k_rom_path = env;
}

static const char *k_pc_trace_sha =
    "f42d1802a54def230c48e8af9e54fb2a8d49612d8bc2e13ddcb7929270ef5f6f";

static const uint32_t k_cp_steps[CHECKPOINTS] = {
    50000u, 100000u, 150000u, 200000u, 250000u,
    300000u, 350000u, 400000u, 450000u, 500000u
};

static const uint32_t k_cp_instr[CHECKPOINTS] = {
    19338u, 37989u, 55684u, 74133u, 92355u,
    110038u, 128932u, 146649u, 164724u, 183338u
};

static const uint32_t k_cp_pc[CHECKPOINTS] = {
    4674u, 365u, 365u, 6612u, 365u,
    365u, 360u, 365u, 4672u, 365u
};

static const char *k_cp_vram_sha[CHECKPOINTS] = {
    "4811b9ef205325bcddeadeb6d47f524d53c1c7296048ef156be1a79a326ffd15",
    "95d438c9fad56490eac699f246ae419301f37d00032243e6ac6bc8fb4e18a178",
    "95d438c9fad56490eac699f246ae419301f37d00032243e6ac6bc8fb4e18a178",
    "06e87d3c76068a7f0f1534db80881cfba44263fcacf6ccb9e6bffd69b6578d08",
    "4811b9ef205325bcddeadeb6d47f524d53c1c7296048ef156be1a79a326ffd15",
    "4811b9ef205325bcddeadeb6d47f524d53c1c7296048ef156be1a79a326ffd15",
    "95d438c9fad56490eac699f246ae419301f37d00032243e6ac6bc8fb4e18a178",
    "95d438c9fad56490eac699f246ae419301f37d00032243e6ac6bc8fb4e18a178",
    "95d438c9fad56490eac699f246ae419301f37d00032243e6ac6bc8fb4e18a178",
    "4811b9ef205325bcddeadeb6d47f524d53c1c7296048ef156be1a79a326ffd15"
};

/* ----------------------------------------------------------------- main -- */

/* static ROM buffer: the test driver keeps everything off the heap, like the core */
static uint8_t g_rom_buf[64 * 1024];

static uint8_t *load_rom(const char *path, uint32_t *out_len)
{
    FILE *f;
    long sz;
    uint8_t *buf = g_rom_buf;

    f = fopen(path, "rb");
    if (f == NULL) {
        printf("cannot open ROM: %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    if (sz > (long)sizeof(g_rom_buf)) {
        fclose(f);
        printf("ROM too large for buffer: %ld bytes\n", sz);
        return NULL;
    }
    rewind(f);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (uint32_t)sz;
    return buf;
}

int main(int argc, char **argv)
{
    resolve_rom_path(argc, argv);
    uint8_t *rom;
    uint32_t rom_len = 0;
    uint32_t i;
    uint32_t cp = 0;
    int failures = 0;
    sha256_ctx pc_trace;
    char hex[65];
    uint8_t digest[32];
    uint32_t trace_steps = 0;

    if (argc > 1) {
        trace_steps = (uint32_t)strtoul(argv[1], NULL, 10);
    }


    /* self-test of the bundled SHA-256 against the empty-string digest */
    sha256_init(&pc_trace);
    sha256_final(&pc_trace, digest);
    sha256_hex(digest, hex);
    if (strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") != 0) {
        printf("SHA-256 self-test FAILED: %s\n", hex);
        return 2;
    }

    rom = load_rom(k_rom_path, &rom_len);
    if (rom == NULL) {
        return 2;
    }
    printf("ROM: %s (%lu bytes)\n", k_rom_path, (unsigned long)rom_len);

    dg_init(rom, rom_len);

    sha256_init(&pc_trace);

    for (i = 1; i <= TOTAL_STEPS; i++) {
        uint8_t pcle[2];
        uint16_t pc;

        {
            int cyc = dg_step();
            if (i <= trace_steps) {
                uint16_t st[8];
                int k;
                dg_dbg_state(st);
                printf("T %lu %u %lu %d", (unsigned long)i,
                       (unsigned)dg_pc(), (unsigned long)dg_instr_count(), cyc);
                for (k = 0; k < 8; k++) {
                    printf(" %u", (unsigned)st[k]);
                }
                printf("\n");
            }
        }
        pc = dg_pc();
        pcle[0] = (uint8_t)(pc & 0xFF);
        pcle[1] = (uint8_t)((pc >> 8) & 0xFF);
        sha256_update(&pc_trace, pcle, 2);

        if (i % STEP_STRIDE == 0u) {
            const uint8_t *vram = dg_vram();
            uint32_t vlen = dg_vram_len();
            sha256_ctx vs;
            uint32_t instr = dg_instr_count();
            uint32_t now_pc = (uint32_t)dg_pc();
            int ok = 1;

            sha256_init(&vs);
            sha256_update(&vs, vram, (size_t)vlen);
            sha256_final(&vs, digest);
            sha256_hex(digest, hex);

            if (cp >= CHECKPOINTS) {
                printf("internal error: checkpoint index overflow\n");
                            return 2;
            }
            if (i != k_cp_steps[cp]) {
                printf("  !! checkpoint step MISMATCH (%lu vs %lu)\n",
                       (unsigned long)i, (unsigned long)k_cp_steps[cp]);
                failures++;
            }

            printf("step %7lu  instr %7lu (want %7lu)  pc %5lu (want %5lu)  "
                   "vram[%lu] %s\n",
                   (unsigned long)i, (unsigned long)instr,
                   (unsigned long)k_cp_instr[cp],
                   (unsigned long)now_pc, (unsigned long)k_cp_pc[cp],
                   (unsigned long)vlen, hex);

            if (instr != k_cp_instr[cp]) {
                printf("  !! instr MISMATCH\n");
                ok = 0;
            }
            if (now_pc != k_cp_pc[cp]) {
                printf("  !! pc MISMATCH\n");
                ok = 0;
            }
            if (strcmp(hex, k_cp_vram_sha[cp]) != 0) {
                printf("  !! vram sha MISMATCH (want %s)\n", k_cp_vram_sha[cp]);
                ok = 0;
            }
            if (!ok) {
                failures++;
            }
            cp++;
        }
    }

    sha256_final(&pc_trace, digest);
    sha256_hex(digest, hex);
    printf("pc trace sha256: %s\n", hex);
    if (strcmp(hex, k_pc_trace_sha) != 0) {
        printf("  !! pc trace MISMATCH (want %s)\n", k_pc_trace_sha);
        failures++;
    }


    if (failures != 0) {
        printf("\n%d CHECKPOINT(S) FAILED\n", failures);
        return 1;
    }

    printf("\nALL CHECKPOINTS MATCH\n");
    return 0;
}
