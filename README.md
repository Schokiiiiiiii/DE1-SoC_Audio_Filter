# DE1-SoC_Audio_Filter

## Introduction

The DE1-SoC board allows audio samples to be acquired through the **“MIC IN”** connector. The samples have the following format:

* Stereo sampling: two channels
* Sampling rate: 48 kHz
* Buffer of 128 samples per channel
* One sample is 16-bit data

The goal is to create a **real-time audio filter**. The application receives an audio input signal, applies processing to it, and outputs the processed signal.

An interface, such as a CLI, buttons, or another control method, must allow the user to select the type of processing applied to the signal, for example amplitude adjustment, low-pass filter, high-pass filter, sine-to-cosine conversion, etc. Several processing modes must be supported.

The latency introduced by the processing must be measured and analyzed, especially under overload conditions. It is important to ensure that the system can handle processing deadlines while maintaining acceptable audio quality, even when the resources are under maximum load.

For this project, it is recommended to use a signal generator to generate the input signal and an oscilloscope to visualize the output signal.
