// Rust twin of bench2.wky: hash-chain over a 256-entry table.
static DATA_INIT: [u32; 256] = {
    let mut d = [0u32; 256];
    let mut i = 0usize;
    while i < 256 {
        d[i] = (i as u32).wrapping_mul(2654435761);
        i += 1;
    }
    d
};

#[inline]
fn hash_step(h: u32, v: u32) -> u32 {
    h.wrapping_mul(31).wrapping_add(v)
}

fn main() {
    let data = DATA_INIT;
    let mut total: u64 = 0;
    for _ in 0..3_000_000u64 {
        let mut h: u32 = 2166136261;
        for i in 0..256 {
            h = hash_step(h, data[i]);
        }
        total += h as u64;
    }
    println!("{}", total);
}
