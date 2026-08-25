// Rust twin of b7_errors.kawa: the idiomatic Result pattern. validate()
// returns Ok(()) or Err(ParseErr) -- what Kawa's filter/press/dregs lowers
// to, written the way a Rust programmer would.
const N: i64 = 200_000_000;

#[derive(Clone, Copy)]
struct ParseErr {
    code: i32,
    pos: i32,
}

#[inline]
fn validate(i: i64) -> Result<(), ParseErr> {
    let x = (i as u64).wrapping_mul(2654435761) as u32;
    if x % 7 == 3 {
        return Err(ParseErr { code: 1, pos: (x % 1000) as i32 });
    }
    if (x & 0xFF) == 0xFF {
        return Err(ParseErr { code: 2, pos: (x >> 8) as i32 });
    }
    Ok(())
}

fn main() {
    let mut ok_count: i64 = 0;
    let mut acc: i32 = 0;
    let mut i: i64 = 0;
    while i < N {
        let x = (i as u64).wrapping_mul(2654435761) as u32;
        match validate(i) {
            Ok(()) => {
                acc = acc.wrapping_add(x as i32);
                ok_count += 1;
            }
            Err(e) => {
                acc = acc.wrapping_sub(e.code.wrapping_mul(e.pos));
            }
        }
        i += 1;
    }
    println!("ok={} acc={}", ok_count as i32, acc);
}
