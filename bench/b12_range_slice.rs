#[inline(always)]
fn sum_slice(s: &[i64]) -> i64 {
    let mut total = 0;
    for &v in s {
        total += v;
    }
    total
}

#[inline(always)]
fn transform_slice(s: &mut [i64], delta: i64) {
    for i in 0..s.len() {
        s[i] = (s[i] * 3 + delta) & 65535;
    }
}

fn main() {
    const ROUNDS: i64 = 50_000;
    let mut buf = [0i64; 1024];
    for i in 0..1024 {
        buf[i] = ((i * 17 + 5) & 65535) as i64;
    }

    let mut acc: i64 = 0;
    for r in 0..ROUNDS {
        let start = ((r * 7) & 511) as usize;
        let len = (((r * 13) & 255) + 32) as usize;
        let end = start + len;

        let s = sum_slice(&buf[start..end]);
        transform_slice(&mut buf[start..end], r & 7);

        acc = (acc.wrapping_mul(37) + s) & 2147483647;
    }

    println!("acc={}", acc);
}
