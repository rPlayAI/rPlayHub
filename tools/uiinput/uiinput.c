// uiinput — post synthetic mouse events, so a mirroring session can be driven
// and swiped reproducibly without a human at the keyboard.
//
// The comparison between rPlayHub and Device Hub has always been "by eye, in
// separate sessions" (doc/RENDERING-HANDOFF.md). A hand swipe is never the same
// twice, so it cannot support a claim that one app artefacts and the other does
// not. This makes the motion identical and repeatable: the same pixel path, the
// same duration, the same step rate, into either window.
//
//   uiinput click  <x> <y>
//   uiinput drag   <x1> <y1> <x2> <y2> <ms> [steps]
//   uiinput swipes <x1> <y1> <x2> <y2> <ms> <count> <gap_ms>
//
// Coordinates are screen points, origin top-left, as screencapture reports them.

#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void post(CGEventType type, CGPoint p, CGMouseButton button) {
    CGEventRef e = CGEventCreateMouseEvent(NULL, type, p, button);
    if (!e) return;
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
}

static void move_to(CGPoint p) { post(kCGEventMouseMoved, p, kCGMouseButtonLeft); }

static void click(CGPoint p) {
    move_to(p);
    usleep(60000);
    post(kCGEventLeftMouseDown, p, kCGMouseButtonLeft);
    usleep(60000);
    post(kCGEventLeftMouseUp, p, kCGMouseButtonLeft);
}

// A drag with evenly spaced intermediate points. The step count matters: too
// few and the receiving app sees a teleport rather than a gesture, so the device
// never generates the fast motion we are trying to photograph.
static void drag(CGPoint a, CGPoint b, int ms, int steps) {
    if (steps < 2) steps = 2;
    move_to(a);
    usleep(80000);
    post(kCGEventLeftMouseDown, a, kCGMouseButtonLeft);
    useconds_t per = (useconds_t)(ms * 1000 / steps);
    for (int i = 1; i <= steps; i++) {
        double f = (double)i / steps;
        CGPoint p = {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f};
        post(kCGEventLeftMouseDragged, p, kCGMouseButtonLeft);
        usleep(per);
    }
    post(kCGEventLeftMouseUp, b, kCGMouseButtonLeft);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: uiinput click <x> <y>\n"
                "       uiinput drag <x1> <y1> <x2> <y2> <ms> [steps]\n"
                "       uiinput swipes <x1> <y1> <x2> <y2> <ms> <count> <gap_ms>\n");
        return 2;
    }
    if (!strcmp(argv[1], "click") && argc >= 4) {
        click((CGPoint){atof(argv[2]), atof(argv[3])});
        return 0;
    }
    if (!strcmp(argv[1], "drag") && argc >= 7) {
        int steps = argc > 7 ? atoi(argv[7]) : 30;
        drag((CGPoint){atof(argv[2]), atof(argv[3])},
             (CGPoint){atof(argv[4]), atof(argv[5])}, atoi(argv[6]), steps);
        return 0;
    }
    if (!strcmp(argv[1], "swipes") && argc >= 9) {
        CGPoint a = {atof(argv[2]), atof(argv[3])};
        CGPoint b = {atof(argv[4]), atof(argv[5])};
        int ms = atoi(argv[6]), count = atoi(argv[7]), gap = atoi(argv[8]);
        for (int i = 0; i < count; i++) {
            // Alternate direction so the content keeps moving instead of
            // hitting the end of a list and sitting still.
            if (i % 2) drag(b, a, ms, 30);
            else       drag(a, b, ms, 30);
            fprintf(stderr, "swipe %d/%d\n", i + 1, count);
            usleep((useconds_t)gap * 1000);
        }
        return 0;
    }
    fprintf(stderr, "bad arguments\n");
    return 2;
}
