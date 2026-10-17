# 15 — Sources and how they were used

- **psx-spx (nocash)**: primary public documentation of CPU, memory map, BIOS functions, GPU, GTE, CD-ROM, MDEC, SPU, DMA, memory card. Used as the
  specification; where it says "approximate" (MDEC IDCT) an independent implementation was consulted.
- **DuckStation source** (`cpu_core.cpp`, `gte.cpp`, `mdec.cpp`, `dma.cpp`), downloaded to a git-ignored directory and *read or built locally as a
  test oracle*; its licence forbids derivatives, so none of it is copied or paraphrased line by line here. Used to settle: load-delay cancellation,
  REGIMM link rule, exception PC in delay slots, GTE results (differential test), MDEC rounding/orientation, DMA enable rule.
- **Ghidra** (via an MCP reverse-engineering tool) for identifying library layers and structures in binaries; raw-word scans where its decoding is wrong.
- **AnyPS5**: structure/method inspiration (static knowledge → translator → verification), not code.

Rule for contributors: facts go in with a tag and a way to re-check; third-party code does not.
