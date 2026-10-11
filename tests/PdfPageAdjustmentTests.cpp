#include "../src/PdfPageAdjustment.h"
#include <assert.h>
#include <stdio.h>

static bool Near(double a, double b) {
    return fabs(a - b) < 1e-9;
}

int main() {
    double angle = 42;
    const char* invalid[] = {"", "-", ".", "nan", "inf", "1e2", "3x", "180.01", "-180.01", "1 2"};
    for (const char* value : invalid) {
        assert(!PdfParseAdjustmentAngle(value, &angle));
        assert(angle == 42);
    }
    assert(PdfParseAdjustmentAngle(" -0.01 ", &angle) && Near(angle, -0.01));
    assert(PdfParseAdjustmentAngle("+.25", &angle) && Near(angle, .25));
    assert(PdfParseAdjustmentAngle("180", &angle) && angle == 180);
    PdfPageAdjustment state;
    state.SetDeskew(.01);
    state.RotateBy(90);
    assert(Near(state.TotalAngle(), 90.01));
    state.RotateBy(90);
    assert(Near(state.TotalAngle(), -179.99));
    state.RotateBy(90);
    assert(Near(state.TotalAngle(), -89.99));
    state.RotateBy(90);
    assert(Near(state.TotalAngle(), .01));
    for (int i = 0; i < 4; i++) state.RotateBy(-90);
    assert(Near(state.TotalAngle(), .01));
    state.RotateBy(-90);
    assert(Near(state.TotalAngle(), -89.99));
    state.RotateBy(90);
    assert(Near(state.TotalAngle(), .01));
    state.RotateBy(180);
    assert(Near(state.TotalAngle(), -179.99));
    state.RotateBy(180);
    assert(Near(state.TotalAngle(), .01));
    assert(Near(state.deskew, .01));
    for (int i = 0; i < 100; i++) state.SetDeskew(state.deskew + .05);
    assert(Near(state.deskew, 5.01));
    state.SetDeskew(200);
    assert(state.deskew == 180);
    state.SetDeskew(-200);
    assert(state.deskew == -180);
    assert(PdfAdjustmentHoldTravel(.249) == 0);
    assert(PdfAdjustmentHoldTravel(.25) == 0);
    assert(Near(PdfAdjustmentHoldTravel(1.75), 1.575));
    assert(Near(PdfAdjustmentHoldTravel(2.75), 3.375));
    double accumulated = 0, previous = 0;
    for (int i = 1; i <= 300; i++) {
        double value = PdfAdjustmentHoldTravel(i * .01);
        assert(value >= previous);
        accumulated += value - previous;
        previous = value;
    }
    assert(Near(accumulated, PdfAdjustmentHoldTravel(3)));
    double leftSpeed = (PdfAdjustmentHoldTravel(1.75) - PdfAdjustmentHoldTravel(1.74999)) / .00001;
    double rightSpeed = (PdfAdjustmentHoldTravel(1.75001) - PdfAdjustmentHoldTravel(1.75)) / .00001;
    assert(fabs(leftSpeed - rightSpeed) < .0001);
    puts("PdfPageAdjustment: parsing, precision, composition and continuous hold tests passed");
}
