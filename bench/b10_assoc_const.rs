struct Mat4;

impl Mat4 {
    const DIM: usize = 4;
    const SIZE: usize = 16;
}

fn main() {
    const ROUNDS: i64 = 20_000_000;
    let mut m = [0i64; Mat4::SIZE];
    for i in 0..Mat4::SIZE {
        m[i] = ((i * 7 + 3) & 15) as i64;
    }

    let mut v = [1i64, 2, 3, 4];

    let mut acc: i64 = 0;
    for r in 0..ROUNDS {
        let mut out = [0i64; Mat4::DIM];
        for row in 0..Mat4::DIM {
            let mut sum: i64 = 0;
            for col in 0..Mat4::DIM {
                sum += m[row * Mat4::DIM + col] * v[col];
            }
            out[row] = sum;
        }
        v[0] = (out[0] ^ r) & 65535;
        v[1] = out[1] & 65535;
        v[2] = (out[2] + 1) & 65535;
        v[3] = (out[3] ^ (r >> 2)) & 65535;
        acc = (acc.wrapping_mul(31) + v[0] + v[1] + v[2] + v[3]) & 2147483647;
    }

    println!("acc={}", acc);
}
