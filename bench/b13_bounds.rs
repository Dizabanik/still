fn main() {
    const ROUNDS: i64 = 50_000;
    let mut buf = [0i64; 1024];
    for i in 0..1024 {
        buf[i] = ((i * 19 + 7) & 65535) as i64;
    }

    let mut acc: i64 = 0;
    for r in 0..ROUNDS {
        for i in 0..1024 {
            buf[i] = (buf[i] * 3 + (r & 15)) & 65535;
            acc = (acc + buf[i]) & 2147483647;
        }
        acc = (acc ^ buf[0] ^ buf[1023]) & 2147483647;
    }

    println!("acc={}", acc);
}
