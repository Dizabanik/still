#[inline(always)]
fn lcg_step(x: u64, y: u64) -> (u64, u64) {
    let nx = (x.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407)) & 9223372036854775807;
    let ny = (y.wrapping_mul(2862933555777941757).wrapping_add(3037000493)) & 9223372036854775807;
    (nx, ny)
}

#[inline(always)]
fn mix(p: (u64, u64)) -> (u64, u64) {
    let q = p.0 / 65536;
    let r = p.1 % 65536;
    (p.0 ^ r, (p.1.wrapping_add(q)) & 9223372036854775807)
}

fn main() {
    const ROUNDS: i64 = 20000000;
    let (mut x, mut y) = (123456789u64, 987654321u64);
    let mut acc: u64 = 0;
    for _ in 0..ROUNDS {
        let (nx, ny) = lcg_step(x, y);
        let (mx, my) = mix((nx, ny));
        x = mx;
        y = my;
        acc = (acc.wrapping_add(x).wrapping_add(y)) & 2147483647;
    }
    println!("acc={} x={} y={}", acc, x, y);
}
