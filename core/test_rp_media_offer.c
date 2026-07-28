/*
 * test_rp_media_offer — the offer, byte-compared against the Python that is proven on devices.
 *
 *   ./test_rp_media_offer b   the uncompressed protobuf blob
 *   ./test_rp_media_offer     the whole offer: blob -> zlib 9 -> binary plist
 *
 * Compared by scripts/check-offer.sh. Byte equality is the standard here rather than "decodes to
 * the same thing", because a plist that is valid but differently laid out is exactly the kind of
 * difference that would go unnoticed until a device refuses the stream.
 */
#include "rp_media_offer.h"
#include <stdio.h>
int main(int argc, char **argv) {
    rp_offer_params p = {0};
    p.ssrc = 725448759; p.codec = RP_OFFER_CODEC_AUTO;
    p.model="Mac15,9"; p.os_version="2205.3.1"; p.build="25F80";
    p.call_id="1E312779-5E86-4742-9215-7522E1EB1610"; p.ltrp_enabled=1;
    static uint8_t buf[8192];
    size_t n = (argc>1 && argv[1][0]=='b') ? rp_media_blob_video(&p, buf, sizeof buf)
                                           : rp_build_offer(&p, buf, sizeof buf);
    if (!n) { fprintf(stderr,"build failed\n"); return 1; }
    for (size_t i=0;i<n;i++) printf("%02x", buf[i]);
    printf("\n");
    return 0;
}
