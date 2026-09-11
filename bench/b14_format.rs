use std::io::{stdout, BufWriter, Write};

fn main() {
    const N: i64 = 1_000_000;
    let out = stdout();
    let mut handle = BufWriter::new(out.lock());
    let mut acc: i64 = 0;
    for i in 0..N {
        acc = (acc.wrapping_mul(31) + i) & 1048575;
        let val = (i % 1000) as f64 * 0.001;
        let _ = writeln!(handle, "i={} acc={} val={:.2}", i, acc, val);
    }
}
