# read_training

Small C project for core functions that can be reused from another codebase.

## Layout

- `include/read_training.h`: public API
- `src/read_training.c`: implementation
## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Add Core Functions

Add public declarations to `include/read_training.h`, then implement them in
`src/read_training.c`. Keep company-specific integration code outside this
library boundary when possible.
