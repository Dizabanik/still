#!/bin/bash

make
rm output.bc
rm output.ll
./kawac tests/test.kawa

lli output.bc
