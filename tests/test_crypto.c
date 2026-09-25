#include "flashcheck/chacha20.h"
#include "flashcheck/sha256.h"
#include "flashcheck/util.h"
#include "test_util.h"

static void hex(const uint8_t *p, size_t n, char *out)
{
    static const char *d = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = d[p[i] >> 4];
        out[i * 2 + 1] = d[p[i] & 15];
    }
    out[n * 2] = '\0';
}

static void test_sha256_vectors(void)
{
    static const char *in[] = { "", "abc",
                                "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" };
    static const char *want[] = {
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
    };
    char got[65];

    T_BEGIN("sha256 known answers");
    for (size_t i = 0; i < 3; i++) {
        uint8_t d[32];
        sha256(in[i], strlen(in[i]), d);
        hex(d, 32, got);
        g_test_checks++;
        if (strcmp(got, want[i]) != 0)
            T_FAIL("sha256(\"%s\") = %s, want %s", in[i], got, want[i]);
    }
}

static void test_sha256_streaming(void)
{
    uint8_t a[32], b[32];
    uint8_t *big = xalloc(100000);
    sha256_ctx c;

    T_BEGIN("sha256 streaming equals one-shot");
    for (size_t i = 0; i < 100000; i++)
        big[i] = (uint8_t)(i * 31u + 7u);
    sha256(big, 100000, a);
    sha256_init(&c);
    for (int i = 0; i < 1000; i++)
        sha256_update(&c, big + i * 100, 100);
    sha256_final(&c, b);
    CHECK(memcmp(a, b, 32) == 0);
    free(big);
}

static void test_chacha20_rfc8439(void)
{
    static const uint8_t key[32] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                     0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
                                     0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                     0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f };
    static const uint8_t nonce[12] = { 0x00, 0x00, 0x00, 0x09, 0x00, 0x00,
                                       0x00, 0x4a, 0x00, 0x00, 0x00, 0x00 };
    static const char *want = "10f1e7e4d13b5915500fdd1fa32071c4"
                              "c7d1f4c733c068030422aa9ac3d46c4e"
                              "d2826446079faa0914c2d705d98b02a2"
                              "b5129cd1de164eb9cbd083e8a2503c4e";
    uint8_t out[64];
    char got[129];

    T_BEGIN("chacha20 rfc8439 block");
    chacha20_block(key, 1, nonce, out);
    hex(out, 64, got);
    g_test_checks++;
    if (strcmp(got, want) != 0)
        T_FAIL("keystream = %s, want %s", got, want);
}

static void test_chacha20_seek(void)
{
    uint8_t key[32], nonce[12];
    uint8_t *full = xalloc(4096);
    uint8_t part[512];

    T_BEGIN("chacha20 seek equals slice");
    for (int i = 0; i < 32; i++)
        key[i] = (uint8_t)(i * 7 + 1);
    memset(nonce, 0, sizeof nonce);
    chacha20_stream(key, 0, nonce, full, 4096);
    chacha20_stream(key, (1536 / 64), nonce, part, 512);
    CHECK(memcmp(part, full + 1536, 512) == 0);
    free(full);
}

void test_crypto(void)
{
    printf("crypto\n");
    test_sha256_vectors();
    test_sha256_streaming();
    test_chacha20_rfc8439();
    test_chacha20_seek();
}
