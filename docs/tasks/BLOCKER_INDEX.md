# Emulator Blocker Tasks - Overview

These 4 tasks can be executed in parallel by separate agents. Each task is self-contained with no dependencies on the others.

## Task Summary

| #   | Task                                                               | Priority | Complexity | Files Modified        |
| --- | ------------------------------------------------------------------ | -------- | ---------- | --------------------- |
| 1   | [LZX Decompression](BLOCKER_01_LZX_DECOMPRESSION.md)               | CRITICAL | Medium     | 4 files + new library |
| 2   | [Time Base Register](BLOCKER_02_TIME_BASE_REGISTER.md)             | HIGH     | Low        | 3-4 files             |
| 3   | [Shader Loops/Predication](BLOCKER_03_SHADER_LOOPS_PREDICATION.md) | HIGH     | High       | 4 files               |
| 4   | [Alertable Waits](BLOCKER_04_ALERTABLE_WAITS.md)                   | MEDIUM   | Medium     | 5 files               |

## Parallel Execution

All tasks are independent and can be worked on simultaneously:

```
┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐
│    Agent 1      │  │    Agent 2      │  │    Agent 3      │  │    Agent 4      │
│                 │  │                 │  │                 │  │                 │
│  BLOCKER_01     │  │  BLOCKER_02     │  │  BLOCKER_03     │  │  BLOCKER_04     │
│  LZX Decomp     │  │  Time Base      │  │  Shader Loops   │  │  Alertable      │
│                 │  │                 │  │                 │  │  Waits          │
└────────┬────────┘  └────────┬────────┘  └────────┬────────┘  └────────┬────────┘
         │                    │                    │                    │
         └────────────────────┴────────────────────┴────────────────────┘
                                       │
                                       ▼
                              ┌─────────────────┐
                              │   Merge All     │
                              │   & Test        │
                              └─────────────────┘
```

## How to Use

For each task file, simply:

1. Open a new Cursor agent window
2. Drag and drop the task markdown file
3. Tell the agent: "Implement this"

## Impact After Completion

| Before                                | After                                 |
| ------------------------------------- | ------------------------------------- |
| LZX-compressed XEX files fail to load | All XEX compression types supported   |
| Games hang in timing loops            | Proper time-based execution           |
| Complex shaders produce garbage       | Loops and conditionals work correctly |
| Async callbacks never fire            | Full async I/O support                |

## Testing After Merge

Once all tasks are complete:

1. Run the test suite: `./build.sh test`
2. Try loading a game that previously failed
3. Check logs for:
   - "LZX decompression successful"
   - Time base incrementing
   - "LOOP_START" / "LOOP_END" in shader logs
   - "Executing APC" in kernel logs

## File Conflict Notes

The tasks modify different files, so there should be no merge conflicts:

- **Task 1** (LZX): `xex_crypto.cpp/h`, `CMakeLists.txt`, `third_party/`
- **Task 2** (Time Base): `cpu.h`, `interpreter.cpp`, `jit_compiler.cpp`
- **Task 3** (Shaders): `shader_translator.cpp/h`, `spirv_builder.cpp/h`
- **Task 4** (APCs): `xthread.cpp/h`, `xboxkrnl_threading.cpp`, `types.h`
