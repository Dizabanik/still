fn input(i: usize) -> i64 { std::env::args().nth(i).unwrap().parse().unwrap() }

fn main() {
    let rounds=input(1); let seed=input(2); let mut m=[0i64;16]; let mut v=[0i64;4]; let mut acc=0;
    for i in 0..16 { m[i]=((i as i64)*7+seed)&15; }
    for i in 0..4 { v[i]=(seed+(i as i64))&65535; }
    for r in 0..rounds { let mut out=[0i64;4];
        for row in 0..4 { let mut sum=0;
            for col in 0..4 { sum+=m[row*4+col]*v[col]; }
            out[row]=(sum+r)&65535;
        }
        for i in 0..4 { v[i]=out[i]; acc=(acc*31+v[i])&2147483647; }
    }
    println!("{} {} {}",acc,v[0],v[3]);
}
