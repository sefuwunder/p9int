#!/bin/sh
# Build mail-sync and its test suite. Plain cc + OpenSSL, no configure.
# (No Makefile: this repo's .gitignore ignores generated Makefiles.)
set -e
cd "$(dirname "$0")"
cc -O2 -Wall -Wextra -o mail-sync mail-sync.c util.c json.c goa.c imap.c mbox.c -lssl -lcrypto
cc -O2 -Wall -Wextra -o tests/test tests/test.c util.c json.c goa.c imap.c mbox.c -lssl -lcrypto
./tests/test
