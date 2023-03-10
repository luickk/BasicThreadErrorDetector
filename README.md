# Basic Thread Error Detector based on DynamoRIO

The project is split into `instrument.c` which only contains purely technical (memory instrumentation, fn wrapping..) DynamorRIO api setup/config and `race_detector.c` which contains the race detector implementation described in [this](https://storage.googleapis.com/pub-tools-public-publication-data/pdf/35604.pdf) paper.
This project is a simplified but accurate representation of the, in section 4.3, described "precise, slow" detection mode.
All events are "exported" in instrument.h and implemented by race_detector.c (fed to DynamoRIO by `instrument.c`).

The detector is fully build on a clock-vector before/after relations mechanism to detect races.

## Clock Vector Based Race Detection

Clock Vector Based detection can get very complex very quickly, thus this implementation doesn't quantities memory accesses but instead check after every access, thus the clock vector doesn't need to contain accurate timestamps(instead a memory access counter is used). This isn't very performant and the slowest, but also most simple method.

The mathematical operators and logic is described in [this](https://dl.acm.org/doi/pdf/10.1145/3018610.3018611)(section 3.12) paper quite well.

## Rough implementation summary

Most of the race detectors code comes down to collecting and preperation of data. The detector has per thread data and a global allocations array(which stores context information about allocated memory that has to be checked). The per thread data contains sets that store all reads/ writes relating to allocated memory as well as all lock accesses and states. Checks are performed on every memory access to allocated memory. 