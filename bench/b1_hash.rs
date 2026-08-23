fn fnv1a(h: u32, v: u32) -> u32 {
    (h ^ v).wrapping_mul(16777619)
}

fn main() {
    const ROUNDS: i64 = 200_000_000;
    let mut h: u32 = 2166136261;
    let mut r: i64 = 0;
    while r < ROUNDS {
        h = fnv1a(h, r as u32);
        r += 1;
    }
    println!("h={}", h);
}
