fn input(i: usize) -> i64 { std::env::args().nth(i).unwrap().parse().unwrap() }

fn main() {
    let rounds=input(1); let mut state=input(2); let n=input(3) as usize;
    let mut data=[0i64;4096];
    for i in 0..4096 { data[i]=((i as i64)*17+state)&65535; }
    let view=&mut data[..n]; let mut acc=0i64;
    for r in 0..rounds {
        state=(state*1103515245+12345)&2147483647;
        let index=(state%(n as i64)) as usize;
        view[index]=(view[index]*3+r)&65535;
        acc=(acc+view[index])&2147483647;
    }
    println!("{} {}",acc,state);
}
