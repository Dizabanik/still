fn input(i: usize) -> i64 { std::env::args().nth(i).unwrap().parse().unwrap() }

fn main() {
    let rounds=input(1); let seed=input(2); let n=input(3) as usize;
    let mut data=[0i64;4096];
    for i in 0..4096 { data[i]=((i as i64)+seed)&65535; }
    let view=&mut data[..n]; let mut acc=0;
    // Sequential indexed access expresses the same overlap without conflicting &mut borrows.
    for r in 0..rounds {
        let i=(r%((n-1) as i64)) as usize;
        view[i+1]=(view[i]*3+view[i+1]+r)&65535;
        acc=(acc+view[i+1])&2147483647;
    }
    println!("{} {}",acc,view[n-1]);
}
