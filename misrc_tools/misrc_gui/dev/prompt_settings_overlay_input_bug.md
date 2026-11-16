# Settings overlay input leakage

## Symptom and change

While Settings was open, mouse input could reach the waveform and FFT
panels behind it. The shared panel dispatcher now suppresses clicks and
scrolling while Settings is open. The FFT and waveform also clear an
active drag on render, and the waveform suppresses its middle-button reset.

## Verification

The separate Windows x64 Settings validation run reported 364 panel-dispatch
checks passing, a native UCRT64 Release build, and a GUI smoke run using the
synthetic Test Signal device. The result covers panel input routing and
synthetic GUI operation. Hardware capture was outside that validation.

Harry's sign-off on the Settings interaction change is pending.
