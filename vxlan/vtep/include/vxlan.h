#ifndef VXLAN_H
#define VXLAN_H

#include <stdint.h>
#include <stddef.h>

#define VXLAN_PORT    4789
#define VXLAN_HDR_LEN 8
#define VXLAN_IFLAG   0x08   /* bit 3 — VNI valid */

struct __attribute__((packed)) vxlan_hdr {
    uint8_t flags;
    uint8_t reserved1[3];
    uint8_t vni[3];          /* 24-bit VNI, big-endian */
    uint8_t reserved2;
};

/* Returns 0 on success, -1 on error */
int vxlan_parse(const uint8_t *buf, size_t len,
                struct vxlan_hdr *hdr, uint32_t *vni);

/* Writes 8-byte VXLAN header into buf */
void vxlan_build(uint8_t *buf, uint32_t vni);

/* Allocates new buffer: [VXLAN header][inner frame]. Caller must free(). */
uint8_t *vxlan_encapsulate(const uint8_t *frame, size_t frame_len,
                            uint32_t vni, size_t *out_len);

/* Zero-copy: returns pointer INTO buf. Do NOT free. Do NOT use after buf changes. */
const uint8_t *vxlan_decapsulate(const uint8_t *buf, size_t buf_len,
                                  uint32_t *vni, size_t *frame_len);

#endif
