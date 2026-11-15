#!/bin/bash
# Build and run COWBELL's sequencer tests natively (WSL Ubuntu's gcc). Argument: run only tests whose name has it.
set -e
cd "$(dirname "$0")"
mkdir -p ../out/tests
gcc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -fsanitize=address,undefined \
    -I . -I ../cowbell -o ../out/tests/seq_test seq_test.c ../cowbell/art.c
../out/tests/seq_test "$@"
