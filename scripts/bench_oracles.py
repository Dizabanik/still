"""Independent, arbitrary-precision correctness models; never use a compiler twin as truth."""
MASK = (1 << 31) - 1


def oracle(name, args):
    rounds, seed = args[:2]
    if not 0 <= rounds <= 100_000_000 or not 0 <= seed <= 65535:
        raise ValueError("input exceeds the benchmark's proven arithmetic domain")
    n = args[2] if len(args) == 3 else None
    if n is not None and not 1 <= n <= 4096:
        raise ValueError("active length must be 1..4096")
    if name == "runtime_mix":
        h = seed
        for r in range(rounds):
            h = ((h*31)^r)&MASK
        values = [h]
    elif name == "dynamic_gather":
        data = [(i*17+seed)&65535 for i in range(n)]
        state, acc = seed, 0
        for r in range(rounds):
            state=(state*1103515245+12345)&MASK
            index=state%n
            data[index]=(data[index]*3+r)&65535
            acc=(acc+data[index])&MASK
        values=[acc,state]
    elif name == "indexed_graph":
        edges=[(i*17+1)%n for i in range(n)]
        data=[(i+seed)&65535 for i in range(n)]
        cursor,acc=seed%n,0
        for r in range(rounds):
            cursor=edges[cursor]
            data[cursor]=(data[cursor]+acc+r)&65535
            acc=(acc*31+data[cursor])&MASK
        values=[acc,cursor]
    elif name == "overlap_views":
        if n < 2:
            raise ValueError("overlap requires at least two elements")
        data=[(i+seed)&65535 for i in range(n)]
        acc=0
        for r in range(rounds):
            i=r%(n-1)
            data[i+1]=(data[i]*3+data[i+1]+r)&65535
            acc=(acc+data[i+1])&MASK
        values=[acc,data[-1]]
    elif name == "matrix4":
        matrix=[[( (row*4+col)*7+seed)&15 for col in range(4)] for row in range(4)]
        vector=[(seed+i)&65535 for i in range(4)]
        acc=0
        for r in range(rounds):
            vector=[(sum(a*b for a,b in zip(row,vector))+r)&65535 for row in matrix]
            for v in vector:
                acc=(acc*31+v)&MASK
        values=[acc,vector[0],vector[3]]
    elif name == "ring_pipeline":
        # Closed form independent of ring scheduling; three affine transforms give 27*x+13.
        values=[(27*(rounds*seed+rounds*(rounds-1)//2)+13*rounds)&MASK]
    else:
        raise ValueError("unknown oracle: " + name)
    return (" ".join(map(str,values))+"\n").encode()
