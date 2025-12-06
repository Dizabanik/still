#!/bin/bash

make
rm output.bc
rm output.ll
./kawac test.kawa

lli output.bc
