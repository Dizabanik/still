// Rust twin of b5_strings.wky: tokenize a fixed corpus in place, FNV-1a
// each word over raw bytes, fold into an accumulator, content-compare every
// 16th word. Words are (start,len) slices of one static byte buffer -- the
// manual analogue of Whisky's fat str views.
static CORPUS: &[u8] = b"the quick brown fox jumps over the lazy dog the quick brown fox pack my box with five dozen liquor jugs pack my box with five how vexingly quick daft zebras jump how vexingly quick daft zebras sphinx of black quartz judge my vow sphinx of black quartz judge ";

#[inline]
fn fnv(w: &[u8]) -> u32 {
    let mut h: u32 = 2166136261;
    for &b in w {
        h = (h ^ b as u32).wrapping_mul(16777619);
    }
    h
}

fn main() {
    const ROUNDS: i64 = 2_000_000;
    let cd = CORPUS;
    let clen = cd.len() as u32;
    let mut acc: u64 = 0;
    let mut words: u32 = 0;
    for r in 0..ROUNDS {
        let mut i: u32 = 0;
        while i < clen {
            while i < clen && cd[i as usize] == b' ' {
                i += 1;
            }
            let start = i;
            while i < clen && cd[i as usize] != b' ' {
                i += 1;
            }
            let wlen = (i - start) as usize;
            if wlen > 0 {
                acc += fnv(&cd[start as usize..i as usize]) as u64;
                words += 1;
                if (words & 15) == 0 && &cd[start as usize..i as usize] == b"quick" {
                    acc += 7;
                }
            }
        }
        acc += r as u64; // defeat any whole-loop elision
    }
    println!("acc={} words={}", acc, words);
}
