#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "vxlan.h"

#define TEST(name, cond) do {                              \
    if (cond) printf("[PASS] %s\n", name);                 \
    else      printf("[FAIL] %s  (line %d)\n", name, __LINE__); \
} while(0)

/* Known-good VXLAN packet — VNI = 42 */
static uint8_t valid_pkt[] = {
    0x08,               /* flags: I flag set        */
    0x00, 0x00, 0x00,   /* reserved                 */
    0x00, 0x00, 0x2A,   /* VNI = 42                 */
    0x00,               /* reserved                 */
    /* inner Ethernet frame */
    0xff,0xff,0xff,0xff,0xff,0xff,   /* dst broadcast */
    0x00,0x11,0x22,0x33,0x44,0x55,   /* src           */
    0x08,0x00,                        /* IPv4          */
    0xde,0xad,0xbe,0xef               /* payload       */
};

void test_parse_valid(void)
{
    struct vxlan_hdr hdr;
    uint32_t vni = 0;
    int r = vxlan_parse(valid_pkt, sizeof(valid_pkt), &hdr, &vni);
    TEST("parse valid packet returns 0", r == 0);
    TEST("VNI is 42",                    vni == 42);
    TEST("I flag is set",                hdr.flags & VXLAN_IFLAG);
}

void test_parse_too_short(void)
{
    struct vxlan_hdr hdr; uint32_t vni;
    uint8_t short_pkt[] = {0x08, 0x00, 0x00};
    TEST("parse too short returns -1",
         vxlan_parse(short_pkt, 3, &hdr, &vni) == -1);
}

void test_parse_no_iflag(void)
{
    struct vxlan_hdr hdr; uint32_t vni;
    uint8_t bad[8] = {0x00};   /* I flag not set */
    TEST("no I flag returns -1",
         vxlan_parse(bad, 8, &hdr, &vni) == -1);
}

void test_roundtrip(void)
{
    uint8_t buf[VXLAN_HDR_LEN];
    struct vxlan_hdr hdr;
    uint32_t vni_in = 999, vni_out = 0;

    vxlan_build(buf, vni_in);
    int r = vxlan_parse(buf, VXLAN_HDR_LEN, &hdr, &vni_out);
    TEST("build+parse returns 0",   r == 0);
    TEST("VNI survives round-trip", vni_out == vni_in);
    TEST("reserved bytes are zero", buf[1] == 0 && buf[7] == 0);
}

void test_encap_decap(void)
{
    uint8_t frame[] = {
        0xff,0xff,0xff,0xff,0xff,0xff,
        0x00,0x11,0x22,0x33,0x44,0x55,
        0x08,0x00, 0xde,0xad
    };
    size_t out_len = 0;
    uint32_t vni_in = 12345, vni_out = 0;

    uint8_t *pkt = vxlan_encapsulate(frame, sizeof(frame), vni_in, &out_len);
    TEST("encapsulate non-null",        pkt != NULL);
    TEST("encapsulated length correct", out_len == VXLAN_HDR_LEN + sizeof(frame));

    size_t inner_len = 0;
    const uint8_t *inner = vxlan_decapsulate(pkt, out_len,
                                              &vni_out, &inner_len);
    TEST("decapsulate non-null",    inner != NULL);
    TEST("VNI survives encap/decap", vni_out == vni_in);
    TEST("inner frame bytes match",
         memcmp(inner, frame, sizeof(frame)) == 0);

    free(pkt);
}

void test_vni_boundaries(void)
{
    uint8_t buf[VXLAN_HDR_LEN];
    struct vxlan_hdr hdr;
    uint32_t vni_out;

    vxlan_build(buf, 0);
    vxlan_parse(buf, VXLAN_HDR_LEN, &hdr, &vni_out);
    TEST("VNI=0 round-trips", vni_out == 0);

    vxlan_build(buf, 0xFFFFFF);
    vxlan_parse(buf, VXLAN_HDR_LEN, &hdr, &vni_out);
    TEST("VNI=0xFFFFFF round-trips", vni_out == 0xFFFFFF);
}

int main(void)
{
    printf("=== VXLAN Tests ===\n");
    test_parse_valid();
    test_parse_too_short();
    test_parse_no_iflag();
    test_roundtrip();
    test_encap_decap();
    test_vni_boundaries();
    printf("Done.\n");
    return 0;
}
