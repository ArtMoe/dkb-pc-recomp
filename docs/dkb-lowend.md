# Running on a weak PC

Measurements from `tools/lowend_bench.py`, which runs `dkb.exe` on the first N logical processors with the perf trace and
the draw profile on, and prints one table. This page is the baseline the low-end work is compared against.

## Baseline

Bananza 1.0.0, first level, the scripted input of about 2.4 minutes, 70 s per run, RTX 5060 Ti, 16 GB of RAM, before the
descriptor memos of pull request 181. This route is light: it does not show the cost of an open area (a 3070 Ti Laptop
reports 31 to 35 fps there with about 8,300 draws a frame), so treat it as a floor for the CPU limit, not a ceiling.

| Logical processors | Mean fps | Steady fps | Worst window (ms) | Replay avg (ms) | GPU avg (ms) | Commit max (MB) | Working set max (MB) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 16 | 57.0 | 60.0 | 941 | 3.89 | 1.80 | 14,306 | 5,837 |
| 8 | 55.9 | 60.0 | 938 | 6.36 | 3.02 | 14,133 | 5,704 |
| 4 | 52.7 | 58.9 | 997 | 6.61 | 3.11 | 14,087 | 5,564 |
| 2 | 42.9 | 55.6 | 1,249 | 13.12 | 1.87 | 14,075 | 5,636 |

- Down to 4 logical processors the steady rate holds near 60 fps. At 2 it falls to about 56 fps and the replay thread
  takes 13 ms a frame.
- The worst window is about one second at every count. It falls in the first seconds, where shaders are translated.
- Process commit is 14 GB at every count, so memory, not the CPU, is what a 16 GB machine runs out of first.

## Where the commit goes

`TOTK_MEM_REPORT=N` logs the breakdown every N frames and at exit. Around frame 2400 of the first level, of about 14.4 GiB:

- guest heap and stack: 9.2 GiB, of which only about 4.2 GiB is resident in the working set;
- GPU device-local memory and mirrors: 2.6 to 5.1 GiB, growing during the run;
- the exe image: about 0.5 GiB.

`TOTK_APP_POOL` below 8 GiB makes the game crash at start (access violation reading 0xc8), so it is not a way to save memory.
