#!/usr/bin/env python3
"""Independent reference vectors. --check rejects stale checked-in oracles."""
import argparse
import json
from pathlib import Path
import random

ROOT=Path(__file__).resolve().parents[1]


def expected(seed,count):
    def next_value(value): return (value*1664525+1013904223) % (1 << 32)
    data=[]
    for i in range(17):
        seed=next_value(seed ^ i)
        data.append(seed)
    checksum=0
    for _ in range(count):
        seed=next_value(seed)
        index=seed % 17
        data[index]=next_value(data[index] ^ seed)
        checksum ^= data[index]
    return f'{checksum} {seed}\n'


def document():
    rng=random.Random(20260911)
    seeds=[0,1,127,65535,2147483647,2147483648,4294967295]+[rng.randrange(1 << 32) for _ in range(9)]
    counts=[0,1,16,17,18,100,257,3,31,32,63,64,65,511,512,1025]
    return {'kind':'run','inputs':[{'args':[str(s),str(n)],'stdout':expected(s,n)} for s,n in zip(seeds,counts)]}


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--check',action='store_true')
    args=ap.parse_args()
    path=ROOT/'tests/contracts/runtime_integer_stream.kawa.json'
    rendered=json.dumps(document(),indent=2)+'\n'
    if args.check:
        if path.read_text()!=rendered: raise SystemExit('stale property vectors: rerun scripts/generate_property_cases.py')
    else:
        path.write_text(rendered)


if __name__=='__main__': main()
