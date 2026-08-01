#!/usr/sbin/dtrace -s
/*
 * How does Device Hub get video off the tunnel, and what does it do with it?
 *
 * Static analysis is the expensive answer here: AVConference is cache-resident, all Swift, and
 * `dyld_info -symbols` returns nothing, so there are no anchors to reverse from. dtrace patches
 * functions directly and needs no symbols beyond a name.
 *
 * The question is forced by everything else being excluded. We have every byte of the stream --
 * byte accounting balances exactly, and our NALs are byte-identical to ffmpeg's own RTP stack --
 * and two independent decoders reconstruct the same degrading pictures from them. Device Hub
 * receives an equivalent stream, whose capture decodes to garbage too, and its screen is clean.
 * So it is doing something we are not, between the socket and the display.
 *
 *   sudo ./scripts/trace-devicehub-io.d -p $(pgrep -x DeviceHub)
 */
#pragma D option quiet

dtrace:::BEGIN { printf("mirror the phone and swipe; ^C when done\n\n"); }

/* Socket reads: is it even reading the tunnel itself, and in what sizes? */
syscall::recvfrom:return, syscall::recvmsg:return, syscall::read:return
/pid == $target && arg0 > 0/
{
    @reads[probefunc] = count();
    @bytes[probefunc] = sum(arg0);
    @sizes[probefunc] = quantize(arg0);
}

/* Decode: does it use VideoToolbox's public API at all? */
pid$target::VTDecompressionSessionDecodeFrame:entry { @vt["DecodeFrame"] = count(); }
pid$target::VTDecompressionSessionCreateWithOptions:entry { @vt["CreateWithOptions"] = count(); }
pid$target::VTDecompressionSessionCreate:entry { @vt["Create"] = count(); }

/* Display: how do decoded pictures reach the screen? */
pid$target::CMSampleBufferCreateForImageBuffer:entry { @disp["CMSampleBufferCreateForImageBuffer"] = count(); }
pid$target::CVPixelBufferCreate:entry               { @disp["CVPixelBufferCreate"] = count(); }

dtrace:::END
{
    printf("\n=== socket reads ===\n");
    printa("  %-12s %@8d calls  %@10d bytes\n", @reads, @bytes);
    printa(@sizes);
    printf("\n=== VideoToolbox ===\n");
    printa("  %-32s %@d\n", @vt);
    printf("\n=== presentation ===\n");
    printa("  %-36s %@d\n", @disp);
    printf("\nZERO VideoToolbox decode calls while mirroring would mean Device Hub does not\n");
    printf("decode this stream through the public API, and every comparison we have made\n");
    printf("against 'our VideoToolbox usage' has been against the wrong thing.\n");
}
