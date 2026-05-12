#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "vxlan.h"

int vxlan_parse(const uint8_t *buf, size_t len,
                struct vxlan_hdr *hdr, uint32_t *vni)
{
    if (len < VXLAN_HDR_LEN) {
        fprintf(stderr, "vxlan_parse: too short (%zu bytes)\n", len);
        return -1;
    }

    memcpy(hdr, buf, sizeof(struct vxlan_hdr));

    if (!(hdr->flags & VXLAN_IFLAG)) {
        fprintf(stderr, "vxlan_parse: I flag not set (flags=0x%02x)\n",
                hdr->flags);
        return -1;
    }

    /* Extract 24-bit VNI from bytes 4, 5, 6 — big-endian */
    *vni = ((uint32_t)hdr->vni[0] << 16)
         | ((uint32_t)hdr->vni[1] <<  8)
         | ((uint32_t)hdr->vni[2]);

    return 0;
}

void vxlan_build(uint8_t *buf, uint32_t vni)
{
    struct vxlan_hdr *hdr = (struct vxlan_hdr *)buf;

    memset(hdr, 0, VXLAN_HDR_LEN);
    hdr->flags  = VXLAN_IFLAG;

    /* Encode 24-bit VNI big-endian into 3 bytes */
    hdr->vni[0] = (uint8_t)((vni >> 16) & 0xFF);
    hdr->vni[1] = (uint8_t)((vni >>  8) & 0xFF);
    hdr->vni[2] = (uint8_t)( vni        & 0xFF);
}

uint8_t *vxlan_encapsulate(const uint8_t *frame, size_t frame_len,
                            uint32_t vni, size_t *out_len)
{
    size_t total = VXLAN_HDR_LEN + frame_len;
    uint8_t *pkt = malloc(total);
    if (!pkt) {
        perror("vxlan_encapsulate: malloc");
        return NULL;
    }

    vxlan_build(pkt, vni);
    memcpy(pkt + VXLAN_HDR_LEN, frame, frame_len);

    *out_len = total;
    return pkt;
}

const uint8_t *vxlan_decapsulate(const uint8_t *buf, size_t buf_len,
                                  uint32_t *vni, size_t *frame_len)
{
    struct vxlan_hdr hdr;

    if (vxlan_parse(buf, buf_len, &hdr, vni) < 0)
        return NULL;

    if (buf_len <= VXLAN_HDR_LEN) {
        fprintf(stderr, "vxlan_decapsulate: no payload\n");
        return NULL;
    }

    *frame_len = buf_len - VXLAN_HDR_LEN;
    return buf + VXLAN_HDR_LEN;   /* zero-copy: pointer into original buf */
}
