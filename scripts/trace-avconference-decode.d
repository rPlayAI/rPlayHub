#!/usr/sbin/dtrace -s
/*
 * Does avconference decode through VideoToolbox's public entry points at all?
 *
 * This is the question dyld interposition could not answer. tools/vtcapture loads into Device Hub
 * and its hooks never fire, but that is a limitation of the technique, not a finding: __interpose
 * rebinds symbol references, and AVConference -> VideoToolbox is a call between two dylibs that
 * both live in the shared cache, where those references are already resolved. dtrace patches the
 * target function itself, so it sees the call however it was linked.
 *
 * Why it matters. Under a controlled, scripted swipe -- identical motion, same phone, same
 * network, each app running alone -- Device Hub's window stays clean while rPlayHub's garbles, and
 * ours garbles even when rendered at Device Hub's exact display size. Yet we receive MORE bits per
 * frame than Device Hub does (13,130 B vs 10,304), our depacketization is byte-identical to four
 * independent implementations including ffmpeg's, and every decoder and every build agrees on the
 * pictures. Device Hub's own captured stream decodes to garbage too.
 *
 * The only stage left is what avconference actually hands its decoder.
 *
 *   sudo ./scripts/trace-avconference-decode.d -p $(pgrep -x DeviceHub)
 *
 * Mirror the phone and swipe while it runs. Requires SIP disabled, which this machine already has.
 */

#pragma D option quiet
#pragma D option destructive

dtrace:::BEGIN
{
    printf("tracing Device Hub's decode path -- swipe the phone, then ^C\n\n");
}

/* The public decode entry. arg0 = session, arg1 = CMSampleBufferRef. */
pid$target::VTDecompressionSessionDecodeFrame:entry
{
    @decode["VTDecompressionSessionDecodeFrame"] = count();
    self->sample = arg1;
}

/* Session creation tells us how the decoder is configured, which is the other half of the
 * comparison against app/rPlayHub/VideoDecoder.swift. */
pid$target::VTDecompressionSessionCreateWithOptions:entry
{
    @create["VTDecompressionSessionCreateWithOptions"] = count();
}

pid$target::VTDecompressionSessionCreate:entry
{
    @create["VTDecompressionSessionCreate"] = count();
}

/* If avconference goes lower than VideoToolbox, these will fire instead and the public API will
 * not -- which would itself be the answer, and would explain why our VideoToolbox usage cannot
 * reproduce what it does. */
pid$target::VTDecompressionSessionWaitForAsynchronousFrames:entry
{
    @other["WaitForAsynchronousFrames"] = count();
}

pid$target::VTDecompressionSessionCanAcceptFormatDescription:entry
{
    @other["CanAcceptFormatDescription"] = count();
}

profile:::tick-5sec
{
    printf("--- 5s ---\n");
    printa("  decode  %-48s %@d\n", @decode);
    printa("  create  %-48s %@d\n", @create);
    printa("  other   %-48s %@d\n", @other);
    printf("\n");
}

dtrace:::END
{
    printf("\n=== totals ===\n");
    printa("  %-52s %@d\n", @decode);
    printa("  %-52s %@d\n", @create);
    printa("  %-52s %@d\n", @other);
    printf("\nIf decode count is ZERO while the window was mirroring, avconference is not using\n");
    printf("VideoToolbox's public decode API, and our whole decoder comparison is against the\n");
    printf("wrong thing. If it is NON-ZERO, the next step is dumping arg1's CMBlockBuffer and\n");
    printf("diffing it against what our depacketizer produces from the same packets.\n");
}
