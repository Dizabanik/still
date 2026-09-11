fn input(i: usize) -> i64 { std::env::args().nth(i).unwrap().parse().unwrap() }

fn main() {
    let rounds=input(1); let mut h=input(2);
    for r in 0..rounds { h=((h*31)^r)&2147483647; }
    println!("{}",h);
}
