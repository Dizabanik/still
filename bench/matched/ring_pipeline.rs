fn input(i: usize) -> i64 { std::env::args().nth(i).unwrap().parse().unwrap() }

fn main() {
    let rounds=input(1); let seed=input(2); let mut a=[0i64;64]; let mut b=[0i64;64]; let mut c=[0i64;64];
    let mut sent=0; let mut acc=0;
    while sent<rounds {
        let count=(rounds-sent).min(64) as usize;
        for i in 0..count { a[i]=((sent+(i as i64)+seed)*3+1)&2147483647; }
        for i in 0..count { b[i]=(a[i]*3+1)&2147483647; }
        for i in 0..count { c[i]=(b[i]*3+1)&2147483647; }
        for i in 0..count { acc=(acc+c[i])&2147483647; }
        sent+=count as i64;
    }
    println!("{}",acc);
}
