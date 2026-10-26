#!/bin/sh
# Force feedback test for any wheel Linux offers force feedback for (on the
# HORI FFB Racing Wheel, run hori_wheel_on first). Shows the wheel it found,
# then plays weak effects (20% at most) and reports each as PASS, FAIL or
# "not supported". Let go of the wheel: it will swing to its stops.
echo "Force feedback test in 5 seconds. Let go of the wheel."
sleep 5
/media/fat/linux/ffb-wheel-test
echo
echo "Done."
sleep 15
